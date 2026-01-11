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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "../airplay/dnssd.h"
#include "../airplay/compat.h"
#include "../airplay/threads.h"
#include "../airplay/logger.h"
#include "discovery.h"

#ifndef WIN32
#include <dns_sd.h>
#endif

#define MAX_RECEIVERS 64

typedef struct receiver_node_s {
  airplay_receiver_t receiver;
  char service_name[256];
  void *browse_ref;
  void *resolve_ref;
  void *addrinfo_ref;
  bool resolved;
  struct receiver_node_s *next;
} receiver_node_t;

typedef struct discovery_state_s {
  dnssd_t *dnssd;
  receiver_callback_t callback;
  receiver_node_t *receivers;
  mutex_handle_t mutex;
  bool running;
  logger_t *logger;
  void *browse_ref;  // Main browse reference
} discovery_state_t;

static discovery_state_t *g_discovery = NULL;

static void
parse_hex_features(const char *hex_str, uint64_t *features)
{
  uint64_t val = 0;
  if (hex_str && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
    sscanf(hex_str + 2, "%llx", (unsigned long long *)&val);
  } else if (hex_str) {
    sscanf(hex_str, "%llx", (unsigned long long *)&val);
  }
  *features = val;
}

static receiver_node_t*
find_receiver_by_name(const char *service_name)
{
  receiver_node_t *node = g_discovery->receivers;
  while (node) {
    if (strcmp(node->service_name, service_name) == 0) {
      return node;
    }
    node = node->next;
  }
  return NULL;
}

static void resolve_reply(void *sdRef, uint32_t flags, uint32_t interfaceIndex,
                          int32_t errorCode, const char *fullname,
                          const char *hosttarget, uint16_t port,
                          uint16_t txtLen, const unsigned char *txtRecord, void *context);
static void addrinfo_reply(void *sdRef, uint32_t flags, uint32_t interfaceIndex,
                           int32_t errorCode, const char *hostname,
                           const struct sockaddr *address, uint32_t ttl, void *context);

static void
remove_receiver(receiver_node_t *node)
{
  receiver_node_t **prev = &g_discovery->receivers;
  while (*prev) {
    if (*prev == node) {
      *prev = node->next;
      if (g_discovery->callback) {
        g_discovery->callback(&node->receiver, false);
      }
      if (node->browse_ref) {
        dnssd_browse_stop(g_discovery->dnssd, node->browse_ref);
      }
      if (node->resolve_ref) {
        dnssd_resolve_stop(g_discovery->dnssd, node->resolve_ref);
      }
      if (node->addrinfo_ref) {
        dnssd_getaddrinfo_stop(g_discovery->dnssd, node->addrinfo_ref);
      }
      free(node);
      return;
    }
    prev = &(*prev)->next;
  }
}

