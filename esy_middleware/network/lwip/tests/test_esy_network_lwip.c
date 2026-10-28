/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <string.h>

#include "esy_middleware/network/esy_network_lwip.h"
#include "fake_lwip.h"
#include "network_lwip_dns_fixture.h"

#define TEST_RX_BYTES 8u
#define TEST_PORT 1883u

typedef struct {
  esy_network_lwip_t handle;
  esy_network_stream_t transport;
  esy_network_connection_t connection;
  uint8_t rx_buffer[TEST_RX_BYTES];
  esy_network_lwip_static_resources_t resources;
  esy_network_lwip_config_t config;
  unsigned progress_calls;
  unsigned owner_enters;
  unsigned owner_leaves;
  bool receive_on_progress;
  bool connect_on_progress;
  bool error_on_progress;
  bool reenter_on_progress;
} network_lwip_test_ctx_t;

static network_lwip_test_ctx_t s_ctx;

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_lwip_reset();
  network_lwip_dns_reset();
  s_ctx.resources.rx_buffer = s_ctx.rx_buffer;
  s_ctx.resources.rx_buffer_size = TEST_RX_BYTES;
  s_ctx.config.static_resources = &s_ctx.resources;
}

void tearDown(void) {
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_OK, ERR_OK);
  if (network_lwip_dns_pending()) network_lwip_dns_complete(false);
  if (esy_network_lwip_get_state(&s_ctx.handle) != ESY_NETWORK_UNINITIALIZED) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_deinit(&s_ctx.handle));
  }
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pcbs());
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pbufs());
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_invalid_pbuf_frees());
}

static void initialize(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_init(&s_ctx.handle, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_get_stream(&s_ctx.handle, &s_ctx.transport));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_get_connection(&s_ctx.handle, &s_ctx.connection));
}

static void connect_numeric(void) {
  initialize();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTING,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_connect());
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTED,
      esy_network_lwip_get_state(&s_ctx.handle));
}

static void progress(void *context) {
  network_lwip_test_ctx_t *test = context;
  ++test->progress_calls;
  if (test->reenter_on_progress) {
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_close(&test->handle));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_deinit(&test->handle));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
        test->transport.send(test->transport.context, "x", 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
        test->transport.recv(test->transport.context, test->rx_buffer, 1u));
    esy_network_connection_status_t status = {.state = ESY_NETWORK_CONNECTION_EOF,
        .session_id = 7u};
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
        test->connection.get_status(test->connection.stream.context, &status));
    TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_EOF, status.state);
    TEST_ASSERT_EQUAL_UINT64(7u, status.session_id);
  }
  if (test->connect_on_progress) {
    test->connect_on_progress = false;
    TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_connect());
  }
  if (test->receive_on_progress) {
    test->receive_on_progress = false;
    TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(fake_lwip_last_pcb(),
        (const uint8_t *)"abc", 3u));
  }
  if (test->error_on_progress) {
    test->error_on_progress = false;
    fake_lwip_error(fake_lwip_last_pcb());
  }
}

