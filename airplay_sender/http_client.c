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
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>

#ifndef WIN32
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#define gai_strerror(x) "getaddrinfo error"
#endif

#include "../airplay/compat.h"
#include "../airplay/sockets.h"
#include "../airplay/llhttp/llhttp.h"
#include "../airplay/plist/plist/plist.h"
#include "http_client.h"

#define HTTP_CLIENT_BUFFER_SIZE 4096
#define HTTP_CLIENT_TIMEOUT_SEC 10

struct http_client_s {
  char *host;
  uint16_t port;
  int socket_fd;
  int connected;
};

struct http_response_parser_s {
  llhttp_t parser;
  llhttp_settings_t parser_settings;

  int status_code;
  char *headers;
  int headers_size;
  int headers_length;
  char *body;
  int body_len;
  int complete;
  int protocol_replaced;  // Track if we've already replaced RTSP with HTTP
};

static int
on_status(llhttp_t *parser, const char *at, size_t length)
{
  return 0;
}

static int
on_header_field(llhttp_t *parser, const char *at, size_t length)
{
  struct http_response_parser_s *resp_parser = (struct http_response_parser_s *)parser->data;
  int current_len = resp_parser->headers_length;

  // Allocate space for: existing data + new data + ": " + null terminator
  resp_parser->headers = realloc(resp_parser->headers, current_len + length + 2 + 1);
  if (!resp_parser->headers) {
    return -1;
  }

  memcpy(resp_parser->headers + current_len, at, length);
  resp_parser->headers[current_len + length] = ':';
  resp_parser->headers[current_len + length + 1] = ' ';
  resp_parser->headers_length += length + 2;
  resp_parser->headers[resp_parser->headers_length] = '\0';

  return 0;
}

static int
on_header_value(llhttp_t *parser, const char *at, size_t length)
{
  struct http_response_parser_s *resp_parser = (struct http_response_parser_s *)parser->data;
  int current_len = resp_parser->headers_length;

  // Allocate space for: existing data + new data + "\r\n" + null terminator
  resp_parser->headers = realloc(resp_parser->headers, current_len + length + 2 + 1);
  if (!resp_parser->headers) {
    return -1;
  }

  memcpy(resp_parser->headers + current_len, at, length);
  resp_parser->headers[current_len + length] = '\r';
  resp_parser->headers[current_len + length + 1] = '\n';
  resp_parser->headers_length += length + 2;
  resp_parser->headers[resp_parser->headers_length] = '\0';

  return 0;
}

static int
on_body(llhttp_t *parser, const char *at, size_t length)
{
  struct http_response_parser_s *resp_parser = (struct http_response_parser_s *)parser->data;
  size_t new_size;
  char *old_body;

  if (!at || length == 0) {
    return 0;
  }

  fprintf(stderr, "http_client: on_body called with length=%zu, current body_len=%d\n", length, resp_parser->body_len);

  // Check for overflow
  if (resp_parser->body_len < 0) {
    fprintf(stderr, "http_client: body_len is negative: %d\n", resp_parser->body_len);
    return -1;
  }

  new_size = (size_t)resp_parser->body_len + length;
  if (new_size < (size_t)resp_parser->body_len) {
    fprintf(stderr, "http_client: size overflow detected\n");
    return -1;
  }

  old_body = resp_parser->body;
  resp_parser->body = realloc(resp_parser->body, new_size);
  if (!resp_parser->body) {
    fprintf(stderr, "http_client: realloc failed for body (size=%zu)\n", new_size);
    return -1;
  }

  // Validate the source pointer is reasonable
  if ((uintptr_t)at < 0x1000 || (uintptr_t)at > 0x7fffffffffff) {
    fprintf(stderr, "http_client: suspicious source pointer: %p\n", at);
    return -1;
  }

  // Debug: log first few bytes of what we're copying
  if (length > 0) {
    fprintf(stderr, "http_client: copying %zu bytes from %p, first 8 bytes: ", length, at);
    for (size_t i = 0; i < 8 && i < length; i++) {
      fprintf(stderr, "%02x ", (unsigned char)at[i]);
    }
    fprintf(stderr, "\n");
  }

  memcpy(resp_parser->body + resp_parser->body_len, at, length);
  resp_parser->body_len = (int)new_size;

  // Debug: verify what we copied
  if (resp_parser->body_len > 0) {
    fprintf(stderr, "http_client: body_len now=%d, first 8 bytes of accumulated body: ", resp_parser->body_len);
    for (int i = 0; i < 8 && i < resp_parser->body_len; i++) {
      fprintf(stderr, "%02x ", (unsigned char)resp_parser->body[i]);
    }
    fprintf(stderr, "\n");
  }

  return 0;
}

