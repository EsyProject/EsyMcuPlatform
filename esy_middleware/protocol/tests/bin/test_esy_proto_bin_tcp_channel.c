/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <string.h>
#include "esy_middleware/protocol/esy_proto_bin_tcp_channel.h"
#include "fake_esy_hal_system.h"

#define TEST_PAYLOAD_BYTES 16u
#define TEST_BLOCK_COUNT 4u
#define TEST_RX_BYTES 512u

typedef struct {
  esy_proto_bin_tcp_channel_t channel;
  esy_proto_bin_tcp_channel_config_t config;
  ESY_PROTO_BIN_TCP_CHANNEL_STATIC_STORAGE(storage, TEST_BLOCK_COUNT,
                                      TEST_PAYLOAD_BYTES, TEST_PAYLOAD_BYTES);
  esy_proto_bin_tcp_channel_static_resources_t resources;
  esy_network_connection_status_t status;
  uint8_t rx[TEST_RX_BYTES];
  size_t rx_length;
  size_t rx_offset;
  uint32_t writes;
  uint32_t progress_calls;
  uint32_t status_calls;
  uint32_t deliveries;
  uint32_t errors;
  uint32_t acknowledgments;
  uint32_t tx_timeouts;
  uint32_t timeouts;
  esy_err_t progress_result;
  esy_err_t status_result;
  int32_t send_result;
  bool reenter_progress;
  bool backpressure;
} tcp_test_ctx_t;

static tcp_test_ctx_t s_ctx;

static esy_err_t connection_status(void *context, esy_network_connection_status_t *status) {
  tcp_test_ctx_t *ctx = context;
  ++ctx->status_calls;
  if (ctx->status_result != ESY_OK) return ctx->status_result;
  *status = ctx->status;
  status->rx_available = (uint32_t)(ctx->rx_length - ctx->rx_offset);
  return ESY_OK;
}

static int32_t connection_send_all(void *context, const void *data, size_t size) {
  tcp_test_ctx_t *ctx = context;
  TEST_ASSERT_NOT_NULL(data);
  if (ctx->send_result != ESY_OK) return ctx->send_result;
  if (ctx->backpressure) return 0;
  if (size > ctx->status.send_capacity) return 0;
  ++ctx->writes;
  ctx->status.unacked_bytes += (uint32_t)size;
  return (int32_t)size;
}

static int32_t stream_receive(void *context, void *data, size_t size) {
  tcp_test_ctx_t *ctx = context;
  if (ctx->rx_offset == ctx->rx_length) return 0;
  if (size > ctx->rx_length - ctx->rx_offset) size = ctx->rx_length - ctx->rx_offset;
  memcpy(data, ctx->rx + ctx->rx_offset, size);
  ctx->rx_offset += size;
  return (int32_t)size;
}

static esy_err_t stream_progress(void *context) {
  tcp_test_ctx_t *ctx = context;
  ++ctx->progress_calls;
  if (ctx->reenter_progress) {
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_tcp_channel_poll(&ctx->channel));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_tcp_channel_deinit(&ctx->channel));
  }
  return ctx->progress_result;
}

static void on_data(const esy_proto_bin_frame_t *frame, void *context) {
  (void)frame;
  ++((tcp_test_ctx_t *)context)->deliveries;
}

