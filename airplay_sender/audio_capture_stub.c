/**
 *  Copyright (C) 2024  PiP Project
 *
 *  Stub implementation for testing - platform-specific implementation will replace this
 */

#include "audio_capture.h"
#include <stdlib.h>
#include <string.h>

struct audio_capture_s {
  audio_samples_callback_t callback;
  void *callback_ctx;
  int sample_rate;
  int channels;
  int started;
};

audio_capture_t *
audio_capture_init(int sample_rate, int channels)
{
  audio_capture_t *cap = calloc(1, sizeof(audio_capture_t));
  if (!cap) {
    return NULL;
  }

  cap->sample_rate = sample_rate;
  cap->channels = channels;
  cap->callback = NULL;
  cap->callback_ctx = NULL;
  cap->started = 0;

  return cap;
}

void
audio_capture_set_callback(audio_capture_t *cap, audio_samples_callback_t cb, void *ctx)
{
  if (cap) {
    cap->callback = cb;
    cap->callback_ctx = ctx;
  }
}

int
audio_capture_start(audio_capture_t *cap)
{
  if (!cap) {
    return -1;
  }

  cap->started = 1;
  // Stub - does nothing, no actual capture
  return 0;
}

void
audio_capture_stop(audio_capture_t *cap)
{
  if (cap) {
    cap->started = 0;
  }
}

void
audio_capture_destroy(audio_capture_t *cap)
{
  if (cap) {
    free(cap);
  }
}