static void
browse_reply(void *sdRef, uint32_t flags, uint32_t interfaceIndex,
             int32_t errorCode, const char *serviceName,
             const char *regtype, const char *replyDomain, void *context)
{
  discovery_state_t *state = (discovery_state_t *)context;

  // Log that callback was invoked
  if (state && state->logger) {
    logger_log(state->logger, LOGGER_DEBUG, "airplay_discovery: browse_reply called (errorCode=%d, flags=0x%x, serviceName=%s, regtype=%s)",
               errorCode, flags, serviceName ? serviceName : "NULL", regtype ? regtype : "NULL");
  }

  if (errorCode != kDNSServiceErr_NoError) {
    if (state && state->logger) {
      logger_log(state->logger, LOGGER_ERR, "airplay_discovery browse error: %d", errorCode);
    }
    return;
  }

  // Log services that don't match _airplay._tcp
  if (state->logger && regtype && strstr(regtype, "_airplay._tcp") == NULL) {
    logger_log(state->logger, LOGGER_DEBUG, "airplay_discovery: non-AirPlay service discovered: '%s' (type: '%s', domain: '%s')",
               serviceName, regtype, replyDomain);
  }

  MUTEX_LOCK(state->mutex);

  if (flags & kDNSServiceFlagsAdd) {
    receiver_node_t *node = find_receiver_by_name(serviceName);
    if (!node) {
      node = calloc(1, sizeof(receiver_node_t));
      if (node) {
        strncpy(node->service_name, serviceName, sizeof(node->service_name) - 1);
        strncpy(node->receiver.name, serviceName, sizeof(node->receiver.name) - 1);
        node->browse_ref = sdRef;
        node->next = state->receivers;
        state->receivers = node;

        if (state->logger) {
          logger_log(state->logger, LOGGER_INFO, "airplay_discovery: discovered service '%s' (type: '%s', domain: '%s')",
                     serviceName, regtype ? regtype : "unknown", replyDomain ? replyDomain : "local");
        }

        void *resolve_ref = dnssd_resolve_start(state->dnssd, serviceName, regtype,
                                                 replyDomain, resolve_reply, node);
        if (resolve_ref) {
          node->resolve_ref = resolve_ref;
        } else {
          if (state->logger) {
            logger_log(state->logger, LOGGER_WARNING, "airplay_discovery: failed to start resolve for '%s' (type: '%s')",
                       serviceName, regtype ? regtype : "unknown");
          }
        }
      } else {
        if (state->logger) {
          logger_log(state->logger, LOGGER_ERR, "airplay_discovery: failed to allocate receiver node");
        }
      }
    }
  } else {
    receiver_node_t *node = find_receiver_by_name(serviceName);
    if (node) {
      if (state->logger) {
        logger_log(state->logger, LOGGER_INFO, "airplay_discovery: service removed '%s' (type: '%s')",
                   serviceName, regtype ? regtype : "unknown");
      }
      remove_receiver(node);
    } else if (state->logger && regtype && strstr(regtype, "_airplay._tcp") == NULL) {
      // Log removal of non-AirPlay services too
      logger_log(state->logger, LOGGER_DEBUG, "airplay_discovery: non-AirPlay service removed: '%s' (type: '%s')",
                 serviceName, regtype);
    }
  }

  MUTEX_UNLOCK(state->mutex);
}

static void
resolve_reply(void *sdRef, uint32_t flags, uint32_t interfaceIndex,
              int32_t errorCode, const char *fullname,
              const char *hosttarget, uint16_t port,
              uint16_t txtLen, const unsigned char *txtRecord, void *context)
{
  receiver_node_t *node = (receiver_node_t *)context;
  discovery_state_t *state = g_discovery;

  if (errorCode != kDNSServiceErr_NoError || !state) {
    if (state && state->logger) {
      logger_log(state->logger, LOGGER_ERR, "airplay_discovery: resolve error %d for '%s'", errorCode, node->service_name);
    }
    return;
  }

  MUTEX_LOCK(state->mutex);

  node->receiver.port = ntohs(port);
  // Copy hostname and strip trailing period (DNS root) if present
  strncpy(node->receiver.host, hosttarget, sizeof(node->receiver.host) - 1);
  node->receiver.host[sizeof(node->receiver.host) - 1] = '\0';
  // Remove trailing period if present (DNS-SD hostnames often have it)
  size_t host_len = strlen(node->receiver.host);
  if (host_len > 0 && node->receiver.host[host_len - 1] == '.') {
    node->receiver.host[host_len - 1] = '\0';
  }
  node->resolve_ref = NULL;

  uint8_t value_len;
  const void *value;

  if (dnssd_txt_get_value(state->dnssd, txtRecord, txtLen, "deviceid", &value_len, &value) == 0) {
    int len = value_len < sizeof(node->receiver.deviceId) - 1 ? value_len : sizeof(node->receiver.deviceId) - 1;
    memcpy(node->receiver.deviceId, value, len);
    node->receiver.deviceId[len] = '\0';
  }

  if (dnssd_txt_get_value(state->dnssd, txtRecord, txtLen, "features", &value_len, &value) == 0) {
    char features_str[32];
    int len = value_len < sizeof(features_str) - 1 ? value_len : sizeof(features_str) - 1;
    memcpy(features_str, value, len);
    features_str[len] = '\0';
    parse_hex_features(features_str, &node->receiver.features);
    node->receiver.supportsScreenMirroring = (node->receiver.features & 0x800) != 0;
  }

  if (dnssd_txt_get_value(state->dnssd, txtRecord, txtLen, "model", &value_len, &value) == 0) {
    int len = value_len < sizeof(node->receiver.model) - 1 ? value_len : sizeof(node->receiver.model) - 1;
    memcpy(node->receiver.model, value, len);
    node->receiver.model[len] = '\0';
  }

  if (dnssd_txt_get_value(state->dnssd, txtRecord, txtLen, "pw", &value_len, &value) == 0) {
    node->receiver.requiresPassword = (value_len > 0 && ((const char *)value)[0] == '1');
  }

  node->resolved = true;

  if (state->logger) {
    logger_log(state->logger, LOGGER_INFO, "airplay_discovery: resolved '%s' -> %s:%u (model: %s, features: 0x%llx)",
               node->service_name, node->receiver.host, node->receiver.port,
               node->receiver.model, (unsigned long long)node->receiver.features);
  }

  if (state->callback) {
    state->callback(&node->receiver, true);
  }

  void *addrinfo_ref = dnssd_getaddrinfo_start(state->dnssd, hosttarget, addrinfo_reply, node);
  if (addrinfo_ref) {
    node->addrinfo_ref = addrinfo_ref;
  } else {
    if (state->logger) {
      logger_log(state->logger, LOGGER_WARNING, "airplay_discovery: failed to start addrinfo for '%s'", hosttarget);
    }
  }

  MUTEX_UNLOCK(state->mutex);
}

