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

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "fairplay_client.h"
#include "http_client.h"
#include "../airplay/playfair/playfair.h"

typedef enum {
  FAIRPLAY_STATE_INIT,
  FAIRPLAY_STATE_SETUP_DONE,
  FAIRPLAY_STATE_HANDSHAKE_DONE
} fairplay_state_t;

struct fairplay_client_s {
  fairplay_state_t state;
  unsigned char setup_response[FAIRPLAY_SETUP_RESPONSE_SIZE];
  unsigned char session_key[FAIRPLAY_SESSION_KEY_SIZE];
  unsigned char handshake_message[FAIRPLAY_HANDSHAKE_SIZE];  // message3 for encryption
};

fairplay_client_t *
fairplay_client_init(void)
{
  fairplay_client_t *client;

  client = calloc(1, sizeof(fairplay_client_t));
  if (!client) {
    return NULL;
  }

  client->state = FAIRPLAY_STATE_INIT;

  return client;
}

int
fairplay_client_setup(fairplay_client_t *client, http_client_t *http_client)
{
  http_client_t *client_obj = http_client;
  http_client_response_t *response;
  unsigned char challenge[FAIRPLAY_CHALLENGE_SIZE];

  assert(client);
  assert(http_client);

  if (client->state != FAIRPLAY_STATE_INIT) {
    return -1;
  }

  // Generate challenge: "FPLY" header + version + mode
  // Format based on receiver code: byte 4 = 0x03 (version), byte 14 = mode (0-3)
  memset(challenge, 0, sizeof(challenge));
  challenge[0] = 0x46;  // 'F'
  challenge[1] = 0x50;  // 'P'
  challenge[2] = 0x4c;  // 'L'
  challenge[3] = 0x59;  // 'Y'
  challenge[4] = 0x03;  // Version
  challenge[14] = 0x00;  // Mode (try mode 0 first)

  response = http_client_request(client_obj, "POST", "/fp-setup",
                                 "Content-Type: application/octet-stream\r\n",
                                 (const char *)challenge,
                                 FAIRPLAY_CHALLENGE_SIZE);

  if (!response || response->status_code != 200) {
    fprintf(stderr, "fairplay_client: fp-setup failed, status=%d\n",
            response ? response->status_code : -1);
    if (response) {
      http_client_response_destroy(response);
    }
    return -1;
  }

  if (!response->body || response->body_len != FAIRPLAY_SETUP_RESPONSE_SIZE) {
    fprintf(stderr, "fairplay_client: invalid fp-setup response (len=%d)\n",
            response->body_len);
    http_client_response_destroy(response);
    return -1;
  }

  // Store setup response for handshake
  memcpy(client->setup_response, response->body, FAIRPLAY_SETUP_RESPONSE_SIZE);
  client->state = FAIRPLAY_STATE_SETUP_DONE;

  http_client_response_destroy(response);
  return 0;
}

int
fairplay_client_handshake(fairplay_client_t *client, http_client_t *http_client)
{
  http_client_t *client_obj = http_client;
  http_client_response_t *response;
  unsigned char handshake[FAIRPLAY_HANDSHAKE_SIZE];

  assert(client);
  assert(http_client);

  if (client->state != FAIRPLAY_STATE_SETUP_DONE) {
    return -1;
  }

  // Build 164-byte handshake message
  // Based on receiver code, the format appears to be:
  // - Header (12 bytes): {0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x14}
  // - Followed by data derived from setup response
  memset(handshake, 0, sizeof(handshake));
  handshake[0] = 0x46;  // 'F'
  handshake[1] = 0x50;  // 'P'
  handshake[2] = 0x4c;  // 'L'
  handshake[3] = 0x59;  // 'Y'
  handshake[4] = 0x03;  // Version
  handshake[5] = 0x01;
  handshake[6] = 0x04;
  handshake[7] = 0x00;
  handshake[8] = 0x00;
  handshake[9] = 0x00;
  handshake[10] = 0x00;
  handshake[11] = 0x14;

  // Copy relevant parts from setup response
  // The receiver code shows: memcpy(res + 12, req + 144, 20);
  // This suggests bytes 144-163 of the handshake are used in the response
  // For now, we'll use the setup response data to construct the handshake
  // This may need refinement based on actual protocol analysis
  memcpy(handshake + 12, client->setup_response + 12, 152);

  // Store handshake message for later use in encryption
  memcpy(client->handshake_message, handshake, FAIRPLAY_HANDSHAKE_SIZE);

  response = http_client_request(client_obj, "POST", "/fp-setup",
                                 "Content-Type: application/octet-stream\r\n",
                                 (const char *)handshake,
                                 FAIRPLAY_HANDSHAKE_SIZE);

  if (!response || response->status_code != 200) {
    fprintf(stderr, "fairplay_client: fp-setup handshake failed, status=%d\n",
            response ? response->status_code : -1);
    if (response) {
      http_client_response_destroy(response);
    }
    return -1;
  }

  if (!response->body || response->body_len != FAIRPLAY_SESSION_KEY_SIZE) {
    fprintf(stderr, "fairplay_client: invalid fp-setup handshake response (len=%d)\n",
            response->body_len);
    http_client_response_destroy(response);
    return -1;
  }

  // Store session key material
  memcpy(client->session_key, response->body, FAIRPLAY_SESSION_KEY_SIZE);
  client->state = FAIRPLAY_STATE_HANDSHAKE_DONE;

  http_client_response_destroy(response);
  return 0;
}

int
fairplay_client_get_session_key(fairplay_client_t *client,
                                unsigned char key[FAIRPLAY_SESSION_KEY_SIZE])
{
  assert(client);

  if (client->state != FAIRPLAY_STATE_HANDSHAKE_DONE) {
    return -1;
  }

  memcpy(key, client->session_key, FAIRPLAY_SESSION_KEY_SIZE);
  return 0;
}

int
fairplay_client_encrypt_key(fairplay_client_t *client,
                            const unsigned char key_in[16],
                            unsigned char ekey_out[72])
{
  assert(client);
  assert(key_in);
  assert(ekey_out);

  if (client->state != FAIRPLAY_STATE_HANDSHAKE_DONE) {
    fprintf(stderr, "fairplay_client: cannot encrypt key - handshake not done\n");
    return -1;
  }

  // Use playfair_encrypt to encrypt the key
  playfair_encrypt((unsigned char*)client->handshake_message, (unsigned char*)key_in, ekey_out);

  return 0;
}

void
fairplay_client_destroy(fairplay_client_t *client)
{
  if (client) {
    // Clear sensitive data
    memset(client->session_key, 0, sizeof(client->session_key));
    free(client);
  }
}