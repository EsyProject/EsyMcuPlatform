/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "esy_middleware/mqtt/esy_mqtt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  esy_mqtt_t client;
  uint8_t packet_buffer[1024];
  int socket_fd;
  SSL_CTX *tls_config;
  SSL *tls;
  unsigned messages;
  unsigned published;
  unsigned subscribed;
  unsigned unsubscribed;
  bool event_failed;
} broker_smoke_ctx_t;

static broker_smoke_ctx_t s_ctx;
static const char k_topic[] = "esy/platform/smoke";
static const char k_payload[] = "binary\0payload";

static uint32_t clock_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    abort();
  }
  return (uint32_t)((uint64_t)now.tv_sec * 1000u +
                      (uint64_t)now.tv_nsec / 1000000u);
}

static int32_t send_bytes(void *context, const void *data, size_t size) {
  broker_smoke_ctx_t *ctx = context;
  if (ctx->tls != NULL) {
    int result = SSL_write(ctx->tls, data, (int)size);
    if (result > 0) {
      return result;
    }
    int error = SSL_get_error(ctx->tls, result);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
  }
  ssize_t result = send(ctx->socket_fd, data, size, MSG_NOSIGNAL);
  return result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
             ? 0
             : (int32_t)result;
}

static int32_t receive_bytes(void *context, void *data, size_t size) {
  broker_smoke_ctx_t *ctx = context;
  if (ctx->tls != NULL) {
    int result = SSL_read(ctx->tls, data, (int)size);
    if (result > 0) {
      return result;
    }
    int error = SSL_get_error(ctx->tls, result);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
  }
  ssize_t result = recv(ctx->socket_fd, data, size, 0);
  if (result == 0) {
    return -1;
  }
  return result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
             ? 0
             : (int32_t)result;
}

static void on_event(void *context, const esy_mqtt_event_t *event) {
  broker_smoke_ctx_t *ctx = context;
  if (event->type == ESY_MQTT_EVENT_DISCONNECTED) {
    return;
  }
  if (event->result != ESY_OK) {
    ctx->event_failed = true;
    return;
  }
  switch (event->type) {
  case ESY_MQTT_EVENT_MESSAGE:
    if (event->message.topic.length != sizeof(k_topic) - 1u ||
        memcmp(event->message.topic.data, k_topic, sizeof(k_topic) - 1u) != 0 ||
        event->message.payload_size != sizeof(k_payload) - 1u ||
        memcmp(event->message.payload, k_payload, sizeof(k_payload) - 1u) != 0) {
      ctx->event_failed = true;
    }
    ++ctx->messages;
    break;
  case ESY_MQTT_EVENT_PUBLISHED:
    ++ctx->published;
    break;
  case ESY_MQTT_EVENT_SUBSCRIBED:
    ++ctx->subscribed;
    break;
  case ESY_MQTT_EVENT_UNSUBSCRIBED:
    ++ctx->unsubscribed;
    break;
  default:
    break;
  }
}

static bool wait_for(unsigned *counter, unsigned expected) {
  uint32_t started = clock_ms();
  while (*counter < expected && (uint32_t)(clock_ms() - started) < 3000u) {
    if (esy_mqtt_process(&s_ctx.client) != ESY_OK || s_ctx.event_failed) {
      return false;
    }
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
  }
  return *counter >= expected;
}

