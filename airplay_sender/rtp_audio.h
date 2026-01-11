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

#ifndef RTP_AUDIO_H
#define RTP_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtp_audio_s rtp_audio_t;

/**
 * Initialize RTP audio streaming
 * sample_rate: Audio sample rate (typically 44100)
 * Returns RTP audio instance or NULL on error
 */
rtp_audio_t *rtp_audio_init(int sample_rate);

/**
 * Connect to receiver's audio data port
 * host: Receiver hostname or IP address
 * port: Receiver's audio data port (UDP)
 * Returns 0 on success, -1 on error
 */
int rtp_audio_connect(rtp_audio_t *rtp, const char *host, uint16_t port);

/**
 * Send encoded audio data via RTP
 * data: Encoded AAC/ALAC data
 * data_len: Length of encoded data
 * pts: Presentation timestamp in microseconds
 * Returns 0 on success, -1 on error
 */
int rtp_audio_send(rtp_audio_t *rtp, const uint8_t *data, int data_len, uint64_t pts);

/**
 * Disconnect from receiver
 */
void rtp_audio_disconnect(rtp_audio_t *rtp);

/**
 * Clean up RTP audio streaming
 */
void rtp_audio_destroy(rtp_audio_t *rtp);

#ifdef __cplusplus
}
#endif

#endif