static void on_event(esy_proto_bin_tp_event_t event, uint32_t id, void *context) {
  (void)id;
  tcp_test_ctx_t *ctx = context;
  if (event == ESY_PROTO_BIN_TP_EVENT_IO_ERROR) ++ctx->errors;
  if (event == ESY_PROTO_BIN_TP_EVENT_ACK) ++ctx->acknowledgments;
  if (event == ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT) ++ctx->tx_timeouts;
  if (event == ESY_PROTO_BIN_TP_EVENT_TIMEOUT) ++ctx->timeouts;
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_system_reset();
  esy_proto_bin_tcp_channel_default_cfg(&s_ctx.config);
  ESY_PROTO_BIN_TCP_CHANNEL_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
  s_ctx.config.static_resources = &s_ctx.resources;
  s_ctx.config.connection = (esy_network_connection_t){
      .stream = {.recv = stream_receive, .progress = stream_progress, .context = &s_ctx},
      .send_all = connection_send_all, .get_status = connection_status};
  s_ctx.config.tx_payload_max = TEST_PAYLOAD_BYTES;
  s_ctx.config.rx_payload_max = TEST_PAYLOAD_BYTES;
  s_ctx.config.tx_queue_block_count = TEST_BLOCK_COUNT;
  s_ctx.config.tx_completion_timeout_ms = 10u;
  s_ctx.config.acknowledgement_timeout_ms = 10u;
  s_ctx.config.data_cb = on_data;
  s_ctx.config.event_cb = on_event;
  s_ctx.config.callback_user_data = &s_ctx;
  s_ctx.status.state = ESY_NETWORK_CONNECTION_OPEN;
  s_ctx.status.session_id = 1u;
  s_ctx.status.send_capacity = TEST_RX_BYTES;
}

void tearDown(void) {
  if (esy_proto_bin_tcp_channel_get_session(&s_ctx.channel) != NULL) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_deinit(&s_ctx.channel));
  }
}

static void start(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
}

static void queue_data(void) {
  esy_proto_bin_session_t *transport = esy_proto_bin_tcp_channel_get_session(&s_ctx.channel);
  uint8_t *payload = esy_proto_bin_session_get_tx_buffer(transport, NULL);
  TEST_ASSERT_NOT_NULL(payload);
  *payload = 42u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(transport, 1u, NULL));
}

static void receive_data(void) {
  esy_proto_bin_frame_t frame;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&frame, s_ctx.rx, sizeof(s_ctx.rx)));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_frame_put_header(&frame, ESY_PROTO_BIN_CMD_DATA, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_finish(&frame));
  s_ctx.rx_length = frame.len;
  s_ctx.rx_offset = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
}

void test_channel_waits_for_peer_ack_after_local_acceptance(void) {
  start();
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
  TEST_ASSERT_NOT_EQUAL(0u, s_ctx.status.unacked_bytes);
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.errors);
  /* Local acceptance does not complete transport TX. Peer ACK releases it. */
  s_ctx.status.unacked_bytes = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.errors);
}

void test_channel_defers_whole_frame_until_capacity_is_available(void) {
  start();
  queue_data();
  s_ctx.status.send_capacity = 1u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.writes);
  s_ctx.status.send_capacity = TEST_RX_BYTES;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
}

void test_channel_new_session_discards_old_transactions_and_resets_deduplication(void) {
  start();
  receive_data();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deliveries);
  queue_data();
  queue_data();
  ++s_ctx.status.session_id;
  s_ctx.status.unacked_bytes = 0u;
  receive_data();
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.errors);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.deliveries);
}

void test_channel_observed_disconnect_drops_old_frames_once_before_reconnect(void) {
  start();
  queue_data();
  queue_data();
  s_ctx.status.state = ESY_NETWORK_CONNECTION_INACTIVE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.errors);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.errors);
  s_ctx.status.state = ESY_NETWORK_CONNECTION_OPEN;
  ++s_ctx.status.session_id;
  receive_data();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deliveries);
}

void test_channel_first_connection_preserves_new_queued_frames(void) {
  s_ctx.status.state = ESY_NETWORK_CONNECTION_INACTIVE;
  s_ctx.status.session_id = 0u;
  start();
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.writes);
  s_ctx.status.state = ESY_NETWORK_CONNECTION_OPEN;
  s_ctx.status.session_id = 1u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
}

void test_channel_repeated_init_and_invalid_config_preserve_lifecycle(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_tcp_channel_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_tcp_channel_init(&s_ctx.channel, NULL));
  s_ctx.config.connection.get_status = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
  s_ctx.config.connection.get_status = connection_status;
  s_ctx.config.tx_completion_timeout_ms = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
  s_ctx.config.tx_completion_timeout_ms = 10u;
  start();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_bin_tcp_channel_init(&s_ctx.channel, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_bin_tcp_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_bin_tcp_channel_deinit(NULL));
  TEST_ASSERT_NULL(esy_proto_bin_tcp_channel_get_session(&s_ctx.channel));
  start();
}

