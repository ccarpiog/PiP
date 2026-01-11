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
#include <time.h>

#include "../airplay/compat.h"
#include "../airplay/sockets.h"
#include "../airplay/byteutils.h"
#include "rtp_audio.h"

#define RTP_VERSION 2
#define RTP_PAYLOAD_TYPE_AAC 96  // Dynamic payload type (can vary)
#define RTP_HEADER_SIZE 12
#define RTP_SSRC_MASK 0x7fffffff

struct rtp_audio_s {
  int sample_rate;
  int socket_fd;
  struct sockaddr_storage remote_addr;
  socklen_t remote_addr_len;
  int connected;

  // RTP state
  uint16_t sequence_number;
  uint32_t ssrc;
  uint64_t stream_start_ntp;  // NTP timestamp when stream started
  uint32_t rtp_timestamp_base;  // RTP timestamp at stream start
};

static int
create_udp_socket(void)
{
  int sockfd;

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    return -1;
  }

  return sockfd;
}

static int
connect_udp_to_host(const char *host, uint16_t port, struct sockaddr_storage *addr, socklen_t *addr_len)
{
  struct addrinfo hints, *result, *rp;
  char port_str[6];
  int ret = -1;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  snprintf(port_str, sizeof(port_str), "%u", port);

  if (getaddrinfo(host, port_str, &hints, &result) != 0) {
    return -1;
  }

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    if (rp->ai_family == AF_INET || rp->ai_family == AF_INET6) {
      memcpy(addr, rp->ai_addr, rp->ai_addrlen);
      *addr_len = rp->ai_addrlen;
      ret = 0;
      break;
    }
  }

  freeaddrinfo(result);
  return ret;
}

rtp_audio_t *
rtp_audio_init(int sample_rate)
{
  rtp_audio_t *rtp;

  rtp = calloc(1, sizeof(rtp_audio_t));
  if (!rtp) {
    return NULL;
  }

  rtp->sample_rate = sample_rate;
  rtp->socket_fd = -1;
  rtp->connected = 0;
  rtp->sequence_number = (uint16_t)(rand() & 0xffff);
  rtp->ssrc = (uint32_t)rand() & RTP_SSRC_MASK;
  rtp->stream_start_ntp = 0;
  rtp->rtp_timestamp_base = 0;

  return rtp;
}

int
rtp_audio_connect(rtp_audio_t *rtp, const char *host, uint16_t port)
{
  assert(rtp);
  assert(host);

  if (rtp->connected) {
    return 0;
  }

  rtp->socket_fd = create_udp_socket();
  if (rtp->socket_fd == -1) {
    fprintf(stderr, "rtp_audio: failed to create UDP socket\n");
    return -1;
  }

  if (connect_udp_to_host(host, port, &rtp->remote_addr, &rtp->remote_addr_len) != 0) {
    fprintf(stderr, "rtp_audio: failed to resolve host %s:%u\n", host, port);
    closesocket(rtp->socket_fd);
    rtp->socket_fd = -1;
    return -1;
  }

  rtp->connected = 1;
  return 0;
}

int
rtp_audio_send(rtp_audio_t *rtp, const uint8_t *data, int data_len, uint64_t pts)
{
  uint8_t rtp_header[RTP_HEADER_SIZE];
  uint8_t *packet;
  int packet_len;
  uint32_t rtp_timestamp;
  int sent;

  assert(rtp);
  assert(data);
  assert(data_len > 0);

  if (!rtp->connected || rtp->socket_fd == -1) {
    return -1;
  }

  // Initialize stream start time on first packet
  if (rtp->stream_start_ntp == 0) {
    rtp->stream_start_ntp = pts;
    rtp->rtp_timestamp_base = 0;
  }

  // Calculate RTP timestamp: (pts - stream_start) * sample_rate / 1000000
  // RTP timestamp is in units of 1/sample_rate seconds
  uint64_t time_offset = pts - rtp->stream_start_ntp;
  rtp_timestamp = rtp->rtp_timestamp_base + (uint32_t)((time_offset * rtp->sample_rate) / 1000000ULL);

  // Build RTP header (RFC 3550)
  memset(rtp_header, 0, RTP_HEADER_SIZE);

  // Byte 0: V=2, P=0, X=0, CC=0
  rtp_header[0] = (RTP_VERSION << 6);

  // Byte 1: M=0, PT=payload_type
  rtp_header[1] = RTP_PAYLOAD_TYPE_AAC & 0x7f;

  // Bytes 2-3: Sequence number (big-endian)
  uint16_t seq_be = htons(rtp->sequence_number);
  memcpy(rtp_header + 2, &seq_be, 2);

  // Bytes 4-7: RTP timestamp (big-endian)
  uint32_t ts_be = htonl(rtp_timestamp);
  memcpy(rtp_header + 4, &ts_be, 4);

  // Bytes 8-11: SSRC (big-endian)
  uint32_t ssrc_be = htonl(rtp->ssrc);
  memcpy(rtp_header + 8, &ssrc_be, 4);

  // Build complete packet
  packet_len = RTP_HEADER_SIZE + data_len;
  packet = malloc(packet_len);
  if (!packet) {
    return -1;
  }

  memcpy(packet, rtp_header, RTP_HEADER_SIZE);
  memcpy(packet + RTP_HEADER_SIZE, data, data_len);

  // Send packet
  sent = sendto(rtp->socket_fd, packet, packet_len, 0,
                (struct sockaddr *)&rtp->remote_addr, rtp->remote_addr_len);

  free(packet);

  if (sent != packet_len) {
    fprintf(stderr, "rtp_audio: failed to send packet (sent %d of %d)\n", sent, packet_len);
    return -1;
  }

  // Increment sequence number
  rtp->sequence_number++;

  return 0;
}

void
rtp_audio_disconnect(rtp_audio_t *rtp)
{
  assert(rtp);

  if (rtp->socket_fd != -1) {
    closesocket(rtp->socket_fd);
    rtp->socket_fd = -1;
  }
  rtp->connected = 0;
}

void
rtp_audio_destroy(rtp_audio_t *rtp)
{
  if (rtp) {
    rtp_audio_disconnect(rtp);
    free(rtp);
  }
}