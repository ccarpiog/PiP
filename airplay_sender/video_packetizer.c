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
#include <inttypes.h>

#include "../airplay/compat.h"
#include "../airplay/aes.h"
#include "../airplay/byteutils.h"
#include "../airplay/crypto/crypto.h"
#include "../airplay/threads.h"
#include "video_packetizer.h"

#ifndef htonl
#include <arpa/inet.h>
#endif

#define VIDEO_PACKET_HEADER_SIZE 128
#define AES_KEY_SIZE 16
#define AES_IV_SIZE 16

struct video_packetizer_s {
  packetized_data_callback_t callback;
  void *callback_ctx;
  struct AES_ctx aes_ctx;
  int encryption_initialized;
  uint8_t *packet_buffer;
  int packet_buffer_size;
  // Partial block state (matching receiver's mirror_buffer behavior)
  int nextEncryptCount;  // Number of stored bytes from previous partial block
  uint8_t og[16];        // Stored bytes from previous partial block
  mutex_handle_t packetize_mutex;  // Mutex to serialize packetize calls
  int sps_pps_sent;      // Track if SPS/PPS has been sent (iPad style: send once)
};

static int
convert_annex_b_to_avcc(const uint8_t *annex_b_data, int annex_b_len,
                        uint8_t **avcc_data, int *avcc_len)
{
  const uint8_t *src = annex_b_data;
  const uint8_t *end = annex_b_data + annex_b_len;
  uint8_t *dst;
  int total_len = 0;
  int nal_count = 0;
  const uint8_t *nal_start;

  // First pass: count NAL units and calculate total size
  while (src < end) {
    // Find start code (0x00 0x00 0x00 0x01 or 0x00 0x00 0x01)
    if (src + 3 < end && src[0] == 0 && src[1] == 0 && src[2] == 0 && src[3] == 1) {
      src += 4;
      nal_start = src;
      // Find next start code or end
      while (src < end) {
        if (src + 3 < end && src[0] == 0 && src[1] == 0 && src[2] == 0 && src[3] == 1) {
          break;
        }
        src++;
      }
      int nal_len = src - nal_start;
      if (nal_len > 0) {
        total_len += 4 + nal_len;  // 4-byte length prefix + NAL data
        nal_count++;
      }
    } else if (src + 2 < end && src[0] == 0 && src[1] == 0 && src[2] == 1) {
      src += 3;
      nal_start = src;
      while (src < end) {
        if (src + 2 < end && src[0] == 0 && src[1] == 0 && src[2] == 1) {
          break;
        }
        src++;
      }
      int nal_len = src - nal_start;
      if (nal_len > 0) {
        total_len += 4 + nal_len;
        nal_count++;
      }
    } else {
      src++;
    }
  }

  if (nal_count == 0) {
    // No NAL units found, assume data is already in AVCC format
    *avcc_data = malloc(annex_b_len);
    if (!*avcc_data) {
      return -1;
    }
    memcpy(*avcc_data, annex_b_data, annex_b_len);
    *avcc_len = annex_b_len;
    return 0;
  }

  // Second pass: convert to AVCC
  *avcc_data = malloc(total_len);
  if (!*avcc_data) {
    return -1;
  }

  dst = *avcc_data;
  src = annex_b_data;

  while (src < end) {
    if (src + 3 < end && src[0] == 0 && src[1] == 0 && src[2] == 0 && src[3] == 1) {
      src += 4;
      nal_start = src;
      while (src < end) {
        if (src + 3 < end && src[0] == 0 && src[1] == 0 && src[2] == 0 && src[3] == 1) {
          break;
        }
        src++;
      }
      int nal_len = src - nal_start;
      if (nal_len > 0) {
        // Write 4-byte length prefix (big-endian)
        uint32_t len_be = htonl((uint32_t)nal_len);
        memcpy(dst, &len_be, 4);
        dst += 4;
        memcpy(dst, nal_start, nal_len);
        dst += nal_len;
      }
    } else if (src + 2 < end && src[0] == 0 && src[1] == 0 && src[2] == 1) {
      src += 3;
      nal_start = src;
      while (src < end) {
        if (src + 2 < end && src[0] == 0 && src[1] == 0 && src[2] == 1) {
          break;
        }
        src++;
      }
      int nal_len = src - nal_start;
      if (nal_len > 0) {
        uint32_t len_be = htonl((uint32_t)nal_len);
        memcpy(dst, &len_be, 4);
        dst += 4;
        memcpy(dst, nal_start, nal_len);
        dst += nal_len;
      }
    } else {
      src++;
    }
  }

  *avcc_len = dst - *avcc_data;
  return 0;
}