void test_invalid_lifecycle_inputs_preserve_handle_and_outputs(void) {
  esy_network_stream_t output = {.context = &s_ctx};
  uint8_t initial[sizeof(s_ctx.handle)];
  memcpy(initial, &s_ctx.handle, sizeof(initial));
  TEST_ASSERT_EQUAL(ESY_NETWORK_UNINITIALIZED, esy_network_lwip_get_state(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_init(&s_ctx.handle, NULL));
  s_ctx.config.static_resources = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_init(&s_ctx.handle,
      &s_ctx.config));
  s_ctx.config.static_resources = &s_ctx.resources;
  s_ctx.resources.rx_buffer = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_init(&s_ctx.handle,
      &s_ctx.config));
  s_ctx.resources.rx_buffer = s_ctx.rx_buffer;
  const uint32_t invalid_sizes[] = {0u, 3u, UINT32_MAX};
  for (size_t i = 0u; i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++i) {
    s_ctx.resources.rx_buffer_size = invalid_sizes[i];
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_init(&s_ctx.handle,
        &s_ctx.config));
  }
  s_ctx.resources.rx_buffer_size = TEST_RX_BYTES;
  TEST_ASSERT_EQUAL_MEMORY(initial, &s_ctx.handle, sizeof(initial));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_get_stream(NULL, &output));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_network_lwip_get_stream(&s_ctx.handle, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_network_lwip_get_stream(&s_ctx.handle, &output));
  TEST_ASSERT_EQUAL_PTR(&s_ctx, output.context);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_close(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_network_lwip_deinit(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  initialize();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_network_lwip_init(&s_ctx.handle, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_network_lwip_begin(NULL, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_network_lwip_begin(&s_ctx.handle, NULL, TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_network_lwip_begin(&s_ctx.handle, "", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", 0u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_deinit(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_network_lwip_deinit(&s_ctx.handle));
  initialize();
}

void test_numeric_connect_accepts_partial_send_and_temporary_backpressure(void) {
  uint8_t data[TCP_SND_BUF + 3u] = {0};
  connect_numeric();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      s_ctx.transport.send(s_ctx.transport.context, NULL, 1u));
  TEST_ASSERT_EQUAL(0, s_ctx.transport.send(s_ctx.transport.context, NULL, 0u));
  TEST_ASSERT_EQUAL(TCP_SND_BUF,
      s_ctx.transport.send(s_ctx.transport.context, data, sizeof(data)));
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_writes());
  fake_lwip_set_write_result(ERR_MEM);
  TEST_ASSERT_EQUAL(0, s_ctx.transport.send(s_ctx.transport.context, data, 1u));
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_writes());
  fake_lwip_set_write_result(ERR_OK);
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_OK, ERR_MEM);
  TEST_ASSERT_EQUAL(3, s_ctx.transport.send(s_ctx.transport.context, data, 3u));
  TEST_ASSERT_EQUAL_UINT(2u, fake_lwip_writes());
  fake_lwip_set_write_result(ERR_ABRT);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      s_ctx.transport.send(s_ctx.transport.context, data, 1u));
  TEST_ASSERT_EQUAL(ESY_NETWORK_ERROR,
      esy_network_lwip_get_state(&s_ctx.handle));
}

void test_rx_larger_than_ring_preserves_order_and_refused_pbuf_ownership(void) {
  uint8_t received[20] = {0};
  connect_numeric();
  struct tcp_pcb *pcb = fake_lwip_last_pcb();
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(pcb,
      (const uint8_t *)"abcdefghijkl", 12u));
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_live_pbufs());
  TEST_ASSERT_EQUAL(ERR_MEM, fake_lwip_receive(pcb,
      (const uint8_t *)"mnop", 4u));
  TEST_ASSERT_EQUAL_UINT(2u, fake_lwip_live_pbufs());
  TEST_ASSERT_EQUAL(3,
      s_ctx.transport.recv(s_ctx.transport.context, received, 3u));
  TEST_ASSERT_EQUAL(ERR_MEM, fake_lwip_redeliver(pcb));
  TEST_ASSERT_EQUAL(9,
      s_ctx.transport.recv(s_ctx.transport.context, received + 3u, 9u));
  TEST_ASSERT_EQUAL_UINT(1u, fake_lwip_live_pbufs());
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_redeliver(pcb));
  TEST_ASSERT_EQUAL(4,
      s_ctx.transport.recv(s_ctx.transport.context, received + 12u, 8u));
  TEST_ASSERT_EQUAL_MEMORY("abcdefghijklmnop", received, 16u);
  TEST_ASSERT_EQUAL(0,
      s_ctx.transport.recv(s_ctx.transport.context, received, sizeof(received)));
  TEST_ASSERT_EQUAL(0, s_ctx.transport.recv(s_ctx.transport.context, NULL, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      s_ctx.transport.recv(s_ctx.transport.context, NULL, 1u));
}

