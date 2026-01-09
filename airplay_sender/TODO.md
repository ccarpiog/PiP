# AirPlay Sender Implementation Guide

This document outlines the step-by-step implementation plan for adding AirPlay sender functionality to PiP.

## Overview

The AirPlay sender will allow PiP to stream pip window (screen/window/hls/airplay) content to AirPlay receivers (Apple TV, AirPlay-enabled speakers, etc.). This is the reverse of the current receiver implementation.

## Protocol Analysis (From Receiver Code)

Based on analysis of the existing receiver implementation, AirPlay 2 uses:

### Control Layer
- **HTTP/RTSP** for control messages (POST/GET requests)
- **Binary plist** for data serialization (`plist/` library)
- Endpoints: `/info`, `/pair-setup`, `/pair-verify`, `/fp-setup`, `/stream`, `/feedback`, `/play`, `/stop`

### Security Layer
- **Ed25519** for digital signatures (`ed25519/`)
- **X25519 (Curve25519)** for key exchange (`curve25519/`)
- **FairPlay** for DRM (`fairplay.h`, `playfair/`)
- **AES-CTR** for video encryption (`mirror_buffer.c`, `aes_ctr.c`)
- **SHA-512** for hashing

### Streaming Layer
- **TCP** for video mirroring (port negotiated during `/stream` setup)
- **RTP/UDP** for audio streaming
- **NTP** for time synchronization (`raop_ntp.c`)

### Video Format
- H.264 with NAL unit format (size-prefixed, not Annex-B start codes)
- 128-byte packet header containing:
  - Bytes 0-3: Payload size (big-endian)
  - Byte 4: Packet type (0x00=encrypted video, 0x01=SPS/PPS, 0x05=streaming report)
  - Bytes 8-15: NTP timestamp
- Video encrypted with AES-128-CTR

### Audio Format
- AAC or AAC-ELD or ALAC (codec type 2, 4, or 8)
- Sample rate: 44100 Hz
- Channels: 2 (stereo)
- RTP packetization

---

## Prerequisites

- [ ] Study `airplay/raop_handlers.h` for protocol message formats
- [ ] Study `airplay/raop_rtp_mirror.c` for video packet structure
- [ ] Study `airplay/raop_rtp.c` for audio packet structure
- [ ] Study `airplay/pairing.c` for authentication handshake
- [ ] Study `airplay/fairplay_playfair.c` for FairPlay handling
- [ ] Understand NTP time conversion (`raop_ntp.c`)

---

## Implementation Steps

### Phase 1: DNS-SD Service Discovery

#### Step 1.1: Extend DNS-SD Library for Browsing
- [ ] Add `DNSServiceBrowse` function pointers to `airplay/dnssd.c`:
  ```c
  typedef void (*DNSServiceBrowseReply)(
      DNSServiceRef sdRef,
      DNSServiceFlags flags,
      uint32_t interfaceIndex,
      DNSServiceErrorType errorCode,
      const char *serviceName,
      const char *regtype,
      const char *replyDomain,
      void *context
  );
  ```
- [ ] Add `DNSServiceResolve` for getting IP/port of discovered services
- [ ] Add `DNSServiceGetAddrInfo` for IP address resolution
- [ ] Add `TXTRecordGetValue*` functions for parsing TXT records

#### Step 1.2: Create Discovery Module
- [ ] Create `airplay_sender/discovery.h`:
  ```c
  typedef struct airplay_receiver_s {
      char name[256];
      char host[256];
      uint16_t port;
      char deviceId[32];
      uint64_t features;
      char model[64];
      bool requiresPassword;
      bool supportsScreenMirroring;
  } airplay_receiver_t;

  typedef void (*receiver_callback_t)(airplay_receiver_t *receiver, bool added);

  void airplay_discovery_start(receiver_callback_t callback);
  void airplay_discovery_stop(void);
  airplay_receiver_t* airplay_discovery_get_receivers(int *count);
  ```
- [ ] Create `airplay_sender/discovery.c`:
  - Browse for `_airplay._tcp` service type
  - Parse TXT records for features:
    - `features` (hex capabilities bitmask)
    - `deviceid` (MAC address)
    - `model` (AppleTV5,3 etc.)
    - `srcvers` (source version)
    - `flags` (status flags)
  - Resolve service to IP address and port
  - Maintain list of available receivers
  - Call callback when receivers appear/disappear

#### Step 1.3: UI Integration for Discovery
- [ ] Add "AirPlay to..." submenu in window context menu
- [ ] Populate submenu with discovered receivers
- [ ] Display receiver icon (based on model type)
- [ ] Show connection status indicator