static int
on_message_complete(llhttp_t *parser)
{
  struct http_response_parser_s *resp_parser = (struct http_response_parser_s *)parser->data;

  resp_parser->status_code = parser->status_code;
  resp_parser->complete = 1;
  return 0;
}

static int
connect_to_host(const char *host, uint16_t port)
{
  struct addrinfo hints, *result, *rp;
  int sockfd = -1;
  char port_str[6];
  int ret;
  struct timeval timeout;
  fd_set write_fds;
  int error;
  socklen_t error_len;
  int getaddrinfo_error;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  snprintf(port_str, sizeof(port_str), "%u", port);

  getaddrinfo_error = getaddrinfo(host, port_str, &hints, &result);
  if (getaddrinfo_error != 0) {
    fprintf(stderr, "http_client: getaddrinfo failed for %s:%u: %s\n",
            host, port, gai_strerror(getaddrinfo_error));
    return -1;
  }

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sockfd == -1) {
      continue;
    }

    // Set socket to non-blocking mode
#ifndef WIN32
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
      closesocket(sockfd);
      sockfd = -1;
      continue;
    }
#else
    u_long mode = 1;
    if (ioctlsocket(sockfd, FIONBIO, &mode) != 0) {
      closesocket(sockfd);
      sockfd = -1;
      continue;
    }
#endif

    // Attempt connection (will return immediately with non-blocking socket)
    ret = connect(sockfd, rp->ai_addr, rp->ai_addrlen);

    if (ret == 0) {
      // Connected immediately
      goto connected;
    }

#ifndef WIN32
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
      closesocket(sockfd);
      sockfd = -1;
      continue;
    }
#else
    if (WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINPROGRESS) {
      closesocket(sockfd);
      sockfd = -1;
      continue;
    }
#endif

    // Wait for connection with timeout
    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);
    timeout.tv_sec = HTTP_CLIENT_TIMEOUT_SEC;
    timeout.tv_usec = 0;

    ret = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
    if (ret <= 0 || !FD_ISSET(sockfd, &write_fds)) {
      // Timeout or error
      closesocket(sockfd);
      sockfd = -1;
      continue;
    }

    // Check if connection succeeded
    error_len = sizeof(error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&error, &error_len) == 0) {
      if (error == 0) {
        // Connection successful
        goto connected;
      } else {
        fprintf(stderr, "http_client: connection failed to %s:%u: error %d\n", host, port, error);
      }
    } else {
      fprintf(stderr, "http_client: getsockopt failed for %s:%u\n", host, port);
    }

    // Connection failed
    closesocket(sockfd);
    sockfd = -1;
    continue;

connected:
    // Set socket back to blocking mode
#ifndef WIN32
    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags != -1) {
      fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK);
    }
#else
    mode = 0;
    ioctlsocket(sockfd, FIONBIO, &mode);
#endif

    // Set socket timeouts for send/recv
    timeout.tv_sec = HTTP_CLIENT_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

    freeaddrinfo(result);
    return sockfd;
  }

  freeaddrinfo(result);
  fprintf(stderr, "http_client: failed to connect to %s:%u after trying all addresses\n", host, port);
  return -1;
}

http_client_t *
http_client_init(const char *host, uint16_t port)
{
  http_client_t *client;

  assert(host);

  client = calloc(1, sizeof(http_client_t));
  if (!client) {
    return NULL;
  }

  client->host = strdup(host);
  if (!client->host) {
    free(client);
    return NULL;
  }

  client->port = port;
  client->socket_fd = -1;
  client->connected = 0;

  return client;
}

int
http_client_connect(http_client_t *client)
{
  assert(client);

  if (client->connected) {
    return 0;
  }

  client->socket_fd = connect_to_host(client->host, client->port);
  if (client->socket_fd == -1) {
    return -1;
  }

  // Small delay to ensure connection is fully established
  // This helps with localhost connections that might have timing issues
  usleep(10000); // 10ms delay

  client->connected = 1;
  return 0;
}