void test_channel_provider_errors_propagate_and_owner_reentry_is_rejected(void) {
  start();
  s_ctx.progress_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  s_ctx.progress_result = ESY_OK;
  s_ctx.status_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  s_ctx.status_result = ESY_OK;
  s_ctx.reenter_progress = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
}

void test_channel_peer_ack_completes_tx_even_when_future_send_capacity_is_small(void) {
  start();
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  s_ctx.status.unacked_bytes = 0u;
  s_ctx.status.send_capacity = 1u;
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tx_timeouts);
  fake_esy_hal_system_set_tick(20u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.timeouts);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.tx_timeouts);
}

void test_channel_whole_send_backpressure_preserves_queued_transaction(void) {
  start();
  queue_data();
  s_ctx.backpressure = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.errors);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.writes);
  s_ctx.backpressure = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
}

void test_channel_terminal_progress_error_resets_once_but_busy_preserves_session(void) {
  start();
  queue_data();
  s_ctx.progress_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.errors);
  s_ctx.progress_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.errors);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.errors);
}

void test_channel_fin_drains_last_data_and_waits_for_control_peer_ack(void) {
  start();
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  receive_data();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deliveries);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
  TEST_ASSERT_FALSE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  s_ctx.status.unacked_bytes = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_TRUE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.errors);
}

void test_channel_fin_drains_last_protocol_ack_before_resetting_transactions(void) {
  start();
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  s_ctx.status.unacked_bytes = 0u;
  esy_proto_bin_frame_t frame;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&frame, s_ctx.rx, sizeof(s_ctx.rx)));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_frame_put_header(&frame, ESY_PROTO_BIN_CMD_ACK, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_finish(&frame));
  s_ctx.rx_length = frame.len;
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.acknowledgments);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.errors);
  TEST_ASSERT_TRUE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
}

void test_channel_new_peer_already_closed_does_not_reuse_previous_drain_result(void) {
  start();
  s_ctx.rx[0] = ESY_PROTO_BIN_HEAD;
  s_ctx.rx_length = 1u;
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_TRUE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  ++s_ctx.status.session_id;
  receive_data();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deliveries);
  TEST_ASSERT_FALSE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  s_ctx.status.unacked_bytes = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_TRUE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  s_ctx.status.state = ESY_NETWORK_CONNECTION_INACTIVE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_FALSE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
}

void test_channel_fin_drains_input_over_multiple_budgeted_polls_and_retries_control(void) {
  start();
  memset(s_ctx.rx, 0u, sizeof(s_ctx.rx));
  esy_proto_bin_frame_t frame;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&frame, s_ctx.rx + 300u, 64u));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_proto_bin_frame_put_header(&frame, ESY_PROTO_BIN_CMD_DATA, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_finish(&frame));
  s_ctx.rx_length = 300u + frame.len;
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  s_ctx.backpressure = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.deliveries);
  TEST_ASSERT_FALSE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.deliveries);
  TEST_ASSERT_FALSE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  s_ctx.backpressure = false;
  s_ctx.send_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.writes);
  TEST_ASSERT_FALSE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  s_ctx.send_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
  TEST_ASSERT_FALSE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  s_ctx.status.unacked_bytes = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_TRUE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
}

void test_channel_fin_resets_remaining_inflight_and_queued_frames_once(void) {
  start();
  queue_data();
  queue_data();
  s_ctx.status.state = ESY_NETWORK_CONNECTION_EOF;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_FALSE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  s_ctx.status.unacked_bytes = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.errors);
  TEST_ASSERT_TRUE(esy_proto_bin_tcp_channel_is_peer_drained(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.errors);
}

