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

#ifndef SENDER_H
#define SENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sender_s sender_t;

typedef enum {
  SENDER_STATE_IDLE,
  SENDER_STATE_CONNECTING,
  SENDER_STATE_PAIRING,
  SENDER_STATE_STREAMING,
  SENDER_STATE_ERROR
} sender_state_t;

/**
 * Callback for sender state changes
 * state: New state
 * error: Error message (if state is SENDER_STATE_ERROR)
 * ctx: User context
 */
typedef void (*sender_state_callback_t)(sender_state_t state, const char *error, void *ctx);

/**
 * Initialize sender
 * Returns sender instance or NULL on error
 */
sender_t *sender_init(void);

/**
 * Set callback for state changes
 */
void sender_set_state_callback(sender_t *s, sender_state_callback_t cb, void *ctx);

struct airplay_receiver_s;

/**
 * Connect to AirPlay receiver
 * receiver: Discovered receiver information
 * device_id: Our device ID (MAC address string, e.g., "aa:bb:cc:dd:ee:ff")
 * os_name: Operating system name (e.g., "Mac OS X")
 * os_version: Operating system version
 * model: Device model (e.g., "MacBookPro18,1")
 * name: Device name (e.g., hostname, can be NULL)
 * Returns 0 on success, -1 on error
 */
int sender_connect(sender_t *s, struct airplay_receiver_s *receiver,
                   const char *device_id, const char *os_name,
                   const char *os_version, const char *model, const char *name);

/**
 * Start mirroring to receiver
 * source_id: Platform-specific source identifier (e.g., CGWindowID* or CGDirectDisplayID*)
 *            Pass NULL for default source
 * Returns 0 on success, -1 on error
 */
int sender_start_mirroring(sender_t *s, void *source_id);

/**
 * Set volume (0.0 to 1.0)
 */
void sender_set_volume(sender_t *s, float volume);

/**
 * Set video encoder (platform-specific implementation)
 * The encoder will be automatically wired to the packetizer
 */
void sender_set_video_encoder(sender_t *s, void *video_encoder);

/**
 * Set frame capture (platform-specific implementation)
 * The capture will be automatically wired to the encoder
 */
void sender_set_frame_capture(sender_t *s, void *frame_capture);

/**
 * Set audio encoder (platform-specific implementation)
 * The encoder will be automatically wired to RTP audio
 */
void sender_set_audio_encoder(sender_t *s, void *audio_encoder);

/**
 * Set audio capture (platform-specific implementation)
 * The capture will be automatically wired to the encoder
 */
void sender_set_audio_capture(sender_t *s, void *audio_capture);

/**
 * Stop streaming and disconnect
 */
void sender_stop(sender_t *s);

/**
 * Get current sender state
 */
sender_state_t sender_get_state(sender_t *s);

/**
 * Clean up sender
 */
void sender_destroy(sender_t *s);

#ifdef __cplusplus
}
#endif

#endif