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

#ifndef AUDIO_ENCODER_H
#define AUDIO_ENCODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_encoder_s audio_encoder_t;

/**
 * Callback for encoded audio data
 * data: Encoded AAC/ALAC data
 * data_len: Length of encoded data
 * pts: Presentation timestamp in microseconds
 * ctx: User context
 */
typedef void (*encoded_audio_callback_t)(
    uint8_t *data,
    int data_len,
    uint64_t pts,
    void *ctx
);

/**
 * Initialize audio encoder
 * sample_rate: Sample rate in Hz (typically 44100)
 * channels: Number of channels (typically 2 for stereo)
 * bitrate: Target bitrate in bits per second (typically 128000-256000)
 * Returns encoder instance or NULL on error
 */
audio_encoder_t *audio_encoder_init(int sample_rate, int channels, int bitrate);

/**
 * Set callback for encoded audio data
 */
void audio_encoder_set_callback(audio_encoder_t *enc, encoded_audio_callback_t cb, void *ctx);

/**
 * Encode audio samples
 * pcm_samples: Interleaved float32 PCM samples (channels * num_frames)
 * num_frames: Number of audio frames to encode
 * pts: Presentation timestamp in microseconds
 * Returns 0 on success, -1 on error
 */
int audio_encoder_encode(audio_encoder_t *enc, float *pcm_samples, int num_frames, uint64_t pts);

/**
 * Clean up audio encoder
 */
void audio_encoder_destroy(audio_encoder_t *enc);

#ifdef __cplusplus
}
#endif

#endif