---

### Phase 2: HTTP Client for AirPlay Control

#### Step 2.1: Create HTTP Client Module
- [ ] Create `airplay_sender/http_client.h`:
  ```c
  typedef struct http_client_s http_client_t;
  typedef struct http_client_response_s {
      int status_code;
      char *headers;
      char *body;
      int body_len;
  } http_client_response_t;

  http_client_t *http_client_init(const char *host, uint16_t port);
  int http_client_connect(http_client_t *client);
  http_client_response_t *http_client_request(http_client_t *client,
      const char *method, const char *path,
      const char *headers, const char *body, int body_len);
  void http_client_disconnect(http_client_t *client);
  void http_client_destroy(http_client_t *client);
  ```
- [ ] Create `airplay_sender/http_client.c`:
  - TCP socket connection
  - HTTP request formatting
  - HTTP response parsing
  - Keep-alive connection handling
  - Timeout handling

#### Step 2.2: Implement Server Info Request
- [ ] Implement `GET /info` request
- [ ] Parse binary plist response (reuse `plist/` library)
- [ ] Extract receiver capabilities:
  - Display resolution (`displays` array)
  - Audio formats (`audioFormats` array)
  - Feature flags (`features`)
  - Source version (`sourceVersion`)

---

### Phase 3: Authentication and Pairing

#### Step 3.1: Implement Pair-Setup (Client Side)
- [ ] Create `airplay_sender/pairing_client.h` and `.c`
- [ ] Generate Ed25519 keypair for this device
- [ ] Send `POST /pair-setup` with 32-byte public key
- [ ] Receive receiver's Ed25519 public key (32 bytes)
- [ ] Store receiver's public key for pair-verify

#### Step 3.2: Implement Pair-Verify (Client Side)
- [ ] Generate X25519 keypair (ephemeral)
- [ ] Send `POST /pair-verify` with:
  - Byte 0: 0x01 (verify mode 1)
  - Bytes 1-3: 0x00 0x00 0x00
  - Bytes 4-35: X25519 public key
  - Bytes 36-67: Ed25519 public key
- [ ] Receive response with receiver's X25519 public key + signature
- [ ] Compute shared secret using X25519
- [ ] Derive encryption keys using HKDF-SHA512
- [ ] Verify receiver's signature
- [ ] Generate our signature and send in step 2
- [ ] Send `POST /pair-verify` with:
  - Byte 0: 0x00 (verify mode 0)
  - Bytes 1-3: 0x00 0x00 0x00
  - Bytes 4-67: Our signature

#### Step 3.3: Implement FairPlay Setup (Client Side)
- [ ] Study `fairplay_playfair.c` to understand FairPlay protocol
- [ ] Send `POST /fp-setup` with 16-byte challenge
- [ ] Receive 142-byte response
- [ ] Send `POST /fp-setup` with 164-byte handshake data
- [ ] Receive 32-byte response (session key material)
- [ ] Derive AES key for video encryption

**Note:** FairPlay is Apple's proprietary DRM. The existing receiver code has
a reverse-engineered implementation. For sender, we need to generate valid
FairPlay messages that the receiver will accept.

---

### Phase 4: Stream Setup and Control

#### Step 4.1: Implement Stream Setup
- [ ] Create `airplay_sender/stream_client.h` and `.c`
- [ ] Build `/stream` request binary plist with:
  ```
  {
    "streams": [{
      "type": 110,  // Screen mirroring
      "streamConnectionID": <random 8 bytes>,
      "timestampInfo": [{
        "name": "local", "rate": 1000000
      }]
    }],
    "deviceID": <our MAC address>,
    "sessionUUID": <random UUID>,
    "osName": "Mac OS X",
    "osVersion": "...",
    "model": "MacBookPro..."
  }
  ```
- [ ] Parse response for:
  - `streams[0].dataPort` (TCP port for video)
  - `streams[0].controlPort` (if applicable)
  - `eventPort` (for feedback)
- [ ] Store `streamConnectionID` for AES key derivation

#### Step 4.2: Implement Video Stream Connection
- [ ] Connect TCP socket to receiver's `dataPort`
- [ ] Initialize AES-CTR encryption using derived key
- [ ] The stream connection ID is used for AES IV derivation
  (see `mirror_buffer_init_aes` in `mirror_buffer.c`)

#### Step 4.3: Implement Feedback Channel
- [ ] Connect to `eventPort` for receiver feedback
- [ ] Handle heartbeat/keep-alive messages
- [ ] Process rate control feedback

---

### Phase 5: Video Encoding and Streaming

