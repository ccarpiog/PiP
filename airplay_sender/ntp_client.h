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

#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntp_client_s ntp_client_t;

/**
 * Initialize NTP client
 * Returns NTP client instance or NULL on error
 */
ntp_client_t *ntp_client_init(void);

/**
 * Connect to receiver's timing port
 * host: Receiver hostname or IP address
 * port: Receiver's timing port (UDP)
 * Returns 0 on success, -1 on error
 */
int ntp_client_connect(ntp_client_t *ntp, const char *host, uint16_t port);

/**
 * Perform NTP synchronization (send request and process response)
 * Should be called periodically (e.g., once per second)
 * Returns 0 on success, -1 on error
 */
int ntp_client_sync(ntp_client_t *ntp);

/**
 * Get current local time in microseconds since Unix epoch
 */
uint64_t ntp_client_get_local_time(ntp_client_t *ntp);

/**
 * Convert remote NTP timestamp to local time
 * remote_ntp: Remote NTP timestamp in microseconds
 * Returns local time in microseconds
 */
uint64_t ntp_client_convert_remote_time(ntp_client_t *ntp, uint64_t remote_ntp);

/**
 * Convert local time to NTP format (for video packets)
 * local_time: Local time in microseconds since Unix epoch
 * Returns NTP timestamp in microseconds (may not include epoch offset)
 */
uint64_t ntp_client_convert_to_ntp(ntp_client_t *ntp, uint64_t local_time);

/**
 * Get current clock offset (difference between remote and local clocks)
 * Returns offset in microseconds
 */
int64_t ntp_client_get_offset(ntp_client_t *ntp);

/**
 * Disconnect from receiver
 */
void ntp_client_disconnect(ntp_client_t *ntp);

/**
 * Clean up NTP client
 */
void ntp_client_destroy(ntp_client_t *ntp);

#ifdef __cplusplus
}
#endif

#endif