http_client_response_t *
http_client_request(http_client_t *client, const char *method, const char *path,
                    const char *headers, const char *body, int body_len)
{
  char request[8192];
  int request_len;
  int sent;
  char buffer[HTTP_CLIENT_BUFFER_SIZE];
  int received;
  struct http_response_parser_s resp_parser;
  http_client_response_t *response;
  int ret;

  assert(client);
  assert(method);
  assert(path);

  if (!client->connected) {
    if (http_client_connect(client) != 0) {
      return NULL;
    }
  }

  // Use HTTP/1.1 in request (llhttp parser expects HTTP, not RTSP)
  // But include CSeq header which is required for RTSP/AirPlay
  // HTTP/1.1 requires Host header
  static int cseq_counter = 1;
  int cseq = cseq_counter++;

  request_len = snprintf(request, sizeof(request),
                         "%s %s HTTP/1.1\r\n"
                         "Host: %s:%u\r\n"
                         "CSeq: %d\r\n"
                         "User-Agent: AirPlay/320.20\r\n",
                         method, path, client->host, client->port, cseq);

  if (headers) {
    int headers_len = strlen(headers);
    if (request_len + headers_len < sizeof(request) - 2) {
      memcpy(request + request_len, headers, headers_len);
      request_len += headers_len;
    }
  }

  if (body && body_len > 0) {
    char content_length[32];
    int clen = snprintf(content_length, sizeof(content_length),
                        "Content-Length: %d\r\n", body_len);
    if (request_len + clen < sizeof(request) - 2) {
      memcpy(request + request_len, content_length, clen);
      request_len += clen;
    }
  }

  // RTSP/HTTP requests must end with \r\n\r\n (empty line)
  if (request_len + 4 < sizeof(request)) {
    memcpy(request + request_len, "\r\n\r\n", 4);
    request_len += 4;
  } else {
    return NULL;
  }

  // Debug: log the request being sent
  fprintf(stderr, "http_client: sending request (%d bytes):\n%.*s\n", request_len, request_len, request);

  sent = 0;
  while (sent < request_len) {
    ret = send(client->socket_fd, request + sent, request_len - sent, 0);
    if (ret == -1) {
      fprintf(stderr, "http_client: send failed: %s\n", strerror(errno));
      http_client_disconnect(client);
      return NULL;
    }
    sent += ret;
  }

  fprintf(stderr, "http_client: sent %d bytes\n", sent);

  if (body && body_len > 0) {
    sent = 0;
    while (sent < body_len) {
      ret = send(client->socket_fd, body + sent, body_len - sent, 0);
      if (ret == -1) {
        http_client_disconnect(client);
        return NULL;
      }
      sent += ret;
    }
  }

  // Initialize parser structure - don't use memset as it might interfere with parser internals
  resp_parser.status_code = 0;
  resp_parser.headers = NULL;
  resp_parser.headers_size = 0;
  resp_parser.headers_length = 0;
  resp_parser.body = NULL;
  resp_parser.body_len = 0;
  resp_parser.complete = 0;
  resp_parser.protocol_replaced = 0;

  llhttp_settings_init(&resp_parser.parser_settings);
  resp_parser.parser_settings.on_status = &on_status;
  resp_parser.parser_settings.on_header_field = &on_header_field;
  resp_parser.parser_settings.on_header_value = &on_header_value;
  resp_parser.parser_settings.on_body = &on_body;
  resp_parser.parser_settings.on_message_complete = &on_message_complete;

  llhttp_init(&resp_parser.parser, HTTP_RESPONSE, &resp_parser.parser_settings);
  resp_parser.parser.data = &resp_parser;

  while (!resp_parser.complete) {
    char *parse_buffer;

    received = recv(client->socket_fd, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      if (received == 0) {
        fprintf(stderr, "http_client: connection closed by peer\n");
      } else {
        fprintf(stderr, "http_client: recv error: %s\n", strerror(errno));
      }
      http_client_disconnect(client);
      return NULL;
    }

    fprintf(stderr, "http_client: received %d bytes of response\n", received);

    // Create a copy of the buffer for the parser to avoid any issues with
    // the parser storing pointers into the buffer
    parse_buffer = malloc(received);
    if (!parse_buffer) {
      fprintf(stderr, "http_client: failed to allocate parse buffer\n");
      http_client_disconnect(client);
      return NULL;
    }
    memcpy(parse_buffer, buffer, received);

    // Replace RTSP/1.0 with HTTP/1.0 ONLY in the status line (before first \r\n)
    // The llhttp parser expects HTTP, not RTSP, but the formats are identical
    // We must only replace in the status line to avoid corrupting binary plist data in body
    if (!resp_parser.protocol_replaced && received >= 8) {
      // Find the end of the status line (first \r\n) to ensure we only replace in status line
      int status_line_end = -1;
      for (int i = 0; i < received - 1; i++) {
        if (parse_buffer[i] == '\r' && parse_buffer[i + 1] == '\n') {
          status_line_end = i;
          break;
        }
      }

      // Only replace if we found the status line boundary and the replacement is within it
      // The status line format is: "RTSP/1.0 200 OK\r\n" or "RTSP/1.1 200 OK\r\n"
      if (status_line_end >= 7 && strncmp(parse_buffer, "RTSP/1.", 7) == 0) {
        // Safe to modify: we're only changing "RTSP" to "HTTP" (4 bytes)
        // This works for both RTSP/1.0 and RTSP/1.1
        memcpy(parse_buffer, "HTTP", 4);
        resp_parser.protocol_replaced = 1;
        fprintf(stderr, "http_client: replaced RTSP/1. with HTTP/1. in response status line\n");
      }
    }

    ret = llhttp_execute(&resp_parser.parser, parse_buffer, received);
    free(parse_buffer);

    if (ret != 0) {
      fprintf(stderr, "http_client: parser error: %d\n", ret);
      http_client_disconnect(client);
      return NULL;
    }
  }

  fprintf(stderr, "http_client: response complete, status: %d\n", resp_parser.status_code);

  response = calloc(1, sizeof(http_client_response_t));
  if (!response) {
    return NULL;
  }

  response->status_code = resp_parser.status_code;
  response->headers = resp_parser.headers;
  response->body = resp_parser.body;
  response->body_len = resp_parser.body_len;

  return response;
}