#### Step 5.1: Create Video Encoder
- [ ] Create `airplay_sender/video_encoder.h`:
  ```c
  typedef struct video_encoder_s video_encoder_t;
  typedef void (*encoded_frame_callback_t)(
      uint8_t *data, int len,
      bool is_keyframe,
      uint8_t *sps, int sps_len,
      uint8_t *pps, int pps_len,
      uint64_t pts,
      void *ctx
  );

  video_encoder_t *video_encoder_init(int width, int height, int fps, int bitrate);
  void video_encoder_set_callback(video_encoder_t *enc, encoded_frame_callback_t cb, void *ctx);
  int video_encoder_encode_frame(video_encoder_t *enc, CVPixelBufferRef pixelBuffer, uint64_t pts);
  void video_encoder_destroy(video_encoder_t *enc);
  ```
- [ ] Create `airplay_sender/video_encoder.m` (Objective-C for VideoToolbox):
  - Use `VTCompressionSessionCreate`
  - Configure for real-time encoding:
    - `kVTCompressionPropertyKey_RealTime` = true
    - `kVTCompressionPropertyKey_AllowFrameReordering` = false (no B-frames)
    - `kVTCompressionPropertyKey_ProfileLevel` = Baseline or Main
    - `kVTCompressionPropertyKey_AverageBitRate`
    - `kVTCompressionPropertyKey_MaxKeyFrameInterval`
  - Extract SPS/PPS from CMFormatDescription
  - Handle `VTCompressionOutputCallback`

#### Step 5.2: Create Video Packetizer
- [ ] Create `airplay_sender/video_packetizer.h` and `.c`
- [ ] Format packets according to receiver's expected format:
  ```c
  struct video_packet_header {
      uint32_t payload_size;     // Big-endian
      uint8_t  packet_type;      // 0x00=video, 0x01=SPS/PPS
      uint8_t  flags;            // Usually 0x00 or 0x10
      uint16_t reserved;         // Usually 0x0000
      uint64_t ntp_timestamp;    // Big-endian NTP time
      // ... additional header bytes up to 128 total
  };
  ```
- [ ] Convert NAL units from Annex-B to AVCC (length-prefixed) format
- [ ] Encrypt payload with AES-128-CTR
- [ ] Send 128-byte header followed by encrypted payload

#### Step 5.3: Integrate with Screen Capture
- [ ] Create capture loop using existing `CaptureWindow()` or display stream
- [ ] Convert `CGImageRef` to `CVPixelBufferRef`:
  ```objc
  CVPixelBufferRef pixelBuffer = NULL;
  CVPixelBufferCreate(kCFAllocatorDefault, width, height,
      kCVPixelFormatType_32BGRA, attrs, &pixelBuffer);
  CVPixelBufferLockBaseAddress(pixelBuffer, 0);
  // Copy CGImage data to pixel buffer
  CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
  ```
- [ ] Alternatively, use `CGDisplayStream` with direct `CVPixelBufferRef` output
- [ ] Feed frames to encoder at target frame rate (30/60 fps)
- [ ] Handle resolution changes (recreate encoder)

---

### Phase 6: Audio Capture and Streaming

#### Step 6.1: Create Audio Capture
- [ ] Create `airplay_sender/audio_capture.h`:
  ```c
  typedef struct audio_capture_s audio_capture_t;
  typedef void (*audio_samples_callback_t)(
      float *samples, int num_frames,
      int channels, int sample_rate,
      uint64_t pts, void *ctx
  );

  audio_capture_t *audio_capture_init(int sample_rate, int channels);
  void audio_capture_set_callback(audio_capture_t *cap, audio_samples_callback_t cb, void *ctx);
  int audio_capture_start(audio_capture_t *cap);
  void audio_capture_stop(audio_capture_t *cap);
  void audio_capture_destroy(audio_capture_t *cap);
  ```
- [ ] Create `airplay_sender/audio_capture.m`:
  - Use `AVCaptureSession` with `AVCaptureAudioDataOutput` for app audio
  - Or use `AudioObjectAddPropertyListener` for system audio (requires ScreenCaptureKit or audio loopback)
  - Handle sample format conversion

**Note:** Capturing system audio on macOS requires either:
1. ScreenCaptureKit (macOS 12.3+) with audio
2. A virtual audio driver (like BlackHole)
3. Limiting to app-specific audio

