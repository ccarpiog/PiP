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

#ifndef PAIRING_CLIENT_H
#define PAIRING_CLIENT_H

#include <stdint.h>
#include "../airplay/pairing.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pairing_client_s pairing_client_t;

/**
 * Initialize pairing client and generate Ed25519 keypair
 */
pairing_client_t *pairing_client_init(void);

struct http_client_s;

/**
 * Perform pair-setup: exchange Ed25519 public keys with receiver
 * Returns 0 on success, -1 on error
 */
int pairing_client_setup(pairing_client_t *client, struct http_client_s *http_client);

/**
 * Perform pair-verify step 1: send X25519 and Ed25519 public keys
 * Returns 0 on success, -1 on error
 */
int pairing_client_verify_step1(pairing_client_t *client, struct http_client_s *http_client);

/**
 * Perform pair-verify step 2: send signature and verify receiver's signature
 * Returns 0 on success, -1 on error
 */
int pairing_client_verify_step2(pairing_client_t *client, struct http_client_s *http_client);

/**
 * Get the X25519 shared secret (for AES key derivation)
 * Returns 0 on success, -1 if not available
 */
int pairing_client_get_shared_secret(pairing_client_t *client,
                                     unsigned char secret[X25519_KEY_SIZE]);

/**
 * Clean up pairing client
 */
void pairing_client_destroy(pairing_client_t *client);

#ifdef __cplusplus
}
#endif

#endif