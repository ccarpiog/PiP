/**
 *  Copyright (C) 2024  PiP Project
 *
 *  Stub implementation for testing - platform-specific implementation will replace this
 */

#include "frame_capture.h"
#include <stdlib.h>
#include <string.h>

struct frame_capture_s {
  frame_capture_callback_t callback;
  void *callback_ctx;
  void *source_id;
  int started;
};

frame_capture_t *
frame_capture_init(void *source_id)
{
  frame_capture_t *cap = calloc(1, sizeof(frame_capture_t));
  if (!cap) {
    return NULL;
  }

  cap->source_id = source_id;
  cap->callback = NULL;
  cap->callback_ctx = NULL;
  cap->started = 0;

  return cap;
}

void
frame_capture_set_callback(frame_capture_t *cap, frame_capture_callback_t cb, void *ctx)
{
  if (cap) {
    cap->callback = cb;
    cap->callback_ctx = ctx;
  }
}

int
frame_capture_start(frame_capture_t *cap, int fps)
{
  if (!cap) {
    return -1;
  }

  cap->started = 1;
  // Stub - does nothing, no actual capture
  (void)fps;
  return 0;
}

void
frame_capture_stop(frame_capture_t *cap)
{
  if (cap) {
    cap->started = 0;
  }
}

void
frame_capture_destroy(frame_capture_t *cap)
{
  if (cap) {
    free(cap);
  }
}
