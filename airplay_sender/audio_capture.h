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

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_capture_s audio_capture_t;

/**
 * Callback for captured audio samples
 * samples: Interleaved PCM samples (channels * num_frames)
 * num_frames: Number of audio frames
 * channels: Number of audio channels (typically 2 for stereo)
 * sample_rate: Sample rate in Hz (typically 44100)
 * pts: Presentation timestamp in microseconds
 * ctx: User context
 */
typedef void (*audio_samples_callback_t)(
    float *samples,
    int num_frames,
    int channels,
    int sample_rate,
    uint64_t pts,
    void *ctx
);

/**
 * Initialize audio capture
 * sample_rate: Target sample rate (typically 44100 Hz)
 * channels: Number of channels (typically 2 for stereo)
 * Returns capture instance or NULL on error
 */
audio_capture_t *audio_capture_init(int sample_rate, int channels);

/**
 * Set callback for captured audio samples
 */
void audio_capture_set_callback(audio_capture_t *cap, audio_samples_callback_t cb, void *ctx);

/**
 * Start audio capture
 * Returns 0 on success, -1 on error
 */
int audio_capture_start(audio_capture_t *cap);

/**
 * Stop audio capture
 */
void audio_capture_stop(audio_capture_t *cap);

/**
 * Clean up audio capture
 */
void audio_capture_destroy(audio_capture_t *cap);

#ifdef __cplusplus
}
#endif

#endif