video_packetizer_t *
video_packetizer_init(void)
{
  video_packetizer_t *pkt;

  pkt = calloc(1, sizeof(video_packetizer_t));
  if (!pkt) {
    return NULL;
  }

  pkt->encryption_initialized = 0;
  pkt->packet_buffer = NULL;
  pkt->packet_buffer_size = 0;
  pkt->nextEncryptCount = 0;
  memset(pkt->og, 0, sizeof(pkt->og));
  pkt->sps_pps_sent = 0;  // SPS/PPS not sent yet
  MUTEX_CREATE(pkt->packetize_mutex);

  return pkt;
}

void
video_packetizer_set_callback(video_packetizer_t *pkt, packetized_data_callback_t cb, void *ctx)
{
  assert(pkt);
  pkt->callback = cb;
  pkt->callback_ctx = ctx;
}

int
video_packetizer_set_encryption(video_packetizer_t *pkt,
                                const uint8_t *key, const uint8_t *iv)
{
  assert(pkt);
  assert(key);
  assert(iv);

  // Use the same AES-CTR implementation as the receiver
  AES_init_ctx_iv(&pkt->aes_ctx, key, iv);
  pkt->encryption_initialized = 1;
  // Reset partial block state when encryption is reinitialized
  pkt->nextEncryptCount = 0;
  memset(pkt->og, 0, sizeof(pkt->og));

  // Debug: log key and IV for verification
  fprintf(stderr, "video_packetizer_set_encryption: key=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x, iv=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
          key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
          key[8], key[9], key[10], key[11], key[12], key[13], key[14], key[15],
          iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], iv[6], iv[7],
          iv[8], iv[9], iv[10], iv[11], iv[12], iv[13], iv[14], iv[15]);

  return 0;
}

/**
 * Encrypt buffer with partial block handling (matching receiver's mirror_buffer_decrypt behavior)
 * This ensures counter state stays synchronized across packets with partial blocks
 */
static void
encrypt_with_partial_block_handling(video_packetizer_t *pkt, const uint8_t *input, uint8_t *output, int inputLen)
{
  // Use stored bytes from previous partial block (if any)
  if (pkt->nextEncryptCount > 0) {
    for (int i = 0; i < pkt->nextEncryptCount; i++) {
      output[i] = (input[i] ^ pkt->og[(16 - pkt->nextEncryptCount) + i]);
    }
  }

  // Process full blocks
  int encryptlen = ((inputLen - pkt->nextEncryptCount) / 16) * 16;
  if (encryptlen > 0) {
    // Copy input to output, then encrypt output in place
    memcpy(output + pkt->nextEncryptCount, input + pkt->nextEncryptCount, encryptlen);
    AES_CTR_xcrypt_buffer(&pkt->aes_ctx, output + pkt->nextEncryptCount, encryptlen);
  }

  // Handle remaining partial block
  int restlen = (inputLen - pkt->nextEncryptCount) % 16;
  pkt->nextEncryptCount = 0;
  if (restlen > 0) {
    int reststart = inputLen - restlen;
    memset(pkt->og, 0, 16);
    memcpy(pkt->og, input + reststart, restlen);
    // Encrypt full 16-byte block (even though we only need restlen bytes)
    AES_CTR_xcrypt_buffer(&pkt->aes_ctx, pkt->og, 16);
    // Use only the bytes we need
    for (int j = 0; j < restlen; j++) {
      output[reststart + j] = pkt->og[j];
    }
    // Store unused bytes for next packet
    pkt->nextEncryptCount = 16 - restlen;
  }
}