void test_peer_fin_allows_buffered_rx_before_eof_and_requires_close_for_reuse(void) {
  uint8_t received[16];
  connect_numeric();
  struct tcp_pcb *pcb = fake_lwip_last_pcb();
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(pcb,
      (const uint8_t *)"abcdefghijkl", 12u));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_fin(pcb));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CLOSED,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(12,
      s_ctx.transport.recv(s_ctx.transport.context, received, sizeof(received)));
  TEST_ASSERT_EQUAL_MEMORY("abcdefghijkl", received, 12u);
  TEST_ASSERT_LESS_THAN(0,
      s_ctx.transport.recv(s_ctx.transport.context, received, sizeof(received)));
  esy_network_connection_status_t status;
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.connection.get_status(s_ctx.connection.stream.context, &status));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_EOF, status.state);
  TEST_ASSERT_TRUE(status.send_capacity > 0u);
  TEST_ASSERT_EQUAL(1, s_ctx.connection.send_all(s_ctx.connection.stream.context, received, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_NETWORK_IDLE,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
}

void test_async_dns_cancel_retains_handle_until_callback_then_can_reinitialize(void) {
  initialize();
  /* The shared fake ipaddr_aton rejects a leading '!'; use it to enter DNS. */
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "!broker.invalid", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_NETWORK_RESOLVING,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_deinit(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CLOSING,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_network_lwip_init(&s_ctx.handle, &s_ctx.config));
  network_lwip_dns_complete(true);
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pcbs());
  TEST_ASSERT_EQUAL(ESY_NETWORK_IDLE,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_deinit(&s_ctx.handle));
  initialize();
}

void test_async_and_cached_dns_successes_start_raw_tcp_connection(void) {
  initialize();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "!broker.invalid", TEST_PORT));
  network_lwip_dns_complete(true);
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTING,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_connect());
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  network_lwip_dns_result(ERR_OK);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "!broker.invalid", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTING,
      esy_network_lwip_get_state(&s_ctx.handle));
}

void test_dns_failures_require_explicit_close_before_new_attempt(void) {
  initialize();
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "!broker.invalid", TEST_PORT));
  network_lwip_dns_complete(false);
  TEST_ASSERT_EQUAL(ESY_NETWORK_ERROR,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_network_lwip_begin(&s_ctx.handle, "!broker.invalid", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  network_lwip_dns_result(ERR_MEM);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,
      esy_network_lwip_begin(&s_ctx.handle, "!broker.invalid", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  network_lwip_dns_result(ERR_ABRT);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_network_lwip_begin(&s_ctx.handle, "!broker.invalid", TEST_PORT));
}

void test_native_connect_failures_and_error_callbacks_release_owned_resources(void) {
  initialize();
  fake_lwip_fail(ERR_MEM, ERR_OK, ERR_OK, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  fake_lwip_fail(ERR_OK, ERR_MEM, ERR_OK, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pcbs());
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  fake_lwip_fail(ERR_OK, ERR_ABRT, ERR_OK, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_OK, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  struct tcp_pcb *pcb = fake_lwip_last_pcb();
  TEST_ASSERT_EQUAL(ERR_OK, pcb->connected(pcb->arg, pcb, ERR_ABRT));
  TEST_ASSERT_EQUAL(ESY_NETWORK_ERROR,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_connect());
  pcb = fake_lwip_last_pcb();
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(pcb,
      (const uint8_t *)"abcdefghijkl", 12u));
  TEST_ASSERT_EQUAL(ERR_MEM, fake_lwip_receive(pcb,
      (const uint8_t *)"mnop", 4u));
  fake_lwip_error(pcb);
  TEST_ASSERT_EQUAL(ESY_NETWORK_ERROR,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pbufs());
}

void test_failed_close_preserves_callback_context_and_allows_retry(void) {
  connect_numeric();
  struct tcp_pcb *pcb = fake_lwip_last_pcb();
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(pcb,
      (const uint8_t *)"abcdefghijkl", 12u));
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_MEM, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_deinit(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CLOSING,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_NOT_NULL(pcb->arg);
  TEST_ASSERT_NOT_NULL(pcb->recv);
  TEST_ASSERT_NOT_NULL(pcb->error);
  TEST_ASSERT_EQUAL(ERR_MEM, fake_lwip_receive(pcb,
      (const uint8_t *)"mnop", 4u));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_fin(pcb));
  TEST_ASSERT_EQUAL(ERR_OK, pcb->connected(pcb->arg, pcb, ERR_OK));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CLOSING,
      esy_network_lwip_get_state(&s_ctx.handle));
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_ABRT, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_network_lwip_close(&s_ctx.handle));
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_OK, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pbufs());
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_aborts());
}

