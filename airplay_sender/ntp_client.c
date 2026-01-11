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
#include <sys/time.h>
#include <errno.h>

#include "../airplay/compat.h"
#include "../airplay/sockets.h"
#include "../airplay/byteutils.h"
#include "ntp_client.h"

#define NTP_DATA_COUNT 8
#define NTP_REQUEST_SIZE 32
#define NTP_RESPONSE_SIZE 128
#define NTP_TIMEOUT_MS 300

struct ntp_client_s {
  int socket_fd;
  struct sockaddr_storage remote_addr;
  socklen_t remote_addr_len;
  int connected;

  // NTP sync data
  int64_t offset;  // Clock offset in microseconds
  int64_t delay;   // Round-trip delay in microseconds
  int64_t offset_history[NTP_DATA_COUNT];
  int offset_index;
  int sync_count;
};

static uint64_t
get_local_time_us(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static int
create_udp_socket(void)
{
  int sockfd;
  struct timeval timeout;

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    return -1;
  }

  // Set receive timeout
  timeout.tv_sec = 0;
  timeout.tv_usec = NTP_TIMEOUT_MS * 1000;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

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

ntp_client_t *
ntp_client_init(void)
{
  ntp_client_t *ntp;

  ntp = calloc(1, sizeof(ntp_client_t));
  if (!ntp) {
    return NULL;
  }

  ntp->socket_fd = -1;
  ntp->connected = 0;
  ntp->offset = 0;
  ntp->delay = 0;
  ntp->offset_index = 0;
  ntp->sync_count = 0;

  // Initialize offset history
  for (int i = 0; i < NTP_DATA_COUNT; i++) {
    ntp->offset_history[i] = 0;
  }

  return ntp;
}

int
ntp_client_connect(ntp_client_t *ntp, const char *host, uint16_t port)
{
  assert(ntp);
  assert(host);

  if (ntp->connected) {
    return 0;
  }

  ntp->socket_fd = create_udp_socket();
  if (ntp->socket_fd == -1) {
    fprintf(stderr, "ntp_client: failed to create UDP socket\n");
    return -1;
  }

  if (connect_udp_to_host(host, port, &ntp->remote_addr, &ntp->remote_addr_len) != 0) {
    fprintf(stderr, "ntp_client: failed to resolve host %s:%u\n", host, port);
    closesocket(ntp->socket_fd);
    ntp->socket_fd = -1;
    return -1;
  }

  ntp->connected = 1;
  return 0;
}

int
ntp_client_sync(ntp_client_t *ntp)
{
  unsigned char request[NTP_REQUEST_SIZE];
  unsigned char response[NTP_RESPONSE_SIZE];
  int response_len;
  uint64_t t0, t1, t2, t3;
  int64_t offset_calc, delay_calc;

  assert(ntp);

  if (!ntp->connected || ntp->socket_fd == -1) {
    return -1;
  }

  // Build NTP request packet
  // Format: {0x80, 0xd2, 0x00, 0x07, ...} with timestamp at offset 24
  memset(request, 0, sizeof(request));
  request[0] = 0x80;
  request[1] = 0xd2;
  request[2] = 0x00;
  request[3] = 0x07;

  // t0: Local time when request is sent
  t0 = get_local_time_us();
  byteutils_put_ntp_timestamp(request, 24, t0);

  // Send request
  if (sendto(ntp->socket_fd, request, sizeof(request), 0,
             (struct sockaddr *)&ntp->remote_addr, ntp->remote_addr_len) != sizeof(request)) {
    fprintf(stderr, "ntp_client: failed to send NTP request\n");
    return -1;
  }

  // Receive response
  response_len = recvfrom(ntp->socket_fd, response, sizeof(response), 0,
                          (struct sockaddr *)&ntp->remote_addr, &ntp->remote_addr_len);

  if (response_len < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      fprintf(stderr, "ntp_client: NTP response timeout (receiver may not be responding to client requests)\n");
    } else {
      fprintf(stderr, "ntp_client: recvfrom failed: %s (errno=%d)\n", strerror(errno), errno);
    }
    return -1;
  }

  if (response_len < 32) {
    fprintf(stderr, "ntp_client: invalid NTP response (len=%d)\n", response_len);
    return -1;
  }

  // t3: Local time when response is received
  t3 = get_local_time_us();

  // Parse response timestamps
  // t0: Client send time (from request, already have it)
  // t1: Server receive time (from response offset 8)
  // t2: Server send time (from response offset 16)
  // t3: Client receive time (just measured)

  t1 = byteutils_get_ntp_timestamp(response, 8);
  t2 = byteutils_get_ntp_timestamp(response, 16);

  // Calculate offset and delay using NTP algorithm
  // offset = ((t1 - t0) + (t2 - t3)) / 2
  // delay = (t3 - t0) - (t2 - t1)
  offset_calc = ((int64_t)(t1 - t0) + (int64_t)(t2 - t3)) / 2;
  delay_calc = (int64_t)(t3 - t0) - (int64_t)(t2 - t1);

  // Store offset in history
  ntp->offset_history[ntp->offset_index] = offset_calc;
  ntp->offset_index = (ntp->offset_index + 1) % NTP_DATA_COUNT;
  ntp->sync_count++;

  // Calculate average offset (simple average for now)
  int64_t offset_sum = 0;
  int count = ntp->sync_count < NTP_DATA_COUNT ? ntp->sync_count : NTP_DATA_COUNT;
  for (int i = 0; i < count; i++) {
    offset_sum += ntp->offset_history[i];
  }
  ntp->offset = offset_sum / count;
  ntp->delay = delay_calc;

  return 0;
}

uint64_t
ntp_client_get_local_time(ntp_client_t *ntp)
{
  assert(ntp);
  return get_local_time_us();
}

uint64_t
ntp_client_convert_remote_time(ntp_client_t *ntp, uint64_t remote_ntp)
{
  assert(ntp);

  // Convert remote time to local time by subtracting offset
  // remote_ntp is in microseconds, offset is in microseconds
  return remote_ntp - (uint64_t)ntp->offset;
}

uint64_t
ntp_client_convert_to_ntp(ntp_client_t *ntp, uint64_t local_time)
{
  assert(ntp);

  // Convert local time to remote NTP time by adding offset
  // For video packets, AirPlay uses timestamps relative to boot (no epoch offset)
  // So we just add the offset to align with receiver's clock
  return local_time + (uint64_t)ntp->offset;
}

int64_t
ntp_client_get_offset(ntp_client_t *ntp)
{
  assert(ntp);
  return ntp->offset;
}

void
ntp_client_disconnect(ntp_client_t *ntp)
{
  assert(ntp);

  if (ntp->socket_fd != -1) {
    closesocket(ntp->socket_fd);
    ntp->socket_fd = -1;
  }
  ntp->connected = 0;
}

void
ntp_client_destroy(ntp_client_t *ntp)
{
  if (ntp) {
    ntp_client_disconnect(ntp);
    free(ntp);
  }
}