#### Step 6.2: Create Audio Encoder
- [ ] Create `airplay_sender/audio_encoder.h` and `.c`
- [ ] Use AudioToolbox `AudioConverterRef`:
  ```c
  AudioStreamBasicDescription inputFormat = {
      .mSampleRate = 44100,
      .mFormatID = kAudioFormatLinearPCM,
      .mChannelsPerFrame = 2,
      // ...
  };
  AudioStreamBasicDescription outputFormat = {
      .mSampleRate = 44100,
      .mFormatID = kAudioFormatMPEG4AAC,
      .mChannelsPerFrame = 2,
      // ...
  };
  AudioConverterNew(&inputFormat, &outputFormat, &converter);
  ```
- [ ] Configure AAC encoder (128-256 kbps)
- [ ] Package frames with proper timestamps

#### Step 6.3: RTP Audio Streaming
- [ ] Create UDP socket for audio
- [ ] Implement RTP packetization (RFC 3550)
- [ ] Calculate RTP timestamps based on sample count
- [ ] Add RTP header (version, payload type, sequence number, timestamp, SSRC)
- [ ] Handle RTCP for synchronization if needed

#### Step 6.4: Audio-Video Synchronization
- [ ] Use NTP timestamps for both streams
- [ ] Calculate presentation timestamps relative to stream start
- [ ] Ensure audio and video timestamps are aligned

---

### Phase 7: NTP Time Synchronization

#### Step 7.1: Implement NTP Client
- [ ] Create `airplay_sender/ntp_client.h` and `.c`
- [ ] Study `raop_ntp.c` for existing NTP implementation
- [ ] Send NTP requests to receiver's timing port
- [ ] Calculate round-trip time and clock offset
- [ ] Maintain running average of offset

#### Step 7.2: Timestamp Conversion
- [ ] Convert local timestamps to NTP format
- [ ] Apply clock offset for accurate sync
- [ ] Handle timestamp wraparound

---

### Phase 8: Integration and State Management

#### Step 8.1: Create Sender Manager
- [ ] Create `airplay_sender/sender.h`:
  ```c
  typedef enum {
      SENDER_STATE_IDLE,
      SENDER_STATE_CONNECTING,
      SENDER_STATE_PAIRING,
      SENDER_STATE_STREAMING,
      SENDER_STATE_ERROR
  } sender_state_t;

  typedef void (*sender_state_callback_t)(sender_state_t state, const char *error, void *ctx);

  typedef struct sender_s sender_t;

  sender_t *sender_init(void);
  void sender_set_state_callback(sender_t *s, sender_state_callback_t cb, void *ctx);
  int sender_connect(sender_t *s, airplay_receiver_t *receiver);
  int sender_start_mirroring(sender_t *s, CGWindowID window_id);
  int sender_start_mirroring_display(sender_t *s, CGDirectDisplayID display_id);
  void sender_set_volume(sender_t *s, float volume);
  void sender_stop(sender_t *s);
  void sender_destroy(sender_t *s);
  ```
- [ ] Create `airplay_sender/sender.c`:
  - Coordinate all components
  - Handle connection lifecycle
  - Manage streaming threads
  - Error recovery and cleanup

#### Step 8.2: Window Class Integration
- [ ] Add `sender_t *sender` instance variable to Window
- [ ] Add "AirPlay to..." menu items in `rightMouseDown:`
- [ ] Add "Stop AirPlay" when streaming
- [ ] Show AirPlay icon/indicator when streaming
- [ ] Handle sender state callbacks

#### Step 8.3: Preferences Integration
- [ ] Add AirPlay sender enable/disable preference
- [ ] Add quality presets (low/medium/high)
- [ ] Add audio enable/disable option

---

### Phase 9: Testing and Debugging

#### Step 9.1: Create Test Utilities
- [ ] Add debug logging with log levels
- [ ] Create packet dump utility for debugging
- [ ] Add network traffic capture option

#### Step 9.2: Unit Testing
- [ ] Test DNS-SD discovery with mock services
- [ ] Test pairing with known test vectors
- [ ] Test video encoder with sample frames
- [ ] Test packet formatting

#### Step 9.3: Integration Testing
- [ ] Test with Apple TV 4K
- [ ] Test with Apple TV HD
- [ ] Test with AirPlay-enabled speakers
- [ ] Test with HomePod

#### Step 9.4: Edge Case Testing
- [ ] Test network disconnection
- [ ] Test receiver sleep/wake
- [ ] Test resolution changes
- [ ] Test long streaming sessions (memory leaks)

---

## Technical Challenges and Solutions

### Challenge 1: FairPlay DRM
**Problem:** FairPlay is Apple's proprietary DRM system.
**Solution:** The receiver code has a reverse-engineered implementation in `playfair/`.
For sender, we need to generate valid FairPlay handshake messages. Study `fairplay_playfair.c`
and `omg_hax.c` for the implementation details.