static void
addrinfo_reply(void *sdRef, uint32_t flags, uint32_t interfaceIndex,
               int32_t errorCode, const char *hostname,
               const struct sockaddr *address, uint32_t ttl, void *context)
{
  receiver_node_t *node = (receiver_node_t *)context;
  discovery_state_t *state = g_discovery;

  if (errorCode != kDNSServiceErr_NoError || !address) {
    if (state && state->logger) {
      logger_log(state->logger, LOGGER_WARNING, "airplay_discovery: addrinfo error %d for '%s'", errorCode, hostname);
    }
    return;
  }

  char old_host[256];
  strncpy(old_host, node->receiver.host, sizeof(old_host) - 1);
  old_host[sizeof(old_host) - 1] = '\0';

  if (address->sa_family == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *)address;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
    strncpy(node->receiver.host, ip_str, sizeof(node->receiver.host) - 1);
  } else if (address->sa_family == AF_INET6) {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)address;
    char ip_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, sizeof(ip_str));
    strncpy(node->receiver.host, ip_str, sizeof(node->receiver.host) - 1);
  }

  node->addrinfo_ref = NULL;

  if (state && state->logger && strcmp(old_host, node->receiver.host) != 0) {
    logger_log(state->logger, LOGGER_DEBUG, "airplay_discovery: resolved IP for '%s' -> %s", node->service_name, node->receiver.host);
  }
}

void
airplay_discovery_start(receiver_callback_t callback)
{
  int error = 0;

  if (g_discovery) {
    return;
  }

  g_discovery = calloc(1, sizeof(discovery_state_t));
  if (!g_discovery) {
    return;
  }

  g_discovery->logger = logger_init();
  if (g_discovery->logger) {
    // Set to DEBUG level to see non-AirPlay services
    logger_set_level(g_discovery->logger, LOGGER_DEBUG);
    // Log a test message to verify logger is working
    logger_log(g_discovery->logger, LOGGER_INFO, "airplay_discovery: logger initialized");
  }

  g_discovery->dnssd = dnssd_init("PiP", 3, NULL, 0, &error);
  if (!g_discovery->dnssd) {
    if (g_discovery->logger) {
      logger_log(g_discovery->logger, LOGGER_ERR, "airplay_discovery: failed to initialize DNS-SD (error: %d)", error);
      // logger_destroy(g_discovery->logger);
    }
    free(g_discovery);
    g_discovery = NULL;
    return;
  }

  MUTEX_CREATE(g_discovery->mutex);
  g_discovery->callback = callback;
  g_discovery->running = true;

  if (g_discovery->logger) {
    logger_log(g_discovery->logger, LOGGER_INFO, "airplay_discovery: starting discovery for _airplay._tcp");
  }

  void *browse_ref = dnssd_browse_start(g_discovery->dnssd, "_airplay._tcp",
                                        browse_reply, g_discovery);
  if (!browse_ref) {
    if (g_discovery->logger) {
      logger_log(g_discovery->logger, LOGGER_ERR, "airplay_discovery: failed to start browsing");
      // logger_destroy(g_discovery->logger);
    }
    dnssd_destroy(g_discovery->dnssd);
    MUTEX_DESTROY(g_discovery->mutex);
    free(g_discovery);
    g_discovery = NULL;
  } else {
    g_discovery->browse_ref = browse_ref;
    if (g_discovery->logger) {
      logger_log(g_discovery->logger, LOGGER_INFO, "airplay_discovery: browsing started successfully");
      logger_log(g_discovery->logger, LOGGER_DEBUG, "airplay_discovery: browse_ref = %p", browse_ref);
      logger_log(g_discovery->logger, LOGGER_DEBUG, "airplay_discovery: waiting for services (browse callback should be invoked by run loop)");
      logger_log(g_discovery->logger, LOGGER_DEBUG, "airplay_discovery: NOTE - If no services appear, try running 'dns-sd -B _airplay._tcp' in Terminal to verify services exist on network");
    }
  }
}

