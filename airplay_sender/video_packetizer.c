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

#include "../airplay/compat.h"
#include "../airplay/aes_ctr.h"
#include "../airplay/byteutils.h"
#include "../airplay/crypto/crypto.h"
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
  AES_CTR_CTX aes_ctx;
  int encryption_initialized;
  uint8_t *packet_buffer;
  int packet_buffer_size;
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

  AES_ctr_set_key(&pkt->aes_ctx, key, iv, AES_MODE_128);
  pkt->encryption_initialized = 1;

  return 0;
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

  if (!pkt->encryption_initialized) {
    fprintf(stderr, "video_packetizer: encryption not initialized\n");
    return -1;
  }

  // Convert Annex-B to AVCC if needed (check if data is already AVCC)
  // AVCC format has 4-byte length prefixes, Annex-B has start codes
  if (data_len >= 4 && (data[0] != 0 || data[1] != 0 || data[2] != 0 || data[3] != 1)) {
    // Likely already in AVCC format, use directly
    avcc_data = (uint8_t *)data;
    avcc_len = data_len;
  } else {
    // Convert from Annex-B to AVCC
    if (convert_annex_b_to_avcc(data, data_len, &avcc_data, &avcc_len) != 0) {
      return -1;
    }
  }

  // Build payload: SPS + PPS (if keyframe) + frame data
  if (is_keyframe && sps && sps_len > 0 && pps && pps_len > 0) {
    // Send SPS/PPS as separate packet first (type 0x01)
    payload_len = 4 + sps_len + 4 + pps_len;
    payload = malloc(payload_len);
    if (!payload) {
      if (avcc_data != data) {
        free(avcc_data);
      }
      return -1;
    }

    uint8_t *dst = payload;
    uint32_t len_be;

    // SPS with length prefix
    len_be = htonl((uint32_t)sps_len);
    memcpy(dst, &len_be, 4);
    dst += 4;
    memcpy(dst, sps, sps_len);
    dst += sps_len;

    // PPS with length prefix
    len_be = htonl((uint32_t)pps_len);
    memcpy(dst, &len_be, 4);
    dst += 4;
    memcpy(dst, pps, pps_len);

    // Send SPS/PPS packet
    memset(header, 0, sizeof(header));
    uint32_t payload_size_be = htonl((uint32_t)payload_len);
    memcpy(header, &payload_size_be, 4);
    header[4] = 0x01;  // SPS/PPS packet type
    header[5] = 0x00;
    header[6] = 0x01;
    header[7] = 0x16;
    byteutils_put_ntp_timestamp(header, 8, ntp_timestamp);

    if (pkt->callback) {
      // Allocate packet buffer
      total_packet_len = VIDEO_PACKET_HEADER_SIZE + payload_len;
      if (pkt->packet_buffer_size < total_packet_len) {
        pkt->packet_buffer = realloc(pkt->packet_buffer, total_packet_len);
        if (!pkt->packet_buffer) {
          free(payload);
          if (avcc_data != data) {
            free(avcc_data);
          }
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
  }

  // Now send the actual frame data
  payload = (uint8_t *)avcc_data;
  payload_len = avcc_len;

  // Encrypt payload
  encrypted_payload = malloc(payload_len);
  if (!encrypted_payload) {
    if (avcc_data != data) {
      free(avcc_data);
    }
    return -1;
  }

  AES_ctr_encrypt(&pkt->aes_ctx, payload, encrypted_payload, payload_len);

  // Build header for encrypted video packet
  memset(header, 0, sizeof(header));
  uint32_t payload_size_be = htonl((uint32_t)payload_len);
  memcpy(header, &payload_size_be, 4);
  header[4] = 0x00;  // Encrypted video packet
  header[5] = 0x10;
  header[6] = 0x00;
  header[7] = 0x00;
  byteutils_put_ntp_timestamp(header, 8, ntp_timestamp);

  // Send packet
  if (pkt->callback) {
    total_packet_len = VIDEO_PACKET_HEADER_SIZE + payload_len;
    if (pkt->packet_buffer_size < total_packet_len) {
      pkt->packet_buffer = realloc(pkt->packet_buffer, total_packet_len);
      if (!pkt->packet_buffer) {
        free(encrypted_payload);
        if (avcc_data != data) {
          free(avcc_data);
        }
        return -1;
      }
      pkt->packet_buffer_size = total_packet_len;
    }

    memcpy(pkt->packet_buffer, header, VIDEO_PACKET_HEADER_SIZE);
    memcpy(pkt->packet_buffer + VIDEO_PACKET_HEADER_SIZE, encrypted_payload, payload_len);

    pkt->callback(pkt->packet_buffer, total_packet_len, pkt->callback_ctx);
  }

  free(encrypted_payload);
  if (avcc_data != data) {
    free(avcc_data);
  }

  return 0;
}

void
video_packetizer_destroy(video_packetizer_t *pkt)
{
  if (pkt) {
    if (pkt->packet_buffer) {
      free(pkt->packet_buffer);
    }
    free(pkt);
  }
}