int
video_packetizer_packetize(video_packetizer_t *pkt,
                           const uint8_t *data, int data_len,
                           bool is_keyframe,
                           const uint8_t *sps, int sps_len,
                           const uint8_t *pps, int pps_len,
                           uint64_t ntp_timestamp)
{
  uint8_t header[VIDEO_PACKET_HEADER_SIZE];
  uint8_t *avcc_data = NULL;
  int avcc_len = 0;
  uint8_t *payload = NULL;
  int payload_len = 0;
  uint8_t *encrypted_payload = NULL;
  int total_packet_len;
  int ret = -1;

  assert(pkt);
  assert(data);
  assert(data_len > 0);

  // Lock mutex to serialize access (VideoToolbox callbacks can be concurrent)
  MUTEX_LOCK(pkt->packetize_mutex);

  static int packetize_count = 0;

  if (!pkt->encryption_initialized) {
    fprintf(stderr, "video_packetizer: encryption not initialized\n");
    MUTEX_UNLOCK(pkt->packetize_mutex);
    return -1;
  }

  if (packetize_count == 0 || packetize_count % 30 == 0) {
    fprintf(stderr, "video_packetizer: packetize frame %d, len=%d, keyframe=%d, sps_len=%d, pps_len=%d\n",
            packetize_count, data_len, is_keyframe, sps_len, pps_len);
    if (data_len >= 8) {
      // Log first 8 bytes to see the format
      fprintf(stderr, "video_packetizer: first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
              data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
    }
  }

  // Convert Annex-B to AVCC if needed (check if data is already AVCC)
  // AVCC format has 4-byte length prefixes, Annex-B has start codes
  // VideoToolbox provides AVCC format with BIG-ENDIAN length prefixes already
  // We need to copy the data because encryption may modify it in place
  int avcc_was_allocated = 0;
  if (data_len >= 4 && (data[0] != 0 || data[1] != 0 || data[2] != 0 || data[3] != 1)) {
    // Already in AVCC format with big-endian length prefixes - copy it
    avcc_data = malloc(data_len);
    if (!avcc_data) {
      MUTEX_UNLOCK(pkt->packetize_mutex);
      return -1;
    }
    memcpy(avcc_data, data, data_len);
    avcc_len = data_len;
    avcc_was_allocated = 1;  // Allocated copy
  } else {
    // Convert from Annex-B to AVCC
    if (convert_annex_b_to_avcc(data, data_len, &avcc_data, &avcc_len) != 0) {
      MUTEX_UNLOCK(pkt->packetize_mutex);
      return -1;
    }
    avcc_was_allocated = 1;  // Allocated by convert_annex_b_to_avcc
  }

  // Build payload: SPS + PPS (if keyframe and not sent yet - iPad style: send once)
  int sps_pps_sent_this_call = 0;  // Track if SPS/PPS was sent in this call
  if (is_keyframe && sps && sps_len > 0 && pps && pps_len > 0 && !pkt->sps_pps_sent) {
    // Send SPS/PPS as separate packet first (type 0x01)
    // Receiver expects: 6-byte header, then SPS size (2 bytes BE) at offset 6, SPS data at offset 8,
    // then PPS size (2 bytes BE) at offset (sps_size + 9), PPS data at offset (sps_size + 11)
    payload_len = 6 + 2 + sps_len + 2 + pps_len;  // 6-byte header + 2-byte SPS size + SPS + 2-byte PPS size + PPS
    payload = malloc(payload_len);
    if (!payload) {
      if (avcc_data != data) {
        free(avcc_data);
      }
      MUTEX_UNLOCK(pkt->packetize_mutex);
      return -1;
    }

    uint8_t *dst = payload;

    // 6-byte header (first 6 bytes, initialized to 0)
    memset(dst, 0, 6);
    dst += 6;

    // SPS size (2 bytes, big-endian) at offset 6
    uint16_t sps_size_be = htons((uint16_t)sps_len);
    memcpy(dst, &sps_size_be, 2);
    dst += 2;

    // SPS data at offset 8
    memcpy(dst, sps, sps_len);
    dst += sps_len;

    // PPS size (2 bytes, big-endian) at offset (8 + sps_size + 1) = (sps_size + 9)
    // Need 1 byte gap between SPS data and PPS size
    dst += 1;
    uint16_t pps_size_be = htons((uint16_t)pps_len);
    memcpy(dst, &pps_size_be, 2);
    dst += 2;

    // PPS data at offset (8 + sps_size + 3) = (sps_size + 11)
    memcpy(dst, pps, pps_len);

    // Send SPS/PPS packet
    memset(header, 0, sizeof(header));
    // Receiver expects little-endian payload size (byteutils_get_int reads little-endian)
    uint32_t payload_size_le = (uint32_t)payload_len;
    memcpy(header, &payload_size_le, 4);
    header[4] = 0x01;  // SPS/PPS packet type
    header[5] = 0x00;
    header[6] = 0x01;
    header[7] = 0x16;
    byteutils_put_ntp_timestamp(header, 8, ntp_timestamp);

    if (pkt->callback) {
      // Allocate packet buffer
      total_packet_len = VIDEO_PACKET_HEADER_SIZE + payload_len;

      if (packetize_count == 0 || packetize_count % 30 == 0) {
        fprintf(stderr, "video_packetizer: sending SPS/PPS packet, len=%d\n", total_packet_len);
      }
      if (pkt->packet_buffer_size < total_packet_len) {
        pkt->packet_buffer = realloc(pkt->packet_buffer, total_packet_len);
        if (!pkt->packet_buffer) {
          free(payload);
          if (avcc_data != data) {
            free(avcc_data);
          }
          MUTEX_UNLOCK(pkt->packetize_mutex);
          return -1;
        }
        pkt->packet_buffer_size = total_packet_len;
      }

      memcpy(pkt->packet_buffer, header, VIDEO_PACKET_HEADER_SIZE);
      memcpy(pkt->packet_buffer + VIDEO_PACKET_HEADER_SIZE, payload, payload_len);

      pkt->callback(pkt->packet_buffer, total_packet_len, pkt->callback_ctx);
    }

    free(payload);
    payload = NULL;
    pkt->sps_pps_sent = 1;  // Mark SPS/PPS as sent (iPad style: only once)
    sps_pps_sent_this_call = 1;  // Track that we sent it in this call
  }

  // Now send each NAL unit as a separate packet (matching iPad behavior)
  // For keyframes, we need to strip SPS/PPS from the data since we sent them separately
  // Parse AVCC data and send each NAL unit individually
  int pos = 0;
  int nal_packet_count = 0;
  int is_first_video_packet = 1;  // Track if this is the first video packet after SPS/PPS

  while (pos < avcc_len) {
    if (pos + 4 > avcc_len) {
      // Not enough data for NAL length
      break;
    }

    // Read NAL unit length (big-endian)
    uint32_t nal_len = (uint32_t)avcc_data[pos] << 24 |
                       (uint32_t)avcc_data[pos + 1] << 16 |
                       (uint32_t)avcc_data[pos + 2] << 8 |
                       (uint32_t)avcc_data[pos + 3];

    if (pos + 4 + nal_len > avcc_len || nal_len == 0) {
      // Invalid NAL length
      break;
    }

    // Check NAL type (first 5 bits of NAL unit)
    uint8_t nal_type = (avcc_data[pos + 4]) & 0x1F;

    // Skip SPS (type 7) and PPS (type 8) for keyframes - we sent them separately
    if (is_keyframe && sps && sps_len > 0 && pps && pps_len > 0 && (nal_type == 7 || nal_type == 8)) {
      pos += 4 + nal_len;
      continue;
    }

    // Skip SEI (type 6) NALs - iPad doesn't send them (matching iPad style)
    if (nal_type == 6) {
      pos += 4 + nal_len;
      continue;
    }

    // Extract this NAL unit (including 4-byte length prefix)
    payload_len = 4 + nal_len;
    payload = malloc(payload_len);
    if (!payload) {
      if (avcc_was_allocated) {
        free(avcc_data);
      }
      MUTEX_UNLOCK(pkt->packetize_mutex);
      return -1;
    }
    memcpy(payload, avcc_data + pos, payload_len);

    // Encrypt this NAL unit
    encrypted_payload = malloc(payload_len);
    if (!encrypted_payload) {
      free(payload);
      if (avcc_was_allocated) {
        free(avcc_data);
      }
      MUTEX_UNLOCK(pkt->packetize_mutex);
      return -1;
    }

    // Copy payload to encrypted buffer, then encrypt with partial block handling
    // Note: encrypted_payload is both input and output
    memcpy(encrypted_payload, payload, payload_len);

    // Debug: log IV state before encryption (first few packets)
    if (packetize_count < 3 || packetize_count % 30 == 0) {
      fprintf(stderr, "video_packetizer: packet %d, NAL %d (type=%d), IV before encryption (last 4 bytes): %02x %02x %02x %02x, payload_len=%d, nextEncryptCount=%d\n",
              packetize_count, nal_packet_count, nal_type,
              pkt->aes_ctx.Iv[12], pkt->aes_ctx.Iv[13], pkt->aes_ctx.Iv[14], pkt->aes_ctx.Iv[15],
              payload_len, pkt->nextEncryptCount);
    }

    // Encrypt with partial block handling (matching receiver behavior)
    encrypt_with_partial_block_handling(pkt, payload, encrypted_payload, payload_len);

    // Debug: log IV state after encryption (first few packets)
    if (packetize_count < 3 || packetize_count % 30 == 0) {
      fprintf(stderr, "video_packetizer: packet %d, NAL %d (type=%d), IV after encryption (last 4 bytes): %02x %02x %02x %02x, blocks=%d, nextEncryptCount=%d\n",
              packetize_count, nal_packet_count, nal_type,
              pkt->aes_ctx.Iv[12], pkt->aes_ctx.Iv[13], pkt->aes_ctx.Iv[14], pkt->aes_ctx.Iv[15],
              (payload_len + 15) / 16, pkt->nextEncryptCount);
    }

    // Build header for encrypted video packet
    memset(header, 0, sizeof(header));
    // Receiver expects little-endian payload size (byteutils_get_int reads little-endian)
    uint32_t payload_size_le = (uint32_t)payload_len;
    memcpy(header, &payload_size_le, 4);
    header[4] = 0x00;  // Encrypted video packet

    // Set packet[5] = 0x10 if this is the first video packet after SPS/PPS (keyframe)
    // Otherwise use 0x00 for regular encrypted packets
    // Note: Only set 0x10 if SPS/PPS was just sent in this call (iPad style)
    if (is_first_video_packet && is_keyframe && sps_pps_sent_this_call) {
      header[5] = 0x10;  // First encrypted packet after SPS/PPS
      is_first_video_packet = 0;
    } else {
      header[5] = 0x00;  // Regular encrypted packet
    }
    header[6] = 0x00;
    header[7] = 0x00;
    byteutils_put_ntp_timestamp(header, 8, ntp_timestamp);

    if (packetize_count == 0 || packetize_count % 30 == 0) {
      fprintf(stderr, "video_packetizer: sending NAL %d (type=%d), header[5]=0x%02x, ntp_timestamp=%" PRIu64 "\n",
              nal_packet_count, nal_type, header[5], ntp_timestamp);
    }

    // Send packet
    if (pkt->callback) {
      // Allocate packet buffer
      total_packet_len = VIDEO_PACKET_HEADER_SIZE + payload_len;

      if (pkt->packet_buffer_size < total_packet_len) {
        pkt->packet_buffer = realloc(pkt->packet_buffer, total_packet_len);
        if (!pkt->packet_buffer) {
          free(encrypted_payload);
          free(payload);
          if (avcc_was_allocated) {
            free(avcc_data);
          }
          MUTEX_UNLOCK(pkt->packetize_mutex);
          return -1;
        }
        pkt->packet_buffer_size = total_packet_len;
      }

      memcpy(pkt->packet_buffer, header, VIDEO_PACKET_HEADER_SIZE);
      memcpy(pkt->packet_buffer + VIDEO_PACKET_HEADER_SIZE, encrypted_payload, payload_len);

      if (packetize_count == 0 || packetize_count % 30 == 0) {
        fprintf(stderr, "video_packetizer: sending video packet, len=%d\n", total_packet_len);
      }

      pkt->callback(pkt->packet_buffer, total_packet_len, pkt->callback_ctx);
    }

    free(encrypted_payload);
    free(payload);

    pos += 4 + nal_len;
    nal_packet_count++;
  }

  // Free AVCC data if we allocated it
  if (avcc_was_allocated) {
    free(avcc_data);
  }

  packetize_count++;
  MUTEX_UNLOCK(pkt->packetize_mutex);
  return 0;
}

void
video_packetizer_destroy(video_packetizer_t *pkt)
{
  if (pkt) {
    MUTEX_DESTROY(pkt->packetize_mutex);
    if (pkt->packet_buffer) {
      free(pkt->packet_buffer);
    }
    free(pkt);
  }
}