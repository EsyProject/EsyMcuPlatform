/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#include "esy_middleware/tls/esy_tls_mbedtls.h"
#include "esy_middleware/crypto/esy_crypto_mbedtls.h"

#if defined(MBEDTLS_USE_PSA_CRYPTO)
#include "psa/crypto.h"
#endif

#define CERTIFICATE_CAPACITY 8192u
#define TRANSMIT_CAPACITY 256u
#define RECEIVE_CAPACITY 4096u
#define SHORT_NATIVE_SEND_BYTES 17u

typedef struct {
  esy_tls_t tls;
  esy_crypto_random_t generator;
  esy_credentials_t credentials;
  esy_credentials_config_t credential_config;
  esy_tls_config_t config;
  esy_tls_static_resources_t resources;
  int descriptor;
  bool send_blocked;
  bool fragment_sends;
  bool random_failure;
  bool utc_failure;
  bool fake_clock;
  bool fixed_utc;
  uint64_t microseconds;
  int64_t utc_seconds;
  int64_t utc_offset;
  unsigned send_calls;
  unsigned random_calls;
  uint8_t tx[TRANSMIT_CAPACITY];
  uint8_t trust[CERTIFICATE_CAPACITY];
  uint8_t certificate[CERTIFICATE_CAPACITY];
  uint8_t key[CERTIFICATE_CAPACITY];
} tls_smoke_ctx_t;

static tls_smoke_ctx_t s_ctx;

static uint64_t actual_microseconds(void) {
  struct timespec timestamp;
  assert(clock_gettime(CLOCK_MONOTONIC, &timestamp) == 0);
  return (uint64_t)timestamp.tv_sec * UINT64_C(1000000) +
         (uint64_t)timestamp.tv_nsec / UINT64_C(1000);
}

static void pause_iteration(void) {
  struct timespec delay = {0, 100000};
  (void)nanosleep(&delay, NULL);
}

static esy_err_t monotonic_us(void *context, uint64_t *microseconds) {
  tls_smoke_ctx_t *ctx = context;
  *microseconds = ctx->fake_clock ? ctx->microseconds : actual_microseconds();
  return ESY_OK;
}

static esy_err_t utc_now(void *context, esy_timestamp_t *timestamp) {
  tls_smoke_ctx_t *ctx = context;
  if (ctx->utc_failure) { return ESY_ERR_IO; }
  timestamp->seconds = (ctx->fixed_utc ? ctx->utc_seconds : (int64_t)time(NULL)) +
                       ctx->utc_offset;
  timestamp->nanoseconds = 0u;
  return ESY_OK;
}

static esy_err_t entropy_fill(void *context, uint8_t *output, size_t length) {
  tls_smoke_ctx_t *ctx = context;
  ++ctx->random_calls;
  size_t offset = 0u;
  while (offset < length) {
    ssize_t received = getrandom(output + offset, length - offset, 0);
    if (received < 0 && errno == EINTR) { continue; }
    if (received <= 0) { return ESY_ERR_IO; }
    offset += (size_t)received;
  }
  return ESY_OK;
}

static esy_err_t failed_random(void *context, uint8_t *output, size_t length) {
  (void)context;
  (void)output;
  (void)length;
  return ESY_ERR_IO;
}

static int32_t transport_send(void *context, const void *data, size_t size) {
  tls_smoke_ctx_t *ctx = context;
  if (ctx->descriptor < 0 || ctx->send_blocked) { return 0; }
  if (ctx->fragment_sends) {
    ++ctx->send_calls;
    if ((ctx->send_calls & 1u) == 0u) { return 0; }
    if (size > SHORT_NATIVE_SEND_BYTES) { size = SHORT_NATIVE_SEND_BYTES; }
  }
  ssize_t written = send(ctx->descriptor, data, size, MSG_NOSIGNAL);
  if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
    return 0;
  }
  return written < 0 ? ESY_ERR_IO : (int32_t)written;
}