static bool open_stream(uint16_t port, const char *certificate,
                         const char *hostname) {
  s_ctx.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s_ctx.socket_fd < 0) {
    return false;
  }
  struct timeval timeout = {3, 0};
  setsockopt(s_ctx.socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(s_ctx.socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s_ctx.socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    return false;
  }
  if (certificate != NULL) {
    s_ctx.tls_config = SSL_CTX_new(TLS_client_method());
    if (s_ctx.tls_config == NULL ||
        SSL_CTX_load_verify_locations(s_ctx.tls_config, certificate, NULL) != 1) {
      return false;
    }
    SSL_CTX_set_verify(s_ctx.tls_config, SSL_VERIFY_PEER, NULL);
    s_ctx.tls = SSL_new(s_ctx.tls_config);
    if (s_ctx.tls == NULL || SSL_set_fd(s_ctx.tls, s_ctx.socket_fd) != 1 ||
        SSL_set_tlsext_host_name(s_ctx.tls, hostname) != 1 ||
        SSL_set1_host(s_ctx.tls, hostname) != 1 || SSL_connect(s_ctx.tls) != 1 ||
        SSL_get_verify_result(s_ctx.tls) != X509_V_OK) {
      return false;
    }
  }
  int flags = fcntl(s_ctx.socket_fd, F_GETFL, 0);
  return flags >= 0 && fcntl(s_ctx.socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_stream(void) {
  SSL_free(s_ctx.tls);
  SSL_CTX_free(s_ctx.tls_config);
  s_ctx.tls = NULL;
  s_ctx.tls_config = NULL;
  if (s_ctx.socket_fd >= 0) {
    close(s_ctx.socket_fd);
  }
  s_ctx.socket_fd = -1;
}

static bool mqtt_exchange(void) {
  esy_mqtt_connect_config_t connect_config = {0};
  connect_config.client_id = (esy_mqtt_string_t){"esy-smoke", 9u};
  connect_config.keep_alive_seconds = 2u;
  connect_config.timeout_ms = 2000u;
  if (esy_mqtt_connect(&s_ctx.client, &connect_config) != ESY_OK) {
    return false;
  }
  uint16_t packet_id;
  unsigned subscriptions = s_ctx.subscribed + 1u;
  esy_mqtt_string_t topic = {k_topic, sizeof(k_topic) - 1u};
  if (esy_mqtt_subscribe(&s_ctx.client, topic, ESY_MQTT_QOS_1, &packet_id) !=
          ESY_OK ||
      !wait_for(&s_ctx.subscribed, subscriptions)) {
    return false;
  }
  esy_mqtt_message_t message = {0};
  message.topic = topic;
  message.payload = k_payload;
  message.payload_size = sizeof(k_payload) - 1u;
  unsigned messages = s_ctx.messages;
  unsigned publishes = s_ctx.published;
  if (esy_mqtt_publish(&s_ctx.client, &message, &packet_id) != ESY_OK ||
      packet_id != 0u || !wait_for(&s_ctx.messages, ++messages)) {
    return false;
  }
  message.qos = ESY_MQTT_QOS_1;
  if (esy_mqtt_publish(&s_ctx.client, &message, &packet_id) != ESY_OK ||
      packet_id == 0u || !wait_for(&s_ctx.messages, ++messages) ||
      !wait_for(&s_ctx.published, publishes + 1u)) {
    return false;
  }
  unsigned unsubscribes = s_ctx.unsubscribed + 1u;
  return esy_mqtt_unsubscribe(&s_ctx.client, topic, &packet_id) == ESY_OK &&
         wait_for(&s_ctx.unsubscribed, unsubscribes);
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 4) {
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  s_ctx.socket_fd = -1;
  unsigned long number = strtoul(argv[1], NULL, 10);
  if (number == 0u || number > UINT16_MAX) {
    return 2;
  }
  const char *certificate = argc >= 3 ? argv[2] : NULL;
  const char *hostname = argc >= 4 ? argv[3] : "localhost";
  if (!open_stream((uint16_t)number, certificate, hostname)) {
    close_stream();
    return 10;
  }
  esy_mqtt_static_resources_t resources = {s_ctx.packet_buffer,
                                            sizeof(s_ctx.packet_buffer)};
  esy_mqtt_config_t config = {0};
  config.transport = (esy_network_stream_t){.send = send_bytes, .recv = receive_bytes, .context = &s_ctx};
  config.static_resources = &resources;
  config.get_time_ms = clock_ms;
  config.acknowledgement_timeout_ms = 1000u;
  config.event_cb = on_event;
  config.event_context = &s_ctx;
  int result = 0;
  if (esy_mqtt_init(&s_ctx.client, &config) != ESY_OK || !mqtt_exchange()) {
    result = 11;
  } else {
    shutdown(s_ctx.socket_fd, SHUT_RDWR);
    if (esy_mqtt_process(&s_ctx.client) != ESY_ERR_IO ||
        esy_mqtt_is_connected(&s_ctx.client)) {
      result = 12;
    }
    close_stream();
    if (result == 0 &&
        (!open_stream((uint16_t)number, certificate, hostname) ||
         !mqtt_exchange())) {
      result = 13;
    }
  }
  if (esy_mqtt_is_connected(&s_ctx.client)) {
    if (esy_mqtt_disconnect(&s_ctx.client) != ESY_OK) {
      result = 14;
    }
  }
  if (esy_mqtt_deinit(&s_ctx.client) != ESY_OK || s_ctx.event_failed) {
    result = 15;
  }
  close_stream();
  if (result != 0) {
    fprintf(stderr, "Broker smoke failed at stage %d\n", result);
  }
  return result;
}
