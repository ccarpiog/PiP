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

#include "../airplay/pairing.h"
#include "../airplay/aes_ctr.h"
#include "../airplay/ed25519/ed25519.h"
#include "../airplay/ed25519/sha512.h"
#include "../airplay/curve25519/curve25519.h"
#include "pairing_client.h"
#include "http_client.h"

#define SALT_KEY "Pair-Verify-AES-Key"
#define SALT_IV "Pair-Verify-AES-IV"

typedef enum {
  PAIRING_STATE_INIT,
  PAIRING_STATE_SETUP_DONE,
  PAIRING_STATE_VERIFY_STEP1_DONE,
  PAIRING_STATE_VERIFY_DONE
} pairing_state_t;

struct pairing_client_s {
  pairing_state_t state;

  // Ed25519 keypair (persistent)
  unsigned char ed_private[ED25519_KEY_SIZE];
  unsigned char ed_public[ED25519_KEY_SIZE];
  unsigned char ed_receiver_public[ED25519_KEY_SIZE];

  // X25519 keypair (ephemeral for verify)
  unsigned char ecdh_private[X25519_KEY_SIZE];
  unsigned char ecdh_public[X25519_KEY_SIZE];
  unsigned char ecdh_receiver_public[X25519_KEY_SIZE];
  unsigned char ecdh_secret[X25519_KEY_SIZE];
};

static int
derive_key(pairing_client_t *client, const unsigned char *salt,
           unsigned int saltlen, unsigned char *key, unsigned int keylen)
{
  unsigned char hash[SHA512_DIGEST_LENGTH];

  if (keylen > sizeof(hash)) {
    return -1;
  }

  sha512_context ctx;
  sha512_init(&ctx);
  sha512_update(&ctx, salt, saltlen);
  sha512_update(&ctx, client->ecdh_secret, X25519_KEY_SIZE);
  sha512_final(&ctx, hash);

  memcpy(key, hash, keylen);
  return 0;
}

pairing_client_t *
pairing_client_init(void)
{
  pairing_client_t *client;
  unsigned char seed[ED25519_KEY_SIZE];

  client = calloc(1, sizeof(pairing_client_t));
  if (!client) {
    return NULL;
  }

  if (ed25519_create_seed(seed)) {
    free(client);
    return NULL;
  }

  ed25519_create_keypair(client->ed_public, client->ed_private, seed);
  client->state = PAIRING_STATE_INIT;

  return client;
}

int
pairing_client_setup(pairing_client_t *client, http_client_t *http_client)
{
  http_client_t *client_obj = http_client;
  http_client_response_t *response;

  assert(client);
  assert(http_client);

  if (client->state != PAIRING_STATE_INIT) {
    return -1;
  }

  // Send our Ed25519 public key (32 bytes)
  response = http_client_request(client_obj, "POST", "/pair-setup",
                                 "Content-Type: application/octet-stream\r\n"
                                 "X-Apple-ProtocolVersion: 1\r\n",
                                 (const char *)client->ed_public,
                                 ED25519_KEY_SIZE);

  if (!response || response->status_code != 200) {
    fprintf(stderr, "pairing_client: pair-setup failed, status=%d\n",
            response ? response->status_code : -1);
    if (response) {
      http_client_response_destroy(response);
    }
    return -1;
  }

  if (!response->body || response->body_len != ED25519_KEY_SIZE) {
    fprintf(stderr, "pairing_client: invalid pair-setup response (len=%d)\n",
            response->body_len);
    http_client_response_destroy(response);
    return -1;
  }

  // Store receiver's Ed25519 public key
  memcpy(client->ed_receiver_public, response->body, ED25519_KEY_SIZE);
  client->state = PAIRING_STATE_SETUP_DONE;

  http_client_response_destroy(response);
  return 0;
}