static int32_t transport_recv(void *context, void *data, size_t size) {
  tls_smoke_ctx_t *ctx = context;
  if (ctx->descriptor < 0) { return 0; }
  ssize_t received = recv(ctx->descriptor, data, size, 0);
  if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
    return 0;
  }
  return received <= 0 ? ESY_ERR_IO : (int32_t)received;
}

static size_t read_pem(const char *path, uint8_t *buffer, size_t capacity) {
  FILE *file = fopen(path, "rb");
  assert(file != NULL);
  size_t length = fread(buffer, 1u, capacity - 1u, file);
  assert(length > 0u && !ferror(file) && feof(file));
  assert(fclose(file) == 0);
  buffer[length] = 0u;
  return length + 1u;
}

static void load_credentials(char **arguments) {
  const esy_crypto_random_config_t random_config = {
      .entropy = {entropy_fill, &s_ctx}};
  assert(esy_crypto_random_init(&s_ctx.generator, &random_config) == ESY_OK);
  s_ctx.credential_config.trust_anchors = (esy_credentials_blob_t){
      s_ctx.trust, read_pem(arguments[3], s_ctx.trust, sizeof(s_ctx.trust)),
      ESY_CREDENTIALS_FORMAT_PEM};
  if (strcmp(arguments[5], "-") != 0) {
    s_ctx.credential_config.client_certificate = (esy_credentials_blob_t){
        s_ctx.certificate,
        read_pem(arguments[5], s_ctx.certificate, sizeof(s_ctx.certificate)),
        ESY_CREDENTIALS_FORMAT_PEM};
    s_ctx.credential_config.private_key = (esy_credentials_blob_t){
        s_ctx.key, read_pem(arguments[6], s_ctx.key, sizeof(s_ctx.key)),
        ESY_CREDENTIALS_FORMAT_PEM};
  }
  assert(esy_credentials_init(&s_ctx.credentials, &s_ctx.credential_config) == ESY_OK);
  s_ctx.resources = (esy_tls_static_resources_t){s_ctx.tx, sizeof(s_ctx.tx)};
  s_ctx.config = (esy_tls_config_t){
      .transport = {transport_send, transport_recv, NULL, &s_ctx},
      .credentials = &s_ctx.credentials,
      .random = {esy_crypto_random_fill, &s_ctx.generator},
      .hostname = arguments[4],
      .monotonic_us = monotonic_us,
      .utc_now = utc_now,
      .time_context = &s_ctx,
      .handshake_timeout_us = UINT32_C(5000000),
      .static_resources = &s_ctx.resources};
}

static esy_tls_status_t status(void) {
  esy_tls_status_t snapshot;
  assert(esy_tls_get_status(&s_ctx.tls, &snapshot) == ESY_OK);
  return snapshot;
}

