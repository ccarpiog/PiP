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

#ifndef FRAME_CAPTURE_H
#define FRAME_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct frame_capture_s frame_capture_t;

/**
 * Callback for captured frames
 * rgba_data: RGBA32 format frame data (row-major)
 * width: Frame width in pixels
 * height: Frame height in pixels
 * stride: Bytes per row (may be larger than width * 4 for alignment)
 * pts: Presentation timestamp in microseconds
 * ctx: User context
 */
typedef void (*frame_capture_callback_t)(
    uint8_t *rgba_data,
    int width,
    int height,
    int stride,
    uint64_t pts,
    void *ctx
);

/**
 * Initialize frame capture
 * source_id: Platform-specific source identifier (e.g., CGWindowID* or CGDirectDisplayID*)
 *            Pass NULL for default source
 * Returns capture instance or NULL on error
 */
frame_capture_t *frame_capture_init(void *source_id);

/**
 * Set callback for captured frames
 */
void frame_capture_set_callback(frame_capture_t *cap, frame_capture_callback_t cb, void *ctx);

/**
 * Start frame capture at specified frame rate
 * fps: Target frame rate (e.g., 30 or 60)
 * Returns 0 on success, -1 on error
 */
int frame_capture_start(frame_capture_t *cap, int fps);

/**
 * Stop frame capture
 */
void frame_capture_stop(frame_capture_t *cap);

/**
 * Clean up frame capture
 */
void frame_capture_destroy(frame_capture_t *cap);

#ifdef __cplusplus
}
#endif

#endif