void
http_client_disconnect(http_client_t *client)
{
  assert(client);

  if (client->socket_fd != -1) {
    closesocket(client->socket_fd);
    client->socket_fd = -1;
  }
  client->connected = 0;
}

void
http_client_destroy(http_client_t *client)
{
  if (client) {
    http_client_disconnect(client);
    free(client->host);
    free(client);
  }
}

void
http_client_response_destroy(http_client_response_t *response)
{
  if (response) {
    free(response->headers);
    free(response->body);
    free(response);
  }
}

server_info_t *
http_client_get_info(http_client_t *client)
{
  http_client_response_t *response;
  plist_t root_node = NULL;
  plist_t node;
  server_info_t *info;

  assert(client);

  response = http_client_request(client, "GET", "/info", NULL, NULL, 0);
  if (!response || response->status_code != 200) {
    if (response) {
      http_client_response_destroy(response);
    }
    return NULL;
  }

  if (!response->body || response->body_len == 0) {
    http_client_response_destroy(response);
    return NULL;
  }

  // Validate that body starts with binary plist magic
  if (response->body_len < 8 || memcmp(response->body, "bplist00", 8) != 0) {
    fprintf(stderr, "http_client: invalid plist data (length=%d, magic=%.8s)\n",
            response->body_len, response->body_len >= 8 ? response->body : "too short");
    http_client_response_destroy(response);
    return NULL;
  }

  fprintf(stderr, "http_client: parsing plist (length=%d, first 16 bytes: ", response->body_len);
  for (int i = 0; i < 16 && i < response->body_len; i++) {
    fprintf(stderr, "%02x ", (unsigned char)response->body[i]);
  }
  fprintf(stderr, ")\n");

  plist_from_bin(response->body, response->body_len, &root_node);
  if (!root_node) {
    http_client_response_destroy(response);
    return NULL;
  }

  info = calloc(1, sizeof(server_info_t));
  if (!info) {
    plist_free(root_node);
    http_client_response_destroy(response);
    return NULL;
  }

  node = plist_dict_get_item(root_node, "features");
  if (node && plist_get_node_type(node) == PLIST_UINT) {
    uint64_t val;
    plist_get_uint_val(node, &val);
    info->features = val;
    printf("http_client: extracted features: 0x%llx\n", (unsigned long long)info->features);
  }

  node = plist_dict_get_item(root_node, "sourceVersion");
  if (node && plist_get_node_type(node) == PLIST_STRING) {
    char *val;
    plist_get_string_val(node, &val);
    info->sourceVersion = val;
    printf("http_client: extracted sourceVersion: %s\n", info->sourceVersion ? info->sourceVersion : "(null)");
  }

  node = plist_dict_get_item(root_node, "deviceID");
  if (node && plist_get_node_type(node) == PLIST_STRING) {
    char *val;
    plist_get_string_val(node, &val);
    info->deviceID = val;
    printf("http_client: extracted deviceID: %s\n", info->deviceID ? info->deviceID : "(null)");
  }

  /* Parse audioFormats array */
  node = plist_dict_get_item(root_node, "audioFormats");
  if (node && PLIST_IS_ARRAY(node)) {
    uint32_t count = plist_array_get_size(node);
    printf("http_client: found audioFormats array with %u entries\n", count);
    if (count > 0) {
      info->audioFormats = calloc(count, sizeof(audio_format_t));
      if (info->audioFormats) {
        info->audioFormatsCount = 0;
        for (uint32_t i = 0; i < count; i++) {
          plist_t format_node = plist_array_get_item(node, i);
          if (format_node && PLIST_IS_DICT(format_node)) {
            plist_t type_node = plist_dict_get_item(format_node, "type");
            if (type_node && plist_get_node_type(type_node) == PLIST_UINT) {
              plist_get_uint_val(type_node, &info->audioFormats[info->audioFormatsCount].type);
            }
            plist_t input_node = plist_dict_get_item(format_node, "audioInputFormats");
            if (input_node && plist_get_node_type(input_node) == PLIST_UINT) {
              plist_get_uint_val(input_node, &info->audioFormats[info->audioFormatsCount].audioInputFormats);
            }
            plist_t output_node = plist_dict_get_item(format_node, "audioOutputFormats");
            if (output_node && plist_get_node_type(output_node) == PLIST_UINT) {
              plist_get_uint_val(output_node, &info->audioFormats[info->audioFormatsCount].audioOutputFormats);
            }
            printf("http_client: audioFormat[%d]: type=%llu, inputFormats=0x%llx, outputFormats=0x%llx\n",
                   info->audioFormatsCount,
                   (unsigned long long)info->audioFormats[info->audioFormatsCount].type,
                   (unsigned long long)info->audioFormats[info->audioFormatsCount].audioInputFormats,
                   (unsigned long long)info->audioFormats[info->audioFormatsCount].audioOutputFormats);
            info->audioFormatsCount++;
          }
        }
        printf("http_client: extracted %d audio format(s)\n", info->audioFormatsCount);
      }
    }
  } else {
    printf("http_client: no audioFormats array found\n");
  }

  /* Parse displays array */
  node = plist_dict_get_item(root_node, "displays");
  if (node && PLIST_IS_ARRAY(node)) {
    uint32_t count = plist_array_get_size(node);
    printf("http_client: found displays array with %u entries\n", count);
    if (count > 0) {
      info->displays = calloc(count, sizeof(display_t));
      if (info->displays) {
        info->displaysCount = 0;
        for (uint32_t i = 0; i < count; i++) {
          plist_t display_node = plist_array_get_item(node, i);
          if (display_node && PLIST_IS_DICT(display_node)) {
            plist_t uuid_node = plist_dict_get_item(display_node, "uuid");
            if (uuid_node && plist_get_node_type(uuid_node) == PLIST_STRING) {
              char *val;
              plist_get_string_val(uuid_node, &val);
              info->displays[info->displaysCount].uuid = val;
            }
            plist_t width_phys_node = plist_dict_get_item(display_node, "widthPhysical");
            if (width_phys_node && plist_get_node_type(width_phys_node) == PLIST_BOOLEAN) {
              uint8_t val;
              plist_get_bool_val(width_phys_node, &val);
              info->displays[info->displaysCount].widthPhysical = val;
            }
            plist_t height_phys_node = plist_dict_get_item(display_node, "heightPhysical");
            if (height_phys_node && plist_get_node_type(height_phys_node) == PLIST_BOOLEAN) {
              uint8_t val;
              plist_get_bool_val(height_phys_node, &val);
              info->displays[info->displaysCount].heightPhysical = val;
            }
            plist_t width_node = plist_dict_get_item(display_node, "width");
            if (width_node && plist_get_node_type(width_node) == PLIST_UINT) {
              plist_get_uint_val(width_node, &info->displays[info->displaysCount].width);
            }
            plist_t height_node = plist_dict_get_item(display_node, "height");
            if (height_node && plist_get_node_type(height_node) == PLIST_UINT) {
              plist_get_uint_val(height_node, &info->displays[info->displaysCount].height);
            }
            plist_t width_pixels_node = plist_dict_get_item(display_node, "widthPixels");
            if (width_pixels_node && plist_get_node_type(width_pixels_node) == PLIST_UINT) {
              plist_get_uint_val(width_pixels_node, &info->displays[info->displaysCount].widthPixels);
            }
            plist_t height_pixels_node = plist_dict_get_item(display_node, "heightPixels");
            if (height_pixels_node && plist_get_node_type(height_pixels_node) == PLIST_UINT) {
              plist_get_uint_val(height_pixels_node, &info->displays[info->displaysCount].heightPixels);
            }
            plist_t rotation_node = plist_dict_get_item(display_node, "rotation");
            if (rotation_node && plist_get_node_type(rotation_node) == PLIST_BOOLEAN) {
              uint8_t val;
              plist_get_bool_val(rotation_node, &val);
              info->displays[info->displaysCount].rotation = val;
            }
            plist_t refresh_rate_node = plist_dict_get_item(display_node, "refreshRate");
            if (refresh_rate_node && plist_get_node_type(refresh_rate_node) == PLIST_UINT) {
              plist_get_uint_val(refresh_rate_node, &info->displays[info->displaysCount].refreshRate);
            }
            plist_t max_fps_node = plist_dict_get_item(display_node, "maxFPS");
            if (max_fps_node && plist_get_node_type(max_fps_node) == PLIST_UINT) {
              plist_get_uint_val(max_fps_node, &info->displays[info->displaysCount].maxFPS);
            }
            plist_t overscanned_node = plist_dict_get_item(display_node, "overscanned");
            if (overscanned_node && plist_get_node_type(overscanned_node) == PLIST_BOOLEAN) {
              uint8_t val;
              plist_get_bool_val(overscanned_node, &val);
              info->displays[info->displaysCount].overscanned = val;
            }
            plist_t features_node = plist_dict_get_item(display_node, "features");
            if (features_node && plist_get_node_type(features_node) == PLIST_UINT) {
              plist_get_uint_val(features_node, &info->displays[info->displaysCount].features);
            }
            printf("http_client: display[%d]: uuid=%s, width=%llu, height=%llu, widthPixels=%llu, heightPixels=%llu, refreshRate=%llu, maxFPS=%llu, overscanned=%u, features=0x%llx\n",
                   info->displaysCount,
                   info->displays[info->displaysCount].uuid ? info->displays[info->displaysCount].uuid : "(null)",
                   (unsigned long long)info->displays[info->displaysCount].width,
                   (unsigned long long)info->displays[info->displaysCount].height,
                   (unsigned long long)info->displays[info->displaysCount].widthPixels,
                   (unsigned long long)info->displays[info->displaysCount].heightPixels,
                   (unsigned long long)info->displays[info->displaysCount].refreshRate,
                   (unsigned long long)info->displays[info->displaysCount].maxFPS,
                   info->displays[info->displaysCount].overscanned,
                   (unsigned long long)info->displays[info->displaysCount].features);
            info->displaysCount++;
          }
        }
        printf("http_client: extracted %d display(s)\n", info->displaysCount);
      }
    }
  } else {
    printf("http_client: no displays array found\n");
  }

  plist_free(root_node);
  http_client_response_destroy(response);

  return info;
}

void
server_info_destroy(server_info_t *info)
{
  if (info) {
    free(info->sourceVersion);
    free(info->deviceID);
    if (info->audioFormats) {
      free(info->audioFormats);
    }
    if (info->displays) {
      for (int i = 0; i < info->displaysCount; i++) {
        free(info->displays[i].uuid);
      }
      free(info->displays);
    }
    free(info);
  }
}