void test_error_during_close_retires_pending_state_without_reconnecting(void) {
  connect_numeric();
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_MEM, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_close(&s_ctx.handle));
  fake_lwip_error(fake_lwip_last_pcb());
  TEST_ASSERT_EQUAL(ESY_NETWORK_IDLE,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pcbs());
}

void test_receive_error_marks_stream_failed_and_discards_buffered_bytes(void) {
  connect_numeric();
  struct tcp_pcb *pcb = fake_lwip_last_pcb();
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_receive(pcb,
      (const uint8_t *)"abcdefghijkl", 12u));
  TEST_ASSERT_EQUAL(ERR_OK, pcb->recv(pcb->arg, pcb, NULL, ERR_ABRT));
  TEST_ASSERT_EQUAL(ESY_NETWORK_ERROR,
      esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_LESS_THAN(0,
      s_ctx.transport.recv(s_ctx.transport.context, s_ctx.rx_buffer, 1u));
}

void test_explicit_provider_progress_advances_io_and_rejects_recursive_operations(void) {
  uint8_t received[4];
  s_ctx.config.progress = progress;
  s_ctx.config.progress_context = &s_ctx;
  initialize();
  TEST_ASSERT_LESS_THAN(0,
      s_ctx.transport.send(s_ctx.transport.context, "x", 1u));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.progress_calls);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  s_ctx.connect_on_progress = true;
  s_ctx.receive_on_progress = true;
  s_ctx.reenter_on_progress = true;
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.transport.progress(s_ctx.transport.context));
  TEST_ASSERT_EQUAL(3,
      s_ctx.transport.recv(s_ctx.transport.context, received, sizeof(received)));
  TEST_ASSERT_EQUAL_MEMORY("abc", received, 3u);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.progress_calls);
  s_ctx.reenter_on_progress = false;
  s_ctx.error_on_progress = true;
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.transport.progress(s_ctx.transport.context));
  TEST_ASSERT_LESS_THAN(0,
      s_ctx.transport.send(s_ctx.transport.context, "x", 1u));
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.progress_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_deinit(&s_ctx.handle));
  TEST_ASSERT_LESS_THAN(0,
      s_ctx.transport.send(s_ctx.transport.context, "x", 1u));
  TEST_ASSERT_LESS_THAN(0,
      s_ctx.transport.recv(s_ctx.transport.context, received, sizeof(received)));
}

static void owner_enter(void *context) {
  ++((network_lwip_test_ctx_t *)context)->owner_enters;
}

static void owner_leave(void *context) {
  ++((network_lwip_test_ctx_t *)context)->owner_leaves;
}

