/**
 *  Copyright (C) 2024  PiP Project
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

#include "sender.h"
#include "../airplay/ed25519/sha512.h"
#include "../airplay/byteutils.h"
#include "discovery.h"
#include "http_client.h"
#include "pairing_client.h"
#include "fairplay_client.h"
#include "stream_client.h"
#include "ntp_client.h"
#include "video_encoder.h"
#include "video_packetizer.h"
#include "frame_capture.h"
#include "audio_capture.h"
#include "audio_encoder.h"
#include "rtp_audio.h"

struct sender_s {
  sender_state_t state;
  sender_state_callback_t state_callback;
  void *state_callback_ctx;

  // Components
  http_client_t *http_client;
  pairing_client_t *pairing_client;
  fairplay_client_t *fairplay_client;
  stream_client_t *stream_client;
  ntp_client_t *ntp_client;
  video_encoder_t *video_encoder;
  video_packetizer_t *video_packetizer;
  frame_capture_t *frame_capture;
  audio_capture_t *audio_capture;
  audio_encoder_t *audio_encoder;
  rtp_audio_t *rtp_audio;

  // Connection info
  char *receiver_host;
  uint16_t receiver_port;
  stream_info_t stream_info;

  // FairPlay session key (16 bytes)
  unsigned char fairplay_session_key[16];
  int fairplay_initialized;

  // Streaming state
  int streaming;
  void *source_id;

  // Video AES key and IV (derived from stream connection ID)
  unsigned char video_aes_key[16];
  unsigned char video_aes_iv[16];
  int video_encryption_initialized;
};

static void
sender_set_state(sender_t *s, sender_state_t new_state, const char *error)
{
  assert(s);

  if (s->state == new_state) {
    return;
  }

  s->state = new_state;

  if (s->state_callback) {
    s->state_callback(new_state, error, s->state_callback_ctx);
  }
}

static void
cleanup_components(sender_t *s)
{
  if (s->rtp_audio) {
    rtp_audio_destroy(s->rtp_audio);
    s->rtp_audio = NULL;
  }

  if (s->audio_encoder) {
    audio_encoder_destroy(s->audio_encoder);
    s->audio_encoder = NULL;
  }

  if (s->audio_capture) {
    audio_capture_destroy(s->audio_capture);
    s->audio_capture = NULL;
  }

  if (s->frame_capture) {
    frame_capture_destroy(s->frame_capture);
    s->frame_capture = NULL;
  }

  if (s->video_packetizer) {
    video_packetizer_destroy(s->video_packetizer);
    s->video_packetizer = NULL;
  }

  if (s->video_encoder) {
    video_encoder_destroy(s->video_encoder);
    s->video_encoder = NULL;
  }

  if (s->ntp_client) {
    ntp_client_destroy(s->ntp_client);
    s->ntp_client = NULL;
  }

  if (s->stream_client) {
    stream_client_disconnect_video(s->stream_client);
    stream_client_disconnect_feedback(s->stream_client);
    stream_client_destroy(s->stream_client);
    s->stream_client = NULL;
  }

  if (s->fairplay_client) {
    fairplay_client_destroy(s->fairplay_client);
    s->fairplay_client = NULL;
  }

  if (s->pairing_client) {
    pairing_client_destroy(s->pairing_client);
    s->pairing_client = NULL;
  }

  if (s->http_client) {
    http_client_disconnect(s->http_client);
    http_client_destroy(s->http_client);
    s->http_client = NULL;
  }

  s->fairplay_initialized = 0;
  s->streaming = 0;
}

sender_t *
sender_init(void)
{
  sender_t *s;

  s = calloc(1, sizeof(sender_t));
  if (!s) {
    return NULL;
  }

  s->state = SENDER_STATE_IDLE;
  s->state_callback = NULL;
  s->state_callback_ctx = NULL;
  s->http_client = NULL;
  s->pairing_client = NULL;
  s->fairplay_client = NULL;
  s->stream_client = NULL;
  s->ntp_client = NULL;
  s->video_encoder = NULL;
  s->video_packetizer = NULL;
  s->frame_capture = NULL;
  s->audio_capture = NULL;
  s->audio_encoder = NULL;
  s->rtp_audio = NULL;
  s->receiver_host = NULL;
  s->receiver_port = 0;
  s->fairplay_initialized = 0;
  s->streaming = 0;
  s->source_id = NULL;
  s->video_encryption_initialized = 0;

  return s;
}

// Callback: Encoded video frame -> packetizer
static void
on_encoded_frame(uint8_t *data, int len, bool is_keyframe,
                uint8_t *sps, int sps_len, uint8_t *pps, int pps_len,
                uint64_t pts, void *ctx)
{
  sender_t *s = (sender_t *)ctx;
  uint64_t ntp_timestamp;

  if (!s || !s->video_packetizer || !s->ntp_client) {
    return;
  }

  // Convert local timestamp to NTP timestamp
  ntp_timestamp = ntp_client_convert_to_ntp(s->ntp_client, pts);

  // Packetize and send
  video_packetizer_packetize(s->video_packetizer, data, len, is_keyframe,
                             sps, sps_len, pps, pps_len, ntp_timestamp);
}

// Callback: Packetized video data -> stream client
static void
on_packetized_video(const uint8_t *packet, int packet_len, void *ctx)
{
  sender_t *s = (sender_t *)ctx;

  if (!s || !s->stream_client || packet_len < 128) {
    return;
  }

  // Send raw packet (header + encrypted payload) directly via stream client
  stream_client_send_raw_video_packet(s->stream_client, packet, packet_len);
}

// Callback: Captured frame -> video encoder
static void
on_captured_frame(uint8_t *rgba_data, int width, int height, int stride,
                 uint64_t pts, void *ctx)
{
  sender_t *s = (sender_t *)ctx;

  if (!s || !s->video_encoder) {
    return;
  }

  video_encoder_encode_frame(s->video_encoder, rgba_data, stride, pts);
}

// Callback: Encoded audio -> RTP
static void
on_encoded_audio(uint8_t *data, int data_len, uint64_t pts, void *ctx)
{
  sender_t *s = (sender_t *)ctx;

  if (!s || !s->rtp_audio) {
    return;
  }

  rtp_audio_send(s->rtp_audio, data, data_len, pts);
}

// Callback: Captured audio samples -> audio encoder
static void
on_captured_audio(float *samples, int num_frames, int channels,
                 int sample_rate, uint64_t pts, void *ctx)
{
  sender_t *s = (sender_t *)ctx;

  if (!s || !s->audio_encoder) {
    return;
  }

  audio_encoder_encode(s->audio_encoder, samples, num_frames, pts);
}

void
sender_set_state_callback(sender_t *s, sender_state_callback_t cb, void *ctx)
{
  assert(s);
  s->state_callback = cb;
  s->state_callback_ctx = ctx;
}

int
sender_connect(sender_t *s, airplay_receiver_t *receiver,
               const char *device_id, const char *os_name,
               const char *os_version, const char *model, const char *name)
{
  unsigned char shared_secret[32];
  unsigned char fairplay_session_key[32];
  unsigned char audio_aes_key[16];
  stream_info_t stream_info;

  assert(s);
  assert(receiver);
  assert(device_id);
  assert(os_name);
  assert(os_version);
  assert(model);

  if (s->state != SENDER_STATE_IDLE) {
    return -1;
  }

  sender_set_state(s, SENDER_STATE_CONNECTING, NULL);

  // Initialize HTTP client
  s->http_client = http_client_init(receiver->host, receiver->port);
  if (!s->http_client) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to initialize HTTP client");
    return -1;
  }

  if (http_client_connect(s->http_client) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to connect to receiver");
    cleanup_components(s);
    return -1;
  }

  // Store receiver info
  s->receiver_host = strdup(receiver->host);
  s->receiver_port = receiver->port;

  // Step 1: Pair-Setup
  sender_set_state(s, SENDER_STATE_PAIRING, NULL);

  s->pairing_client = pairing_client_init();
  if (!s->pairing_client) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to initialize pairing client");
    cleanup_components(s);
    return -1;
  }

  if (pairing_client_setup(s->pairing_client, s->http_client) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Pair-setup failed");
    cleanup_components(s);
    return -1;
  }

  // Step 2: Pair-Verify
  if (pairing_client_verify_step1(s->pairing_client, s->http_client) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Pair-verify step 1 failed");
    cleanup_components(s);
    return -1;
  }

  if (pairing_client_verify_step2(s->pairing_client, s->http_client) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Pair-verify step 2 failed");
    cleanup_components(s);
    return -1;
  }

  // Get shared secret for FairPlay
  if (pairing_client_get_shared_secret(s->pairing_client, shared_secret) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to get shared secret");
    cleanup_components(s);
    return -1;
  }

  // Step 3: FairPlay Setup
  s->fairplay_client = fairplay_client_init();
  if (!s->fairplay_client) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to initialize FairPlay client");
    cleanup_components(s);
    return -1;
  }

  if (fairplay_client_setup(s->fairplay_client, s->http_client) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "FairPlay setup failed");
    cleanup_components(s);
    return -1;
  }

  if (fairplay_client_handshake(s->fairplay_client, s->http_client) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "FairPlay handshake failed");
    cleanup_components(s);
    return -1;
  }

  // Get FairPlay session key (this will be used to derive audio AES key)
  if (fairplay_client_get_session_key(s->fairplay_client, fairplay_session_key) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to get FairPlay session key");
    cleanup_components(s);
    return -1;
  }

  // For now, use FairPlay session key as audio AES key
  // In a full implementation, this would be derived from the FairPlay handshake
  memcpy(audio_aes_key, fairplay_session_key, 16);
  s->fairplay_initialized = 1;
  memcpy(s->fairplay_session_key, fairplay_session_key, 16);

  // Step 4: Stream Setup
  s->stream_client = stream_client_init();
  if (!s->stream_client) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to initialize stream client");
    cleanup_components(s);
    return -1;
  }

  // Step 4: Stream Setup
  // Our receiver doesn't support POST /stream, so we skip it and use RTSP SETUP directly
  // Generate a stream connection ID (receiver will use this for AES key derivation)
  // The stream_client_setup_rtsp function will use client->info.stream_connection_id
  // We need to set it first - use a helper function or set it directly
  // For now, we'll let stream_client_setup_rtsp generate it if it's 0

  // Step 4a: GET /info RTSP/1.0 (iPad does this before SETUP)
  if (stream_client_get_info_rtsp(s->stream_client, s->http_client) != 0) {
    fprintf(stderr, "sender: warning - GET /info RTSP failed (non-fatal)\n");
    // Non-fatal, continue
  }

  // Step 4b: First RTSP SETUP with ekey/eiv (initializes mirroring)
  // Generate random IV for encryption
  unsigned char eiv[16];
  for (int i = 0; i < 16; i++) {
    eiv[i] = (unsigned char)(rand() & 0xff);
  }

  // Encrypt audio AES key using FairPlay to get ekey
  unsigned char ekey[72];
  if (fairplay_client_encrypt_key(s->fairplay_client, audio_aes_key, ekey) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to encrypt key with FairPlay");
    cleanup_components(s);
    return -1;
  }

  fprintf(stderr, "sender: sending first SETUP with ekey/eiv\n");
  fprintf(stderr, "sender: ekey[0-15] = ");
  for (int i = 0; i < 16; i++) {
    fprintf(stderr, "%02x ", ekey[i]);
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "sender: eiv[0-15] = ");
  for (int i = 0; i < 16; i++) {
    fprintf(stderr, "%02x ", eiv[i]);
  }
  fprintf(stderr, "\n");

  // Send first SETUP with ekey/eiv to initialize mirroring
  // Use a placeholder timing port (0) - the receiver will provide the actual port
  if (stream_client_setup_rtsp(s->stream_client, s->http_client,
                                s->receiver_host, s->receiver_port,
                                ekey, eiv, 0,  // ekey/eiv for first SETUP, timing_port=0 (receiver will provide)
                                device_id, os_name, os_version, model, name) != 0) {
    fprintf(stderr, "sender: first SETUP failed\n");
    sender_set_state(s, SENDER_STATE_ERROR, "RTSP SETUP (first) failed");
    cleanup_components(s);
    return -1;
  }

  fprintf(stderr, "sender: first SETUP succeeded\n");

  // Get stream info from first SETUP response (already contains all ports including dataPort)
  // Real devices don't send a second SETUP - they use the dataPort from the first SETUP response
  if (stream_client_get_info(s->stream_client, &stream_info) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to get stream info from first SETUP");
    cleanup_components(s);
    return -1;
  }
  s->stream_info = stream_info;

  // Verify we got the dataPort from the first SETUP
  if (stream_info.data_port == 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "First SETUP did not return dataPort");
    cleanup_components(s);
    return -1;
  }

  fprintf(stderr, "sender: using dataPort=%d from first SETUP response\n", stream_info.data_port);

  // Initialize NTP client
  s->ntp_client = ntp_client_init();
  if (!s->ntp_client) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to initialize NTP client");
    cleanup_components(s);
    return -1;
  }

  // Connect NTP client to receiver's timing port
  if (stream_info.timing_port != 0) {
    if (ntp_client_connect(s->ntp_client, s->receiver_host, stream_info.timing_port) != 0) {
      fprintf(stderr, "sender: warning - failed to connect NTP client\n");
      // Non-fatal, continue without NTP sync
    } else {
      // Perform initial NTP sync
      ntp_client_sync(s->ntp_client);
    }
  }

  sender_set_state(s, SENDER_STATE_IDLE, NULL);
  return 0;
}

int
sender_start_mirroring(sender_t *s, void *source_id)
{
  unsigned char video_aes_key[16];
  unsigned char video_aes_iv[16];
  stream_info_t stream_info;

  assert(s);

  if (s->state != SENDER_STATE_IDLE) {
    return -1;
  }

  if (!s->stream_client || !s->fairplay_initialized) {
    return -1;
  }

  if (stream_client_get_info(s->stream_client, &stream_info) != 0) {
    return -1;
  }

  // Step 7: RTSP RECORD (iPad does this before connecting video stream)
  if (stream_client_record_rtsp(s->stream_client, s->http_client,
                                 s->receiver_host, s->receiver_port) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "RTSP RECORD failed");
    return -1;
  }

  // Connect video stream
  if (stream_client_connect_video(s->stream_client, s->receiver_host) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to connect video stream");
    return -1;
  }

  // Initialize video encryption in stream_client
  // Audio AES key is derived from FairPlay session key
  // For now, use the FairPlay session key directly
  if (stream_client_init_video_encryption(s->stream_client, s->fairplay_session_key) != 0) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to initialize video encryption");
    stream_client_disconnect_video(s->stream_client);
    return -1;
  }

  // Derive video AES key and IV from stream connection ID for packetizer
  // This matches stream_client_init_video_encryption logic
  char key_str[64];
  char iv_str[64];
  unsigned char aeskey_video[64];
  unsigned char aesiv_video[64];

  snprintf(key_str, sizeof(key_str), "AirPlayStreamKey%" PRIu64,
           stream_info.stream_connection_id);
  snprintf(iv_str, sizeof(iv_str), "AirPlayStreamIV%" PRIu64,
           stream_info.stream_connection_id);

  sha512_context ctx;
  sha512_init(&ctx);
  sha512_update(&ctx, (const unsigned char *)key_str, strlen(key_str));
  sha512_update(&ctx, s->fairplay_session_key, 16);
  sha512_final(&ctx, aeskey_video);

  sha512_init(&ctx);
  sha512_update(&ctx, (const unsigned char *)iv_str, strlen(iv_str));
  sha512_update(&ctx, s->fairplay_session_key, 16);
  sha512_final(&ctx, aesiv_video);

  memcpy(s->video_aes_key, aeskey_video, 16);
  memcpy(s->video_aes_iv, aesiv_video, 16);
  s->video_encryption_initialized = 1;

  // Initialize video packetizer
  s->video_packetizer = video_packetizer_init();
  if (!s->video_packetizer) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to initialize video packetizer");
    stream_client_disconnect_video(s->stream_client);
    return -1;
  }

  // Set encryption for packetizer
  video_packetizer_set_encryption(s->video_packetizer, s->video_aes_key, s->video_aes_iv);

  // Set packetizer callback to send via stream_client
  // The packetizer encrypts the payload and formats the complete packet,
  // then sends it via stream_client_send_raw_video_packet
  video_packetizer_set_callback(s->video_packetizer, on_packetized_video, s);

  // Initialize RTP audio (platform code will set encoder and capture)
  // Default to 44100 Hz sample rate
  // Note: Audio port is not part of stream_info for screen mirroring (type 110)
  // Audio streaming would require a separate audio stream setup (type 96) in the /stream request
  // For now, we initialize RTP audio but don't connect it
  // TODO: Add audio stream setup to /stream request if audio is enabled
  s->rtp_audio = rtp_audio_init(44100);
  if (!s->rtp_audio) {
    sender_set_state(s, SENDER_STATE_ERROR, "Failed to initialize RTP audio");
    video_packetizer_destroy(s->video_packetizer);
    s->video_packetizer = NULL;
    stream_client_disconnect_video(s->stream_client);
    return -1;
  }

  // Audio connection will be set up when audio streaming is properly implemented
  // For now, RTP audio is initialized but not connected

  // Video encoder and frame capture are platform-specific
  // Platform code should call sender_set_video_encoder() and sender_set_frame_capture()
  // after creating these components

  // Audio encoder and capture are platform-specific
  // Platform code should call sender_set_audio_encoder() and sender_set_audio_capture()
  // after creating these components

  s->source_id = source_id;
  s->streaming = 1;
  sender_set_state(s, SENDER_STATE_STREAMING, NULL);

  return 0;
}

void
sender_set_volume(sender_t *s, float volume)
{
  assert(s);
  // Volume control will be implemented when audio streaming is added
  (void)volume;
}

void
sender_set_video_encoder(sender_t *s, void *video_encoder)
{
  assert(s);
  s->video_encoder = (video_encoder_t *)video_encoder;

  if (s->video_encoder) {
    // Wire encoder callback to packetizer
    video_encoder_set_callback(s->video_encoder, on_encoded_frame, s);
  }
}

void
sender_set_frame_capture(sender_t *s, void *frame_capture)
{
  assert(s);
  s->frame_capture = (frame_capture_t *)frame_capture;

  if (s->frame_capture) {
    // Wire capture callback to encoder
    frame_capture_set_callback(s->frame_capture, on_captured_frame, s);
  }
}

void
sender_set_audio_encoder(sender_t *s, void *audio_encoder)
{
  assert(s);
  s->audio_encoder = (audio_encoder_t *)audio_encoder;

  if (s->audio_encoder) {
    // Wire encoder callback to RTP audio
    audio_encoder_set_callback(s->audio_encoder, on_encoded_audio, s);
  }
}

void
sender_set_audio_capture(sender_t *s, void *audio_capture)
{
  assert(s);
  s->audio_capture = (audio_capture_t *)audio_capture;

  if (s->audio_capture) {
    // Wire capture callback to encoder
    audio_capture_set_callback(s->audio_capture, on_captured_audio, s);
  }
}

void
sender_stop(sender_t *s)
{
  assert(s);

  if (s->streaming) {
    if (s->frame_capture) {
      frame_capture_stop(s->frame_capture);
    }
    if (s->audio_capture) {
      audio_capture_stop(s->audio_capture);
    }
    s->streaming = 0;
  }

  cleanup_components(s);

  if (s->receiver_host) {
    free(s->receiver_host);
    s->receiver_host = NULL;
  }

  sender_set_state(s, SENDER_STATE_IDLE, NULL);
}

sender_state_t
sender_get_state(sender_t *s)
{
  assert(s);
  return s->state;
}

void
sender_destroy(sender_t *s)
{
  if (s) {
    sender_stop(s);
    free(s);
  }
}