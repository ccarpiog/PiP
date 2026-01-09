/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
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

#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* Process DNS-SD events - call this periodically to invoke callbacks */
void airplay_discovery_process_events(void);

/* Logger callback for discovery */
typedef void (*discovery_log_callback_t)(void *cls, int level, const char *msg);
void airplay_discovery_set_log_callback(discovery_log_callback_t callback, void *cls);

#ifdef __cplusplus
}
#endif

#endif