### Challenge 2: System Audio Capture on macOS
**Problem:** macOS doesn't allow direct system audio capture without user consent.
**Solutions:**
1. Use ScreenCaptureKit (macOS 12.3+) which includes audio
2. For older macOS, require virtual audio driver
3. Only capture window/app-specific audio when possible
4. Consider video-only mode as fallback

### Challenge 3: Low Latency Encoding
**Problem:** Default VideoToolbox settings prioritize quality over latency.
**Solution:**
- Set `kVTCompressionPropertyKey_RealTime` = true
- Disable B-frames (`AllowFrameReordering` = false)
- Use lower GOP sizes
- Consider hardware encoding only

### Challenge 4: Time Synchronization
**Problem:** Audio and video must be precisely synchronized.
**Solution:**
- Use NTP for wall-clock synchronization
- Calculate presentation timestamps carefully
- Handle clock drift with periodic resync

---

## File Structure

```
airplay_sender/
├── TODO.md                  # This file
├── discovery.h              # Service discovery API
├── discovery.c              # DNS-SD browsing implementation
├── http_client.h            # HTTP client API
├── http_client.c            # HTTP client implementation
├── pairing_client.h         # Pairing API
├── pairing_client.c         # Ed25519/X25519 client side
├── fairplay_client.h        # FairPlay client API
├── fairplay_client.c        # FairPlay handshake client
├── stream_client.h          # Stream setup API
├── stream_client.c          # Stream connection management
├── video_encoder.h          # Video encoder API
├── video_encoder.m          # VideoToolbox H.264 encoder
├── video_packetizer.h       # Video packet formatting API
├── video_packetizer.c       # AirPlay video packet format
├── audio_capture.h          # Audio capture API
├── audio_capture.m          # Core Audio / ScreenCaptureKit
├── audio_encoder.h          # Audio encoder API
├── audio_encoder.c          # AudioToolbox AAC encoder
├── ntp_client.h             # NTP client API
├── ntp_client.c             # Time synchronization
├── sender.h                 # Main sender API
├── sender.c                 # Sender state machine
└── sender_internal.h        # Internal shared definitions
```

---

## Dependencies

### Existing (reusable from receiver)
- `airplay/plist/` - Binary plist parsing/generation
- `airplay/ed25519/` - Ed25519 signatures
- `airplay/curve25519/` - X25519 key exchange
- `airplay/crypto/` - AES, SHA, HMAC
- `airplay/aes_ctr.c/h` - AES-CTR encryption
- `airplay/playfair/` - FairPlay implementation
- `airplay/byteutils.c/h` - Byte manipulation
- `airplay/logger.c/h` - Logging

### macOS Frameworks (add to Xcode project)
- VideoToolbox.framework (H.264 encoding)
- AudioToolbox.framework (AAC encoding)
- CoreMedia.framework (sample buffers, timestamps)
- CoreVideo.framework (pixel buffers)
- CoreAudio.framework (audio capture)
- ScreenCaptureKit.framework (macOS 12.3+, optional)

---

## Implementation Order

### Week 1: Foundation
1. **Phase 1** - DNS-SD discovery (1-2 days)
2. **Phase 2** - HTTP client (1 day)
3. Start **Phase 3** - Pairing (2 days)

### Week 2: Protocol
1. Complete **Phase 3** - FairPlay (2-3 days)
2. **Phase 4** - Stream setup (2 days)

### Week 3: Video
1. **Phase 5** - Video encoding and streaming (5 days)

### Week 4: Audio & Integration
1. **Phase 6** - Audio (3 days)
2. **Phase 7** - NTP sync (1 day)
3. **Phase 8** - Integration (2 days)

### Week 5: Testing & Polish
1. **Phase 9** - Testing and bug fixes

**Total: ~5 weeks** for complete implementation

---

## References

### From This Codebase
- `airplay/raop_handlers.h` - Protocol message handlers
- `airplay/raop_rtp_mirror.c` - Video packet format (lines 297-400)
- `airplay/raop_rtp.c` - Audio RTP handling
- `airplay/pairing.c` - Ed25519/X25519 key exchange
- `airplay/fairplay_playfair.c` - FairPlay implementation
- `airplay/mirror_buffer.c` - AES encryption setup

### External
- RFC 3550 - RTP specification
- RFC 6184 - RTP Payload Format for H.264
- RFC 3640 - RTP Payload Format for AAC
- Apple VideoToolbox documentation
- Apple AudioToolbox documentation
