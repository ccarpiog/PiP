/**
 *  Copyright (C) 2024  PiP Project
 *
 *  Stub implementation for testing - platform-specific implementation will replace this
 */

#include "audio_encoder.h"
#include <stdlib.h>
#include <string.h>

struct audio_encoder_s {
  encoded_audio_callback_t callback;
  void *callback_ctx;
  int sample_rate;
  int channels;
  int bitrate;
};

audio_encoder_t *
audio_encoder_init(int sample_rate, int channels, int bitrate)
{
  audio_encoder_t *enc = calloc(1, sizeof(audio_encoder_t));
  if (!enc) {
    return NULL;
  }

  enc->sample_rate = sample_rate;
  enc->channels = channels;
  enc->bitrate = bitrate;
  enc->callback = NULL;
  enc->callback_ctx = NULL;

  return enc;
}

void
audio_encoder_set_callback(audio_encoder_t *enc, encoded_audio_callback_t cb, void *ctx)
{
  if (enc) {
    enc->callback = cb;
    enc->callback_ctx = ctx;
  }
}

int
audio_encoder_encode(audio_encoder_t *enc, float *pcm_samples, int num_frames, uint64_t pts)
{
  // Stub - does nothing
  (void)enc;
  (void)pcm_samples;
  (void)num_frames;
  (void)pts;
  return 0;
}

void
audio_encoder_destroy(audio_encoder_t *enc)
{
  if (enc) {
    free(enc);
  }
}