static void connect_socket(const char *port_string) {
  unsigned long port = strtoul(port_string, NULL, 10);
  assert(port > 0u && port <= UINT16_MAX);
  s_ctx.descriptor = socket(AF_INET, SOCK_STREAM, 0);
  assert(s_ctx.descriptor >= 0);
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(connect(s_ctx.descriptor, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(fcntl(s_ctx.descriptor, F_SETFL, O_NONBLOCK) == 0);
}

static void expect_sticky_failure(esy_err_t expected) {
  esy_tls_status_t failed = status();
  assert(failed.state == ESY_TLS_FAILED);
  assert(failed.error != ESY_OK);
  if (expected != ESY_OK) { assert(failed.error == expected); }
  s_ctx.microseconds = UINT64_C(100);
  assert(esy_tls_process(&s_ctx.tls) == failed.error);
  esy_tls_status_t repeated = status();
  assert(repeated.state == ESY_TLS_FAILED && repeated.error == failed.error);
  assert(repeated.native_error == failed.native_error);
  esy_network_stream_t stream = {0};
  assert(esy_tls_get_stream(&s_ctx.tls, &stream) == ESY_ERR_STATE);
}

static void lifecycle_checks(void) {
  assert(esy_tls_init(NULL, &s_ctx.config) == ESY_ERR_INVALID_ARG);
  assert(esy_tls_init(&s_ctx.tls, NULL) == ESY_ERR_INVALID_ARG);
  assert(esy_tls_process(&s_ctx.tls) == ESY_ERR_STATE);
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
  esy_tls_config_t invalid = s_ctx.config;
  invalid.hostname = "";
  assert(esy_tls_init(&s_ctx.tls, &invalid) == ESY_ERR_INVALID_ARG);
  invalid = s_ctx.config;
  invalid.random.fill = NULL;
  assert(esy_tls_init(&s_ctx.tls, &invalid) == ESY_ERR_INVALID_ARG);
  invalid = s_ctx.config;
  invalid.utc_now = NULL;
  assert(esy_tls_init(&s_ctx.tls, &invalid) == ESY_ERR_INVALID_ARG);
  invalid = s_ctx.config;
  invalid.monotonic_us = NULL;
  assert(esy_tls_init(&s_ctx.tls, &invalid) == ESY_ERR_INVALID_ARG);
  invalid = s_ctx.config;
  invalid.handshake_timeout_us = 0u;
  assert(esy_tls_init(&s_ctx.tls, &invalid) == ESY_ERR_INVALID_ARG);
  invalid = s_ctx.config;
  invalid.static_resources = NULL;
  assert(esy_tls_init(&s_ctx.tls, &invalid) == ESY_ERR_INVALID_ARG);
  static const uint8_t k_malformed[] = "not a certificate";
  esy_credentials_config_t malformed = s_ctx.credential_config;
  malformed.trust_anchors = (esy_credentials_blob_t){
      k_malformed, sizeof(k_malformed), ESY_CREDENTIALS_FORMAT_PEM};
  assert(esy_credentials_replace(&s_ctx.credentials, &malformed) == ESY_OK);
  assert(esy_tls_init(&s_ctx.tls, &s_ctx.config) != ESY_OK);
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
  assert(esy_credentials_replace(&s_ctx.credentials, &s_ctx.credential_config) == ESY_OK);
  s_ctx.fake_clock = true;
  s_ctx.microseconds = UINT64_MAX - UINT64_C(50);
  s_ctx.config.handshake_timeout_us = 100u;
  assert(esy_tls_init(&s_ctx.tls, &s_ctx.config) == ESY_OK);
  assert(esy_tls_init(&s_ctx.tls, &s_ctx.config) == ESY_ERR_STATE);
  assert(esy_credentials_deinit(&s_ctx.credentials) == ESY_ERR_BUSY);
  assert(esy_credentials_replace(&s_ctx.credentials, &s_ctx.credential_config) == ESY_ERR_BUSY);
  esy_network_stream_t stream = {0};
  assert(esy_tls_get_stream(&s_ctx.tls, &stream) == ESY_ERR_STATE);
  s_ctx.microseconds += 99u;
  assert(esy_tls_process(&s_ctx.tls) == ESY_OK);
  assert(status().state == ESY_TLS_HANDSHAKING);
  ++s_ctx.microseconds;
  assert(esy_tls_process(&s_ctx.tls) == ESY_ERR_TIMEOUT);
  expect_sticky_failure(ESY_ERR_TIMEOUT);
  /* Returning to the start value models another complete counter cycle. */
  s_ctx.microseconds = UINT64_MAX - UINT64_C(50);
  assert(esy_tls_process(&s_ctx.tls) == ESY_ERR_TIMEOUT);
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
  assert(esy_credentials_replace(&s_ctx.credentials, &s_ctx.credential_config) == ESY_OK);
  assert(esy_tls_init(&s_ctx.tls, &s_ctx.config) == ESY_OK);
  s_ctx.microseconds += 99u;
  assert(esy_tls_process(&s_ctx.tls) == ESY_OK);
  assert(status().state == ESY_TLS_HANDSHAKING);
  /* Aborting a live wait permanently cancels it until an explicit init. */
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
  s_ctx.microseconds = UINT64_MAX - UINT64_C(50);
  assert(esy_tls_process(&s_ctx.tls) == ESY_ERR_STATE);
  s_ctx.config.handshake_timeout_us = UINT32_MAX;
  assert(esy_tls_init(&s_ctx.tls, &s_ctx.config) == ESY_OK);
  s_ctx.microseconds += (uint64_t)UINT32_MAX - 1u;
  assert(esy_tls_process(&s_ctx.tls) == ESY_OK);
  ++s_ctx.microseconds;
  assert(esy_tls_process(&s_ctx.tls) == ESY_ERR_TIMEOUT);
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
}

static void authenticated_exchange(void) {
  esy_network_stream_t stream = {0};
  assert(esy_tls_get_stream(&s_ctx.tls, &stream) == ESY_OK);
  uint8_t request[] = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
  s_ctx.send_blocked = true;
  s_ctx.fragment_sends = true;
  assert(stream.send(stream.context, request, sizeof(request) - 1u) ==
         (int32_t)(sizeof(request) - 1u));
  /* A stream borrower can immediately reuse every accepted input byte. */
  memset(request, 'X', sizeof(request));
  assert(stream.send(stream.context, request, 1u) == 0);
  for (unsigned attempt = 0u; attempt < 4u; ++attempt) {
    assert(stream.progress(stream.context) == ESY_OK);
    assert(status().pending_tx_bytes == sizeof(request) - 1u);
  }
  s_ctx.send_blocked = false;
  uint64_t deadline = actual_microseconds() + UINT64_C(5000000);
  while (status().pending_tx_bytes != 0u && actual_microseconds() < deadline) {
    assert(stream.progress(stream.context) == ESY_OK);
  }
  assert(status().pending_tx_bytes == 0u);
  char response[RECEIVE_CAPACITY] = {0};
  size_t received = 0u;
  while (received < sizeof(response) - 1u && actual_microseconds() < deadline) {
    int32_t count = stream.recv(stream.context, response + received,
                                sizeof(response) - received - 1u);
    assert(count >= 0);
    received += (size_t)count;
    if (strstr(response, "HTTP/1.0 200 ok") != NULL) { break; }
    pause_iteration();
  }
  assert(strstr(response, "HTTP/1.0 200 ok") != NULL);
  assert(s_ctx.random_calls != 0u);
  /* A completed handshake cannot be rearmed by a full clock cycle. */
  s_ctx.fake_clock = true;
  s_ctx.microseconds = 0u;
  assert(esy_tls_process(&s_ctx.tls) == ESY_OK);
  assert(status().state == ESY_TLS_READY);
  assert(esy_tls_close(&s_ctx.tls) == ESY_OK);
  assert(esy_tls_close(&s_ctx.tls) == ESY_OK);
  deadline = actual_microseconds() + UINT64_C(5000000);
  while (status().state == ESY_TLS_CLOSING && actual_microseconds() < deadline) {
    assert(esy_tls_process(&s_ctx.tls) == ESY_OK);
  }
  assert(status().state == ESY_TLS_CLOSED);
  s_ctx.microseconds = UINT64_MAX;
  assert(esy_tls_process(&s_ctx.tls) == ESY_OK);
  assert(status().state == ESY_TLS_CLOSED);
}

static void rejected_control_write(void) {
  esy_network_stream_t stream = {0};
  assert(esy_tls_get_stream(&s_ctx.tls, &stream) == ESY_OK);
  s_ctx.send_blocked = true;
  puts("READY");
  assert(fflush(stdout) == 0);
  uint64_t deadline = actual_microseconds() + UINT64_C(5000000);
  uint8_t byte = 0u;
  while (status().state == ESY_TLS_READY && actual_microseconds() < deadline) {
    (void)stream.recv(stream.context, &byte, sizeof(byte));
    pause_iteration();
  }
  assert(status().state == ESY_TLS_FAILED);
  assert(status().native_error == MBEDTLS_ERR_SSL_WANT_WRITE);
  assert(stream.send(stream.context, &byte, sizeof(byte)) < 0);
  assert(esy_tls_close(&s_ctx.tls) == ESY_ERR_STATE);
  assert(status().pending_tx_bytes == 0u);
}

static void recover_date_rejected_init(const char *mode) {
  /* Failed init must release its lease and restore a reusable zero-state handle. */
  assert(esy_credentials_replace(&s_ctx.credentials, &s_ctx.credential_config) == ESY_OK);
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
  s_ctx.utc_offset = 0;
  if (s_ctx.fixed_utc) {
    s_ctx.utc_seconds = strcmp(mode, "fixed-future-reject") == 0
                           ? INT64_C(2366841600) /* 2045-01-01T00:00:00Z. */
                           : INT64_C(1735689600); /* 2025-01-01T00:00:00Z. */
  }
  assert(esy_tls_init(&s_ctx.tls, &s_ctx.config) == ESY_OK);
  assert(status().state == ESY_TLS_HANDSHAKING);
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
}

int main(int argc, char **argv) {
  assert(argc == 7);
#if defined(MBEDTLS_USE_PSA_CRYPTO)
  /* The native provider's process-global PSA lifecycle belongs to its consumer. */
  assert(psa_crypto_init() == PSA_SUCCESS);
#endif
  s_ctx.descriptor = -1;
  load_credentials(argv);
  const char *mode = argv[1];
  if (strcmp(mode, "lifecycle") == 0) {
    lifecycle_checks();
  } else {
    bool renegotiate = strcmp(mode, "renegotiate") == 0;
    bool success = strcmp(mode, "success") == 0 ||
                   strcmp(mode, "fixed-success") == 0 || renegotiate;
    bool date_rejection = strcmp(mode, "past") == 0 || strcmp(mode, "future") == 0 ||
                          strcmp(mode, "fixed-reject") == 0 ||
                          strcmp(mode, "fixed-future-reject") == 0;
    if (strncmp(mode, "fixed-", 6u) == 0) {
      s_ctx.fixed_utc = true;
      s_ctx.utc_seconds = INT64_C(2051222400); /* 2035-01-01T00:00:00Z. */
    }
    s_ctx.random_failure = strcmp(mode, "random-failure") == 0;
    if (s_ctx.random_failure) {
      s_ctx.config.random = (esy_crypto_random_provider_t){failed_random, &s_ctx};
    }
    s_ctx.utc_failure = strcmp(mode, "utc-failure") == 0;
    if (strcmp(mode, "past") == 0) { s_ctx.utc_offset = -INT64_C(315360000); }
    if (strcmp(mode, "future") == 0) { s_ctx.utc_offset = INT64_C(315360000); }
    if (!s_ctx.random_failure) { connect_socket(argv[2]); }
    esy_err_t initialized = esy_tls_init(&s_ctx.tls, &s_ctx.config);
    if (date_rejection) { assert(initialized == ESY_ERR_HASH_FAIL); }
    if (initialized != ESY_OK) {
      assert(!success);
      if (date_rejection) {
        recover_date_rejected_init(mode);
      } else if (s_ctx.random_failure || s_ctx.utc_failure) {
        assert(initialized == ESY_ERR_IO);
      } else {
        assert(initialized == ESY_OK);
      }
    } else {
      uint64_t deadline = actual_microseconds() + UINT64_C(10000000);
      while (status().state == ESY_TLS_HANDSHAKING && actual_microseconds() < deadline) {
        (void)esy_tls_process(&s_ctx.tls);
        pause_iteration();
      }
      if (success) {
        assert(status().state == ESY_TLS_READY);
        if (renegotiate) { rejected_control_write(); }
        else { authenticated_exchange(); }
      } else {
        expect_sticky_failure(s_ctx.random_failure ? ESY_ERR_IO : ESY_OK);
      }
    }
  }
  assert(esy_tls_deinit(&s_ctx.tls) == ESY_OK);
  assert(esy_credentials_deinit(&s_ctx.credentials) == ESY_OK);
  assert(esy_crypto_random_deinit(&s_ctx.generator) == ESY_OK);
  if (s_ctx.descriptor >= 0) { assert(close(s_ctx.descriptor) == 0); }
  return 0;
}