void
airplay_discovery_stop(void)
{
  if (!g_discovery) {
    return;
  }

  if (g_discovery->logger) {
    logger_log(g_discovery->logger, LOGGER_INFO, "airplay_discovery: stopping discovery");
  }

  MUTEX_LOCK(g_discovery->mutex);
  g_discovery->running = false;


  // Stop main browse reference
  void *browse_ref_to_stop = g_discovery->browse_ref;
  g_discovery->browse_ref = NULL;

  int receiver_count = 0;
  receiver_node_t *node = g_discovery->receivers;
  while (node) {
    receiver_node_t *next = node->next;
    if (node->browse_ref) {
      dnssd_browse_stop(g_discovery->dnssd, node->browse_ref);
    }
    if (node->resolve_ref) {
      dnssd_resolve_stop(g_discovery->dnssd, node->resolve_ref);
    }
    if (node->addrinfo_ref) {
      dnssd_getaddrinfo_stop(g_discovery->dnssd, node->addrinfo_ref);
    }
    if (node->resolved) {
      receiver_count++;
    }
    free(node);
    node = next;
  }
  g_discovery->receivers = NULL;

  // Save dnssd pointer before unlocking - background thread might be using it
  dnssd_t *dnssd_to_destroy = g_discovery->dnssd;
  g_discovery->dnssd = NULL;
  MUTEX_UNLOCK(g_discovery->mutex);

  // Stop browse and destroy dnssd outside of mutex
  if (browse_ref_to_stop && dnssd_to_destroy) {
    dnssd_browse_stop(dnssd_to_destroy, browse_ref_to_stop);
  }

  if (dnssd_to_destroy) {
    dnssd_destroy(dnssd_to_destroy);
  }

  if (g_discovery->logger) {
    logger_log(g_discovery->logger, LOGGER_INFO, "airplay_discovery: stopped (had %d receivers)", receiver_count);
    // logger_destroy(g_discovery->logger);
  }

  MUTEX_DESTROY(g_discovery->mutex);
  free(g_discovery);
  g_discovery = NULL;
}

airplay_receiver_t*
airplay_discovery_get_receivers(int *count)
{
  if (!g_discovery || !count) {
    if (count) *count = 0;
    return NULL;
  }

  MUTEX_LOCK(g_discovery->mutex);

  int receiver_count = 0;
  receiver_node_t *node = g_discovery->receivers;
  while (node) {
    if (node->resolved) {
      receiver_count++;
    }
    node = node->next;
  }

  if (receiver_count == 0) {
    MUTEX_UNLOCK(g_discovery->mutex);
    *count = 0;
    return NULL;
  }

  airplay_receiver_t *receivers = calloc(receiver_count, sizeof(airplay_receiver_t));
  if (!receivers) {
    MUTEX_UNLOCK(g_discovery->mutex);
    *count = 0;
    return NULL;
  }

  int idx = 0;
  node = g_discovery->receivers;
  while (node && idx < receiver_count) {
    if (node->resolved) {
      memcpy(&receivers[idx], &node->receiver, sizeof(airplay_receiver_t));
      idx++;
    }
    node = node->next;
  }

  MUTEX_UNLOCK(g_discovery->mutex);

  *count = receiver_count;
  return receivers;
}

