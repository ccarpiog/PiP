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

#ifndef FAIRPLAY_CLIENT_H
#define FAIRPLAY_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAIRPLAY_CHALLENGE_SIZE 16
#define FAIRPLAY_SETUP_RESPONSE_SIZE 142
#define FAIRPLAY_HANDSHAKE_SIZE 164
#define FAIRPLAY_SESSION_KEY_SIZE 32

typedef struct fairplay_client_s fairplay_client_t;

/**
 * Initialize FairPlay client
 */
fairplay_client_t *fairplay_client_init(void);

struct http_client_s;

/**
 * Perform FairPlay setup step 1: send challenge and receive response
 * Returns 0 on success, -1 on error
 */
int fairplay_client_setup(fairplay_client_t *client, struct http_client_s *http_client);

/**
 * Perform FairPlay setup step 2: send handshake and receive session key
 * Returns 0 on success, -1 on error
 */
int fairplay_client_handshake(fairplay_client_t *client, struct http_client_s *http_client);

/**
 * Get the session key material (32 bytes)
 * Returns 0 on success, -1 if not available
 */
int fairplay_client_get_session_key(fairplay_client_t *client,
                                    unsigned char key[FAIRPLAY_SESSION_KEY_SIZE]);

/**
 * Encrypt a 16-byte AES key into a 72-byte ekey using FairPlay
 * Requires handshake to be completed (message3 available)
 * Returns 0 on success, -1 on error
 */
int fairplay_client_encrypt_key(fairplay_client_t *client,
                                const unsigned char key_in[16],
                                unsigned char ekey_out[72]);

/**
 * Clean up FairPlay client
 */
void fairplay_client_destroy(fairplay_client_t *client);

#ifdef __cplusplus
}
#endif

#endif