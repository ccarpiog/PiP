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

#ifndef STREAM_CLIENT_H
#define STREAM_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct http_client_s;

typedef struct stream_client_s stream_client_t;

typedef struct stream_info_s {
  uint16_t data_port;  // TCP port for video stream
  uint16_t control_port;  // Control port (if applicable)
  uint16_t event_port;  // Port for feedback/events
  uint16_t timing_port;  // Port for NTP timing
  uint64_t stream_connection_id;  // Used for AES key derivation
} stream_info_t;

/**
 * Initialize stream client
 */
stream_client_t *stream_client_init(void);

/**
 * Setup stream: send POST /stream request and parse response
 * Returns 0 on success, -1 on error
 * device_id: MAC address string (e.g., "aa:bb:cc:dd:ee:ff")
 * os_name: Operating system name (e.g., "Mac OS X")
 * os_version: Operating system version
 * model: Device model (e.g., "MacBookPro18,1")
 */
int stream_client_setup(stream_client_t *client, struct http_client_s *http_client,
                       const char *device_id, const char *os_name,
                       const char *os_version, const char *model);

/**
 * Get stream information from setup response
 * Returns 0 on success, -1 if not available
 */
int stream_client_get_info(stream_client_t *client, stream_info_t *info);

/**
 * Connect TCP socket to receiver's dataPort for video streaming
 * Returns 0 on success, -1 on error
 */
int stream_client_connect_video(stream_client_t *client, const char *host);

/**
 * Initialize AES-CTR encryption for video stream
 * audio_aes_key: 16-byte AES key from FairPlay setup
 * Returns 0 on success, -1 on error
 */
int stream_client_init_video_encryption(stream_client_t *client,
                                        const unsigned char *audio_aes_key);

/**
 * Send video packet (encrypted)
 * packet_type: 0x00=encrypted video, 0x01=SPS/PPS, 0x05=streaming report
 * data: Video data to send
 * data_len: Length of video data
 * ntp_timestamp: NTP timestamp (for type 0x00 and 0x01)
 * Returns 0 on success, -1 on error
 */
int stream_client_send_video_packet(stream_client_t *client,
                                    uint8_t packet_type,
                                    const uint8_t *data, int data_len,
                                    uint64_t ntp_timestamp);

/**
 * Send raw video packet (already formatted with header + encrypted payload)
 * packet: Complete packet (128-byte header + encrypted payload)
 * packet_len: Total packet length
 * Returns 0 on success, -1 on error
 */
int stream_client_send_raw_video_packet(stream_client_t *client,
                                        const uint8_t *packet, int packet_len);

/**
 * Connect to eventPort for feedback/events
 * Returns 0 on success, -1 on error
 */
int stream_client_connect_feedback(stream_client_t *client, const char *host);

/**
 * Send GET /info RTSP/1.0 request
 * Returns 0 on success, -1 on error
 */
int stream_client_get_info_rtsp(stream_client_t *client, struct http_client_s *http_client);

/**
 * Send RTSP SETUP request with binary plist
 * host: Receiver hostname
 * port: Receiver port
 * ekey: 72-byte FairPlay-encrypted AES key (or NULL to skip ekey/eiv)
 * eiv: 16-byte AES IV (or NULL to skip ekey/eiv)
 * timing_port: Our NTP client port
 * device_id: Device MAC address
 * os_name: OS name
 * os_version: OS version
 * model: Device model
 * name: Device name
 * Returns 0 on success, -1 on error
 */
int stream_client_setup_rtsp(stream_client_t *client, struct http_client_s *http_client,
                             const char *host, uint16_t port,
                             const unsigned char *ekey, const unsigned char *eiv,
                             uint16_t timing_port,
                             const char *device_id, const char *os_name,
                             const char *os_version, const char *model, const char *name);

/**
 * Send RTSP RECORD request
 * host: Receiver hostname
 * port: Receiver port
 * Returns 0 on success, -1 on error
 */
int stream_client_record_rtsp(stream_client_t *client, struct http_client_s *http_client,
                             const char *host, uint16_t port);

/**
 * Disconnect video stream
 */
void stream_client_disconnect_video(stream_client_t *client);

/**
 * Disconnect feedback channel
 */
void stream_client_disconnect_feedback(stream_client_t *client);

/**
 * Clean up stream client
 */
void stream_client_destroy(stream_client_t *client);

#ifdef __cplusplus
}
#endif

#endif