void test_network_whole_writes_and_peer_ack_status_are_distinct(void) {
  connect_numeric();
  esy_network_connection_status_t status;
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.connection.get_status(s_ctx.connection.stream.context, &status));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CONNECTION_OPEN, status.state);
  TEST_ASSERT_EQUAL_UINT64(1u, status.session_id);
  TEST_ASSERT_EQUAL_UINT32(0u, status.unacked_bytes);
  uint8_t oversized[TCP_SND_BUF + 1u] = {0};
  TEST_ASSERT_EQUAL(0, s_ctx.connection.send_all(s_ctx.connection.stream.context, oversized, sizeof(oversized)));
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_writes());
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_OK, ERR_MEM);
  TEST_ASSERT_EQUAL(3, s_ctx.connection.send_all(s_ctx.connection.stream.context, "abc", 3u));
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.connection.get_status(s_ctx.connection.stream.context, &status));
  TEST_ASSERT_EQUAL_UINT32(3u, status.unacked_bytes);
  fake_lwip_ack(fake_lwip_last_pcb());
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.connection.get_status(s_ctx.connection.stream.context, &status));
  TEST_ASSERT_EQUAL_UINT32(0u, status.unacked_bytes);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_connect());
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.connection.get_status(s_ctx.connection.stream.context, &status));
  TEST_ASSERT_EQUAL_UINT64(2u, status.session_id);
}

void test_network_listener_retires_peer_with_retry_and_preserves_listener(void) {
  initialize();
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_listen(&s_ctx.handle, TEST_PORT, 1u));
  TEST_ASSERT_EQUAL(ESY_NETWORK_LISTENING, esy_network_lwip_get_state(&s_ctx.handle));
  struct tcp_pcb *peer = fake_lwip_accept();
  TEST_ASSERT_NOT_NULL(peer);
  TEST_ASSERT_NULL(fake_lwip_accept());
  TEST_ASSERT_EQUAL_UINT(2u, fake_lwip_live_pcbs());
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_fin(peer));
  TEST_ASSERT_EQUAL(ESY_NETWORK_CLOSED, esy_network_lwip_get_state(&s_ctx.handle));
  unsigned aborts = fake_lwip_aborts();
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_MEM, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_close_peer(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_deinit(&s_ctx.handle));
  TEST_ASSERT_EQUAL_UINT(aborts, fake_lwip_aborts());
  TEST_ASSERT_EQUAL_UINT(2u, fake_lwip_live_pcbs());
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_OK, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close_peer(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_NETWORK_LISTENING, esy_network_lwip_get_state(&s_ctx.handle));
  TEST_ASSERT_NOT_NULL(fake_lwip_accept());
  esy_network_connection_status_t status;
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.connection.get_status(s_ctx.connection.stream.context, &status));
  TEST_ASSERT_EQUAL_UINT64(2u, status.session_id);
}

void test_network_listener_setup_failures_release_original_pcb(void) {
  initialize();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_listen(&s_ctx.handle, 0u, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_listen(&s_ctx.handle, TEST_PORT, 0u));
  fake_lwip_set_listen_result(ERR_MEM);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_network_lwip_listen(&s_ctx.handle, TEST_PORT, 1u));
  TEST_ASSERT_EQUAL_UINT(0u, fake_lwip_live_pcbs());
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  fake_lwip_set_listen_result(ERR_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_listen(&s_ctx.handle, TEST_PORT, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_network_lwip_listen(&s_ctx.handle, TEST_PORT, 1u));
}

void test_network_owner_callbacks_are_paired_and_preserved_through_close_retry(void) {
  s_ctx.config.owner_enter = owner_enter;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_init(&s_ctx.handle, &s_ctx.config));
  s_ctx.config.owner_leave = owner_leave;
  s_ctx.config.owner_context = &s_ctx;
  connect_numeric();
  TEST_ASSERT_EQUAL(1, s_ctx.transport.send(s_ctx.transport.context, "x", 1u));
  esy_network_connection_status_t status;
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.connection.get_status(s_ctx.connection.stream.context, &status));
  TEST_ASSERT_EQUAL_UINT(s_ctx.owner_enters, s_ctx.owner_leaves);
  TEST_ASSERT_TRUE(s_ctx.owner_enters > 0u);
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_MEM, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL_UINT(s_ctx.owner_enters, s_ctx.owner_leaves);
}