void
airplay_discovery_set_log_callback(discovery_log_callback_t callback, void *cls)
{
  if (!g_discovery || !g_discovery->logger) {
    return;
  }
  logger_set_callback(g_discovery->logger, callback, cls);
  // Log a test message to verify callback is working
  if (callback) {
    logger_log(g_discovery->logger, LOGGER_INFO, "airplay_discovery: log callback set");
  }
}

void
airplay_discovery_process_events(void)
{
  // Check g_discovery exists - it might be freed by stop()
  if (!g_discovery) {
    return;
  }

  // Check running flag before processing - exit early if stopped
  MUTEX_LOCK(g_discovery->mutex);
  if (!g_discovery) {
    MUTEX_UNLOCK(g_discovery->mutex);
    return;
  }
  bool is_running = g_discovery->running;
  void *browse_ref = g_discovery->browse_ref;
  dnssd_t *dnssd = g_discovery->dnssd;
  MUTEX_UNLOCK(g_discovery->mutex);

  if (!is_running || !browse_ref || !dnssd) {
    return;
  }

  // Process browse events (this may block briefly, but should return quickly)
  // Note: DNSServiceProcessResult can block, but should return quickly if no events
  int err = dnssd_process_result(dnssd, browse_ref);

  // Check g_discovery again after potentially blocking call - it might have been freed
  if (err != 0 && g_discovery && g_discovery->logger) {
    logger_log(g_discovery->logger, LOGGER_DEBUG, "airplay_discovery: DNSServiceProcessResult error: %d", err);
  }

  // Also process resolve events for any pending resolves
  // Collect refs while holding mutex, then process outside mutex to avoid deadlock
  void **resolve_refs = NULL;
  void **addrinfo_refs = NULL;
  int ref_count = 0;
  int capacity = 0;

  // Check g_discovery exists before locking
  if (!g_discovery) {
    return;
  }

  MUTEX_LOCK(g_discovery->mutex);
  if (!g_discovery || !g_discovery->running || !g_discovery->dnssd) {
    MUTEX_UNLOCK(g_discovery->mutex);
    return;
  }

  // Count refs first
  receiver_node_t *node = g_discovery->receivers;
  while (node) {
    if (node->resolve_ref || node->addrinfo_ref) {
      ref_count++;
    }
    node = node->next;
  }

  if (ref_count > 0) {
    capacity = ref_count * 2; // Space for both resolve and addrinfo refs
    resolve_refs = (void **)malloc(capacity * sizeof(void *));
    addrinfo_refs = (void **)malloc(capacity * sizeof(void *));
    if (resolve_refs && addrinfo_refs) {
      int idx = 0;
      node = g_discovery->receivers;
      while (node) {
        if (node->resolve_ref) {
          resolve_refs[idx] = node->resolve_ref;
          addrinfo_refs[idx] = NULL;
          idx++;
        }
        if (node->addrinfo_ref) {
          resolve_refs[idx] = NULL;
          addrinfo_refs[idx] = node->addrinfo_ref;
          idx++;
        }
        node = node->next;
      }
      ref_count = idx;
    }
  }
  MUTEX_UNLOCK(g_discovery->mutex);

  // Process refs outside mutex to avoid deadlock if DNSServiceProcessResult blocks
  if (resolve_refs && addrinfo_refs) {
    for (int i = 0; i < ref_count; i++) {
      // Check if we should stop before each call - g_discovery might be freed
      if (!g_discovery) {
        break;
      }
      MUTEX_LOCK(g_discovery->mutex);
      if (!g_discovery) {
        MUTEX_UNLOCK(g_discovery->mutex);
        break;
      }
      bool should_continue = g_discovery->running && g_discovery->dnssd;
      dnssd_t *current_dnssd = g_discovery->dnssd;
      MUTEX_UNLOCK(g_discovery->mutex);
      if (!should_continue || !current_dnssd) {
        break;
      }

      if (resolve_refs[i]) {
        dnssd_process_result(current_dnssd, resolve_refs[i]);
      }
      if (addrinfo_refs[i]) {
        dnssd_process_result(current_dnssd, addrinfo_refs[i]);
      }
    }
    free(resolve_refs);
    free(addrinfo_refs);
  }
}