void test_tcp_deinit_does_not_wait_for_peer_ack_or_report_protocol_completion(void) {
  start();
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_NOT_EQUAL(0u, s_ctx.status.unacked_bytes);
  uint32_t unacked = s_ctx.status.unacked_bytes;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_NULL(esy_proto_bin_tcp_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(unacked, s_ctx.status.unacked_bytes);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.acknowledgments);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.errors);
}

void test_tcp_deinit_succeeds_after_endpoint_close_with_old_tx_pending(void) {
  start();
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  s_ctx.status.state = ESY_NETWORK_CONNECTION_INACTIVE;
  s_ctx.status.unacked_bytes = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_NULL(esy_proto_bin_tcp_channel_get_session(&s_ctx.channel));
}

static void assert_tcp_storage_rejected(const esy_proto_bin_tcp_channel_config_t *config) {
  esy_proto_bin_tcp_channel_t before = s_ctx.channel;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_proto_bin_tcp_channel_init(&s_ctx.channel, config));
  TEST_ASSERT_EQUAL_MEMORY(&before, &s_ctx.channel, sizeof(before));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.status_calls);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.progress_calls);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.writes);
  memset(&s_ctx.channel, 0, sizeof(s_ctx.channel));
}

void test_tcp_requires_connection_extensions_before_provider_access(void) {
  s_ctx.config.connection.send_all = NULL;
  assert_tcp_storage_rejected(&s_ctx.config);
  s_ctx.config.connection.send_all = connection_send_all;
  s_ctx.config.connection.get_status = NULL;
  assert_tcp_storage_rejected(&s_ctx.config);
  s_ctx.config.connection.get_status = connection_status;
  s_ctx.config.connection.stream.recv = NULL;
  assert_tcp_storage_rejected(&s_ctx.config);
  s_ctx.config.connection.stream.recv = stream_receive;
  /* Whole-write consumers do not need the base prefix-send operation. */
  TEST_ASSERT_NULL(s_ctx.config.connection.stream.send);
  start();
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
}

void test_tcp_init_rejects_buffer_over_whole_channel_prefix_before_provider_access(void) {
  s_ctx.config.tx_payload_max = 1u;
  s_ctx.resources.tx_work_buf = s_ctx.channel.priv.bytes;
  s_ctx.resources.tx_work_buf_size = 1u + ESY_PROTO_BIN_OVERHEAD_BYTES;
  assert_tcp_storage_rejected(&s_ctx.config);
  s_ctx.resources.tx_work_buf = s_ctx.storage.tx_work_buf;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.storage.tx_work_buf);
  start();
  queue_data();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_tcp_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.writes);
}

void test_tcp_init_rejects_descriptor_aliases_and_buffer_overflow(void) {
  s_ctx.resources.tx_work_buf = (uint8_t *)&s_ctx.config;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.config);
  assert_tcp_storage_rejected(&s_ctx.config);
  s_ctx.resources.tx_work_buf = (uint8_t *)&s_ctx.resources;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.resources);
  assert_tcp_storage_rejected(&s_ctx.config);
  s_ctx.resources.tx_work_buf = s_ctx.storage.tx_work_buf;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.storage.tx_work_buf);
  memcpy(s_ctx.channel.priv.bytes, &s_ctx.config, sizeof(s_ctx.config));
  assert_tcp_storage_rejected((const esy_proto_bin_tcp_channel_config_t *)
      (const void *)s_ctx.channel.priv.bytes);
  memcpy(s_ctx.channel.priv.bytes, &s_ctx.resources, sizeof(s_ctx.resources));
  s_ctx.config.static_resources = (const esy_proto_bin_tcp_channel_static_resources_t *)
      (const void *)s_ctx.channel.priv.bytes;
  assert_tcp_storage_rejected(&s_ctx.config);
  s_ctx.config.static_resources = &s_ctx.resources;
  s_ctx.resources.tx_work_buf = (uint8_t *)(uintptr_t)(UINTPTR_MAX - 4u);
  assert_tcp_storage_rejected(&s_ctx.config);
}