void test_connection_getter_and_status_reject_invalid_inputs_without_overwriting_outputs(void) {
  esy_network_connection_t output = {.stream = {.context = &s_ctx}};
  uint8_t initial[sizeof(output)];
  memcpy(initial, &output, sizeof(initial));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_get_connection(NULL, &output));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_network_lwip_get_connection(&s_ctx.handle, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_network_lwip_get_connection(&s_ctx.handle, &output));
  TEST_ASSERT_EQUAL_MEMORY(initial, &output, sizeof(initial));
  initialize();
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_get_connection(&s_ctx.handle, &output));
  TEST_ASSERT_EQUAL_PTR(s_ctx.transport.context, output.stream.context);
  TEST_ASSERT_EQUAL_PTR(s_ctx.transport.send, output.stream.send);
  TEST_ASSERT_EQUAL_PTR(s_ctx.transport.recv, output.stream.recv);
  TEST_ASSERT_EQUAL_PTR(s_ctx.transport.progress, output.stream.progress);
  TEST_ASSERT_NOT_NULL(output.send_all);
  TEST_ASSERT_NOT_NULL(output.get_status);
  esy_network_connection_status_t status = {.state = ESY_NETWORK_CONNECTION_EOF,
      .session_id = 42u, .send_capacity = 3u, .unacked_bytes = 2u, .rx_available = 1u};
  uint8_t initial_status[sizeof(status)];
  memcpy(initial_status, &status, sizeof(status));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, output.get_status(NULL, &status));
  TEST_ASSERT_EQUAL_MEMORY(initial_status, &status, sizeof(status));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, output.get_status(output.stream.context, NULL));
  memcpy(initial, &output, sizeof(initial));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_deinit(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, output.get_status(output.stream.context, &status));
  TEST_ASSERT_EQUAL_MEMORY(initial_status, &status, sizeof(status));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_network_lwip_get_connection(&s_ctx.handle, &output));
  TEST_ASSERT_EQUAL_MEMORY(initial, &output, sizeof(initial));
}

static void assert_connection_state(esy_network_connection_state_t expected) {
  esy_network_connection_status_t status;
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.connection.get_status(s_ctx.connection.stream.context, &status));
  TEST_ASSERT_EQUAL(expected, status.state);
}

void test_connection_status_folds_endpoint_setup_and_errors_into_inactive(void) {
  initialize();
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_listen(&s_ctx.handle, TEST_PORT, 1u));
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
  struct tcp_pcb *peer = fake_lwip_accept();
  TEST_ASSERT_NOT_NULL(peer);
  assert_connection_state(ESY_NETWORK_CONNECTION_OPEN);
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_fin(peer));
  assert_connection_state(ESY_NETWORK_CONNECTION_EOF);
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_MEM, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_close_peer(&s_ctx.handle));
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
  fake_lwip_fail(ERR_OK, ERR_OK, ERR_OK, ERR_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close_peer(&s_ctx.handle));
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
  peer = fake_lwip_accept();
  TEST_ASSERT_NOT_NULL(peer);
  assert_connection_state(ESY_NETWORK_CONNECTION_OPEN);
  fake_lwip_error(peer);
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_begin(&s_ctx.handle, "192.0.2.1", TEST_PORT));
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
  TEST_ASSERT_EQUAL(ERR_OK, fake_lwip_connect());
  assert_connection_state(ESY_NETWORK_CONNECTION_OPEN);
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_close(&s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK, esy_network_lwip_begin(&s_ctx.handle, "!broker.invalid", TEST_PORT));
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_network_lwip_close(&s_ctx.handle));
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
  network_lwip_dns_complete(false);
  assert_connection_state(ESY_NETWORK_CONNECTION_INACTIVE);
}