int
pairing_client_verify_step1(pairing_client_t *client, http_client_t *http_client)
{
  http_client_t *client_obj = http_client;
  http_client_response_t *response;
  unsigned char request[4 + X25519_KEY_SIZE + ED25519_KEY_SIZE];
  unsigned char ecdh_priv[X25519_KEY_SIZE];

  assert(client);
  assert(http_client);

  if (client->state != PAIRING_STATE_SETUP_DONE) {
    return -1;
  }

  // Generate ephemeral X25519 keypair
  if (ed25519_create_seed(ecdh_priv)) {
    return -1;
  }
  memcpy(client->ecdh_private, ecdh_priv, X25519_KEY_SIZE);
  curve25519_donna(client->ecdh_public, ecdh_priv, kCurve25519BasePoint);

  // Build request: [0x01, 0x00, 0x00, 0x00, X25519_pub, Ed25519_pub]
  request[0] = 0x01;  // Verify mode 1
  request[1] = 0x00;
  request[2] = 0x00;
  request[3] = 0x00;
  memcpy(request + 4, client->ecdh_public, X25519_KEY_SIZE);
  memcpy(request + 4 + X25519_KEY_SIZE, client->ed_public, ED25519_KEY_SIZE);

  // Debug: log what we're sending
  fprintf(stderr, "pairing_client: sending pair-verify step1 request, request[0]=0x%02x, size=%zu\n",
          request[0], sizeof(request));
  fprintf(stderr, "pairing_client: first 8 bytes of request: ");
  for (int i = 0; i < 8 && i < (int)sizeof(request); i++) {
    fprintf(stderr, "%02x ", request[i]);
  }
  fprintf(stderr, "\n");

  response = http_client_request(client_obj, "POST", "/pair-verify",
                                 "Content-Type: application/octet-stream\r\n"
                                 "X-Apple-ProtocolVersion: 1\r\n",
                                 (const char *)request, sizeof(request));

  if (!response || response->status_code != 200) {
    fprintf(stderr, "pairing_client: pair-verify step1 failed, status=%d\n",
            response ? response->status_code : -1);
    if (response) {
      http_client_response_destroy(response);
    }
    return -1;
  }

  // Response: [X25519_pub (32 bytes), signature (64 bytes)]
  if (!response->body || response->body_len != X25519_KEY_SIZE + PAIRING_SIG_SIZE) {
    fprintf(stderr, "pairing_client: invalid pair-verify step1 response (len=%d)\n",
            response->body_len);
    http_client_response_destroy(response);
    return -1;
  }

  // Store receiver's X25519 public key
  memcpy(client->ecdh_receiver_public, response->body, X25519_KEY_SIZE);

  // Compute shared secret
  curve25519_donna(client->ecdh_secret, client->ecdh_private,
                   client->ecdh_receiver_public);

  // Verify receiver's signature
  unsigned char signature[PAIRING_SIG_SIZE];
  unsigned char sig_msg[PAIRING_SIG_SIZE];
  unsigned char key[AES_128_BLOCK_SIZE];
  unsigned char iv[AES_128_BLOCK_SIZE];
  AES_CTR_CTX aes_ctx;

  memcpy(signature, response->body + X25519_KEY_SIZE, PAIRING_SIG_SIZE);

  // Decrypt signature with keys derived from shared secret
  derive_key(client, (const unsigned char *)SALT_KEY, strlen(SALT_KEY),
             key, sizeof(key));
  derive_key(client, (const unsigned char *)SALT_IV, strlen(SALT_IV),
             iv, sizeof(iv));

  AES_ctr_set_key(&aes_ctx, key, iv, AES_MODE_128);
  // Decrypt signature (no fake round - receiver doesn't use one when encrypting for real device compatibility)
  unsigned char sig_buffer[PAIRING_SIG_SIZE];
  AES_ctr_encrypt(&aes_ctx, signature, sig_buffer, PAIRING_SIG_SIZE);

  // Verify signature: sign(ecdh_receiver || ecdh_ours)
  memcpy(sig_msg, client->ecdh_receiver_public, X25519_KEY_SIZE);
  memcpy(sig_msg + X25519_KEY_SIZE, client->ecdh_public, X25519_KEY_SIZE);

  // Debug: print first 8 bytes of decrypted signature and message
  fprintf(stderr, "pairing_client: decrypted signature (first 8 bytes): ");
  for (int i = 0; i < 8; i++) {
    fprintf(stderr, "%02x ", sig_buffer[i]);
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "pairing_client: signature message (first 8 bytes): ");
  for (int i = 0; i < 8; i++) {
    fprintf(stderr, "%02x ", sig_msg[i]);
  }
  fprintf(stderr, "\n");

  if (!ed25519_verify(sig_buffer, sig_msg, sizeof(sig_msg),
                      client->ed_receiver_public)) {
    fprintf(stderr, "pairing_client: receiver signature verification failed\n");
    http_client_response_destroy(response);
    return -1;
  }

  client->state = PAIRING_STATE_VERIFY_STEP1_DONE;
  http_client_response_destroy(response);
  return 0;
}

int
pairing_client_verify_step2(pairing_client_t *client, http_client_t *http_client)
{
  http_client_t *client_obj = http_client;
  http_client_response_t *response;
  unsigned char request[4 + PAIRING_SIG_SIZE];
  // sig_msg holds the message to be signed: ecdh_public || ecdh_receiver_public (2 * 32 = 64 bytes)
  unsigned char sig_msg[PAIRING_SIG_SIZE];
  unsigned char key[AES_128_BLOCK_SIZE];
  unsigned char iv[AES_128_BLOCK_SIZE];
  AES_CTR_CTX aes_ctx;

  assert(client);
  assert(http_client);

  if (client->state != PAIRING_STATE_VERIFY_STEP1_DONE) {
    return -1;
  }

  // Build request header: [0x00, 0x00, 0x00, 0x00, ...]
  request[0] = 0x00;  // Verify mode 0
  request[1] = 0x00;
  request[2] = 0x00;
  request[3] = 0x00;

  // Sign: sign(ecdh_ours || ecdh_receiver) directly into request buffer
  memcpy(sig_msg, client->ecdh_public, X25519_KEY_SIZE);
  memcpy(sig_msg + X25519_KEY_SIZE, client->ecdh_receiver_public, X25519_KEY_SIZE);

  ed25519_sign(request + 4, sig_msg, sizeof(sig_msg),
               client->ed_public, client->ed_private);

  // Encrypt signature in-place with keys derived from shared secret
  derive_key(client, (const unsigned char *)SALT_KEY, strlen(SALT_KEY),
             key, sizeof(key));
  derive_key(client, (const unsigned char *)SALT_IV, strlen(SALT_IV),
             iv, sizeof(iv));

  AES_ctr_set_key(&aes_ctx, key, iv, AES_MODE_128);
  // One fake round for the initial handshake encryption
  unsigned char sig_buffer[PAIRING_SIG_SIZE];
  AES_ctr_encrypt(&aes_ctx, sig_buffer, sig_buffer, PAIRING_SIG_SIZE);
  // Encrypt signature in-place (after fake round)
  AES_ctr_encrypt(&aes_ctx, request + 4, request + 4, PAIRING_SIG_SIZE);

  response = http_client_request(client_obj, "POST", "/pair-verify",
                                 "Content-Type: application/octet-stream\r\n"
                                 "X-Apple-ProtocolVersion: 1\r\n",
                                 (const char *)request, sizeof(request));

  if (!response || response->status_code != 200) {
    fprintf(stderr, "pairing_client: pair-verify step2 failed, status=%d\n",
            response ? response->status_code : -1);
    if (response) {
      http_client_response_destroy(response);
    }
    return -1;
  }

  client->state = PAIRING_STATE_VERIFY_DONE;
  http_client_response_destroy(response);
  return 0;
}

int
pairing_client_get_shared_secret(pairing_client_t *client,
                                 unsigned char secret[X25519_KEY_SIZE])
{
  assert(client);

  if (client->state < PAIRING_STATE_VERIFY_STEP1_DONE) {
    return -1;
  }

  memcpy(secret, client->ecdh_secret, X25519_KEY_SIZE);
  return 0;
}

void
pairing_client_destroy(pairing_client_t *client)
{
  if (client) {
    // Clear sensitive data
    memset(client->ed_private, 0, sizeof(client->ed_private));
    memset(client->ecdh_private, 0, sizeof(client->ecdh_private));
    memset(client->ecdh_secret, 0, sizeof(client->ecdh_secret));
    free(client);
  }
}