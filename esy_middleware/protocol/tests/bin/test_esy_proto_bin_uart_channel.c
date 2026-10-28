/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_bin_uart_channel.h"

#include <string.h>

#include "unity.h"

#include "esy_common/protocol/esy_proto_bin.h"
#include "fake_esy_hal_dma.h"
#include "fake_esy_hal_system.h"
#include "fake_esy_hal_uart.h"

#define TEST_TX_PAYLOAD_MAX  16u
#define TEST_RX_PAYLOAD_MAX  32u
#define TEST_TX_BLOCK_COUNT  4u
#define TEST_RX_STREAM_BYTES 64u
#define TEST_EVENT_COUNT 8u
typedef struct {
  esy_proto_bin_uart_channel_t channel;
  esy_uart_t uart;
  ESY_PROTO_BIN_UART_CHANNEL_STATIC_STORAGE(storage, TEST_TX_BLOCK_COUNT,
                                        TEST_TX_PAYLOAD_MAX,
                                        TEST_RX_PAYLOAD_MAX,
                                        TEST_RX_STREAM_BYTES);
  esy_proto_bin_uart_channel_static_resources_t resources;
  uint32_t data_count;
  bool try_deinit_on_data;
  esy_err_t callback_deinit_result;
  uint32_t event_count;
  esy_proto_bin_tp_event_t last_event;
  esy_proto_bin_tp_event_t events[TEST_EVENT_COUNT];
  uint32_t event_ids[TEST_EVENT_COUNT];
  bool try_deinit_on_event;
  uint32_t deferred_start_poll;
  esy_err_t deferred_poll_result;
  uint8_t payload[TEST_RX_PAYLOAD_MAX];
  uint32_t payload_len;
} test_ctx_t;

static test_ctx_t s_ctx;

static void on_data(const esy_proto_bin_frame_t *frame, void *user_data) {
  test_ctx_t *ctx = (test_ctx_t *)user_data;

  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  ctx->payload_len = esy_proto_bin_frame_get_data_len(frame);
  memcpy(ctx->payload, esy_proto_bin_frame_get_data(frame), ctx->payload_len);
  ctx->data_count++;
  if (ctx->try_deinit_on_data) {
    ctx->callback_deinit_result = esy_proto_bin_uart_channel_deinit(&ctx->channel);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_uart_channel_poll(&ctx->channel));
  }
}

static void on_event(esy_proto_bin_tp_event_t event, uint32_t transaction_id,
                     void *user_data) {
  test_ctx_t *ctx = (test_ctx_t *)user_data;

  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_LESS_THAN_UINT32(TEST_EVENT_COUNT, ctx->event_count);
  ctx->events[ctx->event_count] = event;
  ctx->event_ids[ctx->event_count] = transaction_id;
  ctx->last_event = event;
  ctx->event_count++;
  if (ctx->try_deinit_on_event) {
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_uart_channel_deinit(&ctx->channel));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_uart_channel_poll(&ctx->channel));
  }
}

static void fill_config(esy_proto_bin_uart_channel_config_t *config) {
  esy_proto_bin_uart_channel_default_cfg(config);
  ESY_PROTO_BIN_UART_CHANNEL_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
  config->uart = &s_ctx.uart;
  config->tx_payload_max = TEST_TX_PAYLOAD_MAX;
  config->rx_payload_max = TEST_RX_PAYLOAD_MAX;
  config->tx_completion_timeout_ms = 10u;
  config->acknowledgement_timeout_ms = 10u;
  config->retries = 1u;
  config->tx_queue_block_count = TEST_TX_BLOCK_COUNT;
  config->data_cb = on_data;
  config->event_cb = on_event;
  config->callback_user_data = &s_ctx;
  config->static_resources = &s_ctx.resources;
}

static void build_data_frame(uint8_t *buffer, size_t capacity,
                             const uint8_t *payload, size_t payload_len,
                             size_t *frame_len) {
  esy_proto_bin_frame_t frame;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_frame_init(&frame, buffer, capacity));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_frame_put_header(
                                      &frame, ESY_PROTO_BIN_CMD_DATA, 3u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_proto_bin_frame_put_data(&frame, payload, payload_len));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_frame_finish(&frame));
  *frame_len = frame.len;
}

static uint32_t queue_payload(uint8_t payload) {
  esy_proto_bin_session_t *session = esy_proto_bin_uart_channel_get_session(&s_ctx.channel);
  TEST_ASSERT_NOT_NULL(session);
  uint8_t *buffer = esy_proto_bin_session_get_tx_buffer(session, NULL);
  TEST_ASSERT_NOT_NULL(buffer);
  *buffer = payload;
  uint32_t id = ESY_PROTO_BIN_INVALID_TRANSACTION_ID;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(session, 1u, &id));
  TEST_ASSERT_NOT_EQUAL(ESY_PROTO_BIN_INVALID_TRANSACTION_ID, id);
  return id;
}

static uint8_t last_dma_sequence(uint8_t *command) {
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_NOT_NULL(slot->last_dma_src);
  esy_proto_bin_parser_t parser;
  esy_proto_bin_frame_t frame;
  uint8_t bytes[64u];
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_parser_init(&parser));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&frame, bytes, sizeof(bytes)));
  esy_proto_bin_feed_result_t result = ESY_PROTO_BIN_FEED_INCOMPLETE;
  for (size_t i = 0u; i < slot->last_dma_len; ++i)
    result = esy_proto_bin_parser_feed(&parser, &frame,
        ((const uint8_t *)slot->last_dma_src)[i]);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_FEED_OK, result);
  if (command != NULL) *command = esy_proto_bin_frame_get_cmd(&frame);
  return esy_proto_bin_frame_get_seq(&frame);
}

static void receive_ack(uint8_t sequence) {
  esy_proto_bin_frame_t frame;
  uint8_t bytes[32u];
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_init(&frame, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_put_header(&frame, ESY_PROTO_BIN_CMD_ACK, sequence));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_frame_finish(&frame));
  TEST_ASSERT_EQUAL_size_t(frame.len,
      fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, frame.buf, frame.len));
}

static void defer_queued_start(esy_uart_t *uart) {
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(uart);
  if (slot->poll_count == s_ctx.deferred_start_poll) {
    slot->poll_hook = NULL;
    slot->poll_result = s_ctx.deferred_poll_result != ESY_OK
        ? s_ctx.deferred_poll_result : ESY_ERR_BUSY;
  }
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_system_reset();
  fake_esy_hal_dma_reset();
  fake_esy_hal_uart_reset();
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
}

void tearDown(void) { (void)esy_proto_bin_uart_channel_deinit(&s_ctx.channel); }

void test_init_starts_rx_stream_and_exposes_transport(void) {
  esy_proto_bin_uart_channel_config_t config;
  fake_esy_hal_uart_slot_t *slot;

  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));

  slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_PTR(s_ctx.storage.rx_stream_storage,
                        slot->rx_stream_storage);
  TEST_ASSERT_NOT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
}

void test_poll_consumes_rx_stream_without_blocking_receive(void) {
  esy_proto_bin_uart_channel_config_t config;
  fake_esy_hal_uart_slot_t *slot;
  const uint8_t payload[] = {0x11u, 0x22u, 0x33u};
  uint8_t frame[64];
  size_t frame_len;

  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  build_data_frame(frame, sizeof(frame), payload, sizeof(payload), &frame_len);

  TEST_ASSERT_EQUAL_UINT(frame_len, fake_esy_hal_uart_push_rx_stream(
                                        &s_ctx.uart, frame, frame_len));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));

  slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT32(0u, slot->receive_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.data_count);
  TEST_ASSERT_EQUAL_UINT32(sizeof(payload), s_ctx.payload_len);
  TEST_ASSERT_EQUAL_MEMORY(payload, s_ctx.payload, sizeof(payload));
}

void test_poll_restarts_stream_after_overflow(void) {
  esy_proto_bin_uart_channel_config_t config;
  uint8_t bytes[TEST_RX_STREAM_BYTES + 1u] = {0};
  fake_esy_hal_uart_slot_t *slot;

  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  TEST_ASSERT_EQUAL_UINT(
      TEST_RX_STREAM_BYTES,
      fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, bytes, sizeof(bytes)));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_PTR(s_ctx.storage.rx_stream_storage,
                        slot->rx_stream_storage);
  TEST_ASSERT_EQUAL_UINT(0u, slot->rx_stream_used);
}

static void retire_uart_during_poll(esy_uart_t *uart) {
  fake_esy_hal_uart_find(uart)->poll_hook = NULL;
  fake_esy_hal_uart_complete_last_dma(uart);
}

void test_ack_timeout_starts_after_uart_dma_completes(void) {
  esy_proto_bin_uart_channel_config_t config;
  esy_proto_bin_session_t *transport;
  uint8_t *buffer;
  uint32_t capacity;

  fill_config(&config);
  config.retries = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  transport = esy_proto_bin_uart_channel_get_session(&s_ctx.channel);
  buffer = esy_proto_bin_session_get_tx_buffer(transport, &capacity);
  TEST_ASSERT_NOT_NULL(buffer);
  TEST_ASSERT_GREATER_THAN_UINT32(0u, capacity);
  buffer[0] = 0x51u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_session_commit_tx(transport, 1u, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));

  fake_esy_hal_system_set_tick(9u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.event_count);

  fake_esy_hal_uart_find(&s_ctx.uart)->poll_hook = retire_uart_during_poll;
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  fake_esy_hal_system_set_tick(19u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.event_count);

  fake_esy_hal_system_set_tick(20u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, s_ctx.last_event);
}

void test_failed_uart_poll_retains_channel_and_dma_until_retry(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  esy_proto_bin_session_t *transport = esy_proto_bin_uart_channel_get_session(&s_ctx.channel);
  uint8_t *buffer = esy_proto_bin_session_get_tx_buffer(transport, NULL);
  TEST_ASSERT_NOT_NULL(buffer);
  buffer[0] = 0x51u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(transport, 1u, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->poll_result = ESY_ERR_IO;
  slot->poll_hook = retire_uart_during_poll;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_TRUE(slot->tx_busy);
  TEST_ASSERT_NOT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.event_count);
  slot->poll_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_FALSE(slot->tx_busy);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
}

void test_failed_submit_preserves_work_buffer_until_poll_or_abort_recovery(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  esy_proto_bin_session_t *transport = esy_proto_bin_uart_channel_get_session(&s_ctx.channel);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->send_dma_result = ESY_ERR_IO;
  slot->retain_failed_submission = true;
  slot->abort_result = ESY_ERR_IO;
  uint8_t *buffer = esy_proto_bin_session_get_tx_buffer(transport, NULL);
  TEST_ASSERT_NOT_NULL(buffer);
  *buffer = 0x51u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(transport, 1u, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_TRUE(slot->tx_busy);
  TEST_ASSERT_EQUAL_UINT32(0u, slot->abort_count);
  uint8_t borrowed[sizeof(s_ctx.storage.tx_work_buf)];
  memcpy(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  buffer = esy_proto_bin_session_get_tx_buffer(transport, NULL);
  TEST_ASSERT_NOT_NULL(buffer);
  *buffer = 0x72u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(transport, 1u, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, slot->abort_count);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL_MEMORY(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, slot->abort_count);
  slot->abort_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_TRUE(slot->tx_busy);
  TEST_ASSERT_EQUAL_UINT32(3u, slot->abort_count);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL_MEMORY(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  slot->send_dma_result = ESY_OK;
  slot->retain_failed_submission = false;
  slot->poll_hook = retire_uart_during_poll;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(3u, slot->abort_count);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_FALSE(slot->tx_busy);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.event_count);
  TEST_ASSERT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
  slot->abort_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_FALSE(slot->tx_busy);
}

void test_uart_cleanup_failure_retains_handle_and_retry_stops_stream(void) {
  esy_proto_bin_uart_channel_config_t cfg;
  fill_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
  fake_esy_hal_uart_slot_t* slot =
      fake_esy_hal_uart_find(&s_ctx.uart);
  slot->rx_stream_stop_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
  slot->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
}
void test_uart_overflow_discards_partial_parser_before_new_frame(void) {
  esy_proto_bin_uart_channel_config_t cfg;
  fill_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
  const uint8_t partial[] = {ESY_PROTO_BIN_HEAD, ESY_PROTO_BIN_CMD_DATA, 1, 0,
                             24};
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, partial, sizeof(partial));
  esy_proto_bin_uart_channel_poll(&s_ctx.channel);
  uint8_t garbage[TEST_RX_STREAM_BYTES + 1] = {0};
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, garbage, sizeof(garbage));
  esy_proto_bin_uart_channel_poll(&s_ctx.channel);
  uint8_t frame[32], payload = 42;
  size_t len;
  build_data_frame(frame, sizeof(frame), &payload, 1, &len);
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, frame, len);
  esy_proto_bin_uart_channel_poll(&s_ctx.channel);
  TEST_ASSERT_EQUAL_UINT(1, s_ctx.data_count);
}

void test_uart_deinit_aborts_borrowed_dma_before_releasing_handle(void) {
  esy_proto_bin_uart_channel_config_t cfg;
  fill_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
  esy_proto_bin_session_t* t =
      esy_proto_bin_uart_channel_get_session(&s_ctx.channel);
  uint8_t* p = esy_proto_bin_session_get_tx_buffer(t, NULL);
  TEST_ASSERT_NOT_NULL(p);
  *p = 1;
  esy_proto_bin_session_commit_tx(t, 1, NULL);
  esy_proto_bin_uart_channel_poll(&s_ctx.channel);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&s_ctx.uart, true));
  TEST_ASSERT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
}

void test_failed_rx_start_retains_channel_until_stop_cleanup_succeeds(void) {
  esy_proto_bin_uart_channel_config_t cfg;
  fill_config(&cfg);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->rx_stream_start_result = ESY_ERR_IO;
  slot->retain_failed_rx_start = true;
  slot->rx_stream_stop_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
  TEST_ASSERT_NOT_NULL(slot->rx_stream_callback);
  TEST_ASSERT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  slot->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_NULL(slot->rx_stream_callback);
  slot->rx_stream_start_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
}

void test_rx_restart_failure_must_be_stopped_before_retrying_start(void) {
  esy_proto_bin_uart_channel_config_t cfg;
  fill_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &cfg));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->rx_stream_start_result = ESY_ERR_IO;
  slot->retain_failed_rx_start = true;
  slot->rx_stream_callback(&s_ctx.uart, ESY_UART_RX_STREAM_EVENT_ERROR,
      slot->rx_stream_user_data);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  uint32_t starts = slot->rx_stream_start_count;
  slot->rx_stream_stop_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(starts, slot->rx_stream_start_count);
  slot->rx_stream_stop_result = slot->rx_stream_start_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
}

void test_uart_callback_deinit_rejection_does_not_stop_rx_or_abort_dma(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  s_ctx.try_deinit_on_data = true;
  const uint8_t payload = 0x41u;
  uint8_t frame[64u];
  size_t length;
  build_data_frame(frame, sizeof(frame), &payload, 1u, &length);
  TEST_ASSERT_EQUAL_UINT(length,
      fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, frame, length));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.callback_deinit_result);
  TEST_ASSERT_NOT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_NOT_NULL(slot->rx_stream_callback);
}

static void assert_uart_storage_rejected(const esy_proto_bin_uart_channel_config_t *config) {
  esy_proto_bin_uart_channel_t before = s_ctx.channel;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_proto_bin_uart_channel_init(&s_ctx.channel, config));
  TEST_ASSERT_EQUAL_MEMORY(&before, &s_ctx.channel, sizeof(before));
  /* Any premature HAL RX peek would create a fake slot, even without RX data. */
  TEST_ASSERT_NULL(fake_esy_hal_uart_find(&s_ctx.uart));
  memset(&s_ctx.channel, 0, sizeof(s_ctx.channel));
}

void test_uart_init_rejects_buffer_over_whole_channel_prefix_and_can_retry(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  fake_esy_hal_uart_reset();
  config.tx_payload_max = 1u;
  s_ctx.resources.tx_work_buf = s_ctx.channel.priv.bytes;
  s_ctx.resources.tx_work_buf_size = 1u + ESY_PROTO_BIN_OVERHEAD_BYTES;
  assert_uart_storage_rejected(&config);
  s_ctx.resources.tx_work_buf = s_ctx.storage.tx_work_buf;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.storage.tx_work_buf);
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  esy_proto_bin_session_t *session = esy_proto_bin_uart_channel_get_session(&s_ctx.channel);
  uint8_t *buffer = esy_proto_bin_session_get_tx_buffer(session, NULL);
  TEST_ASSERT_NOT_NULL(buffer);
  *buffer = 0x41u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(session, 1u, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_hal_uart_find(&s_ctx.uart)->send_dma_count);
}

void test_uart_init_rejects_rx_stream_aliases_with_every_protocol_buffer(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  fake_esy_hal_uart_reset();
  uint8_t *buffers[] = {s_ctx.resources.tx_queue_storage,
      s_ctx.resources.tx_work_buf, s_ctx.resources.rx_work_buf};
  for (size_t i = 0u; i < sizeof(buffers) / sizeof(buffers[0]); ++i) {
    s_ctx.resources.rx_stream_storage = buffers[i];
    assert_uart_storage_rejected(&config);
  }
  s_ctx.resources.rx_stream_storage = s_ctx.channel.priv.bytes;
  assert_uart_storage_rejected(&config);
  s_ctx.resources.rx_stream_storage = (uint8_t *)(uintptr_t)(UINTPTR_MAX - 4u);
  assert_uart_storage_rejected(&config);
}

void test_uart_init_rejects_descriptor_aliases_and_work_buffer_overflow(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  fake_esy_hal_uart_reset();
  s_ctx.resources.tx_work_buf = (uint8_t *)&config;
  s_ctx.resources.tx_work_buf_size = sizeof(config);
  assert_uart_storage_rejected(&config);
  s_ctx.resources.tx_work_buf = (uint8_t *)&s_ctx.resources;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.resources);
  assert_uart_storage_rejected(&config);
  s_ctx.resources.tx_work_buf = s_ctx.storage.tx_work_buf;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.storage.tx_work_buf);
  memcpy(s_ctx.channel.priv.bytes, &config, sizeof(config));
  assert_uart_storage_rejected((const esy_proto_bin_uart_channel_config_t *)
      (const void *)s_ctx.channel.priv.bytes);
  memcpy(s_ctx.channel.priv.bytes, &s_ctx.resources, sizeof(s_ctx.resources));
  config.static_resources = (const esy_proto_bin_uart_channel_static_resources_t *)
      (const void *)s_ctx.channel.priv.bytes;
  assert_uart_storage_rejected(&config);
  config.static_resources = &s_ctx.resources;
  s_ctx.resources.tx_work_buf = (uint8_t *)(uintptr_t)(UINTPTR_MAX - 4u);
  assert_uart_storage_rejected(&config);
}

void test_scheduler_queue_acceptance_remains_borrowed_and_deinit_cancels_before_dma_start(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  esy_proto_bin_session_t *session = esy_proto_bin_uart_channel_get_session(&s_ctx.channel);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  s_ctx.deferred_start_poll = slot->poll_count + 2u;
  slot->poll_hook = defer_queued_start;
  (void)queue_payload(0x51u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_FALSE(esy_proto_bin_session_is_idle(session));
  TEST_ASSERT_EQUAL(0u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
  uint8_t borrowed[sizeof(s_ctx.storage.tx_work_buf)];
  memcpy(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  slot->poll_result = ESY_OK;
  uint32_t polls = slot->poll_count;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_EQUAL(polls + 1u, slot->poll_count);
  TEST_ASSERT_EQUAL(0u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(0u, slot->abort_count);
  TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_MEMORY(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  TEST_ASSERT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
}

void test_scheduler_ack_wait_starts_only_after_terminal_callback_and_wire_idle(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  config.tx_completion_timeout_ms = 30u;
  config.retries = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  uint32_t id = queue_payload(0x51u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->keep_tx_busy_after_dma = true;
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  fake_esy_hal_system_set_tick(19u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
  slot->tx_busy = false;
  fake_esy_hal_system_set_tick(20u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  fake_esy_hal_system_set_tick(29u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
  fake_esy_hal_system_set_tick(30u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, s_ctx.events[0]);
  TEST_ASSERT_EQUAL(id, s_ctx.event_ids[0]);
}

void test_scheduler_async_error_targets_its_transaction_and_late_error_preserves_next_payload(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  config.retries = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  s_ctx.try_deinit_on_event = true;
  uint32_t first = queue_payload(0x51u);
  uint32_t second = queue_payload(0x72u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  uint8_t old_sequence = last_dma_sequence(NULL);
  fake_esy_hal_uart_fail_last_dma(&s_ctx.uart);
  receive_ack(old_sequence);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, s_ctx.events[0]);
  TEST_ASSERT_EQUAL(first, s_ctx.event_ids[0]);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, slot->send_dma_count);
  uint8_t second_sequence = last_dma_sequence(NULL);
  receive_ack(second_sequence);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, s_ctx.events[1]);
  TEST_ASSERT_EQUAL(second, s_ctx.event_ids[1]);
  uint32_t third = queue_payload(0x93u);
  fake_esy_hal_uart_fail_last_dma(&s_ctx.uart);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(2u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(3u, slot->send_dma_count);
  uint8_t third_sequence = last_dma_sequence(NULL);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  receive_ack(third_sequence);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(3u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, s_ctx.events[2]);
  TEST_ASSERT_EQUAL(third, s_ctx.event_ids[2]);
  TEST_ASSERT_NOT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
}

void test_scheduler_retained_failed_submit_keeps_frame_until_abort_and_preserves_queued_data(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->send_dma_result = ESY_ERR_IO;
  slot->retain_failed_submission = true;
  slot->abort_result = ESY_ERR_TIMEOUT;
  uint32_t first = queue_payload(0x51u);
  uint32_t next = queue_payload(0x72u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  uint8_t borrowed[sizeof(s_ctx.storage.tx_work_buf)];
  memcpy(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  uint32_t polls = slot->poll_count;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(polls + 1u, slot->poll_count);
  TEST_ASSERT_EQUAL(1u, slot->abort_count);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_MEMORY(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  slot->abort_result = ESY_OK;
  slot->send_dma_result = ESY_OK;
  slot->retain_failed_submission = false;
  polls = slot->poll_count;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(polls + 1u, slot->poll_count);
  TEST_ASSERT_EQUAL(2u, slot->abort_count);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, s_ctx.events[0]);
  TEST_ASSERT_EQUAL(first, s_ctx.event_ids[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, slot->send_dma_count);
  uint8_t sequence = last_dma_sequence(NULL);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  receive_ack(sequence);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, s_ctx.events[1]);
  TEST_ASSERT_EQUAL(next, s_ctx.event_ids[1]);
}

void test_scheduler_deinit_abort_failure_closes_admission_and_poll_only_retries_cleanup(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  (void)queue_payload(0x51u);
  (void)queue_payload(0x72u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->abort_result = ESY_ERR_IO;
  uint8_t borrowed[sizeof(s_ctx.storage.tx_work_buf)];
  memcpy(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  uint32_t polls = slot->poll_count;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_EQUAL(polls + 1u, slot->poll_count);
  TEST_ASSERT_EQUAL(1u, slot->abort_count);
  TEST_ASSERT_NULL(esy_proto_bin_uart_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_NOT_NULL(slot->last_dma_callback_arg);
  TEST_ASSERT_NOT_NULL(slot->rx_stream_callback);
  const uint8_t payload = 0x44u;
  uint8_t frame[32u]; size_t frame_length;
  build_data_frame(frame, sizeof(frame), &payload, 1u, &frame_length);
  TEST_ASSERT_EQUAL_size_t(frame_length,
      fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, frame, frame_length));
  polls = slot->poll_count;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(polls + 1u, slot->poll_count);
  TEST_ASSERT_EQUAL(2u, slot->abort_count);
  TEST_ASSERT_EQUAL(0u, s_ctx.data_count);
  TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL_MEMORY(borrowed, s_ctx.storage.tx_work_buf, sizeof(borrowed));
  slot->abort_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_NULL(slot->last_dma_callback_arg);
  TEST_ASSERT_NULL(slot->rx_stream_callback);
  TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
}

void test_scheduler_inflight_tx_does_not_consume_or_block_the_independent_rx_stream(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  (void)queue_payload(0x51u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  const uint8_t payload = 0x44u;
  uint8_t frame[32u]; size_t frame_length;
  build_data_frame(frame, sizeof(frame), &payload, 1u, &frame_length);
  TEST_ASSERT_EQUAL_size_t(frame_length,
      fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, frame, frame_length));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_EQUAL(1u, s_ctx.data_count);
  TEST_ASSERT_EQUAL(0x44u, s_ctx.payload[0]);
  TEST_ASSERT_EQUAL(0u, slot->rx_stream_used);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_TRUE(slot->tx_busy);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, slot->send_dma_count);
  uint8_t command;
  TEST_ASSERT_EQUAL(3u, last_dma_sequence(&command));
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_CMD_ACK, command);
  TEST_ASSERT_EQUAL(1u, s_ctx.data_count);
}

void test_scheduler_control_failure_retries_the_same_ack_without_duplicate_data_delivery(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  const uint8_t payload = 0x44u;
  uint8_t frame[32u]; size_t frame_length;
  build_data_frame(frame, sizeof(frame), &payload, 1u, &frame_length);
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, frame, frame_length);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(1u, s_ctx.data_count);
  uint8_t command;
  TEST_ASSERT_EQUAL(3u, last_dma_sequence(&command));
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_CMD_ACK, command);
  fake_esy_hal_uart_fail_last_dma(&s_ctx.uart);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, s_ctx.events[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_INVALID_TRANSACTION_ID, s_ctx.event_ids[0]);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_uart_find(&s_ctx.uart)->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, fake_esy_hal_uart_find(&s_ctx.uart)->send_dma_count);
  TEST_ASSERT_EQUAL(3u, last_dma_sequence(&command));
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_CMD_ACK, command);
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, frame, frame_length);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(1u, s_ctx.data_count);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(3u, last_dma_sequence(&command));
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_CMD_ACK, command);
  TEST_ASSERT_EQUAL(1u, s_ctx.data_count);
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
}

void test_scheduler_immediate_backend_rejection_is_returned_to_owner_with_matching_id(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->send_dma_result = ESY_ERR_NOT_SUPPORTED;
  uint32_t failed = queue_payload(0x51u);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, s_ctx.events[0]);
  TEST_ASSERT_EQUAL(failed, s_ctx.event_ids[0]);
  TEST_ASSERT_FALSE(slot->tx_busy);
  TEST_ASSERT_EQUAL(0u, slot->abort_count);
  slot->send_dma_result = ESY_OK;
  (void)queue_payload(0x72u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
}

void test_scheduler_delayed_start_then_idle_rejection_cannot_start_two_frames_in_one_poll(void) {
  esy_proto_bin_uart_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_init(&s_ctx.channel, &config));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  s_ctx.deferred_start_poll = slot->poll_count + 2u;
  s_ctx.deferred_poll_result = ESY_ERR_IO;
  slot->poll_hook = defer_queued_start;
  uint32_t first = queue_payload(0x51u);
  uint32_t second = queue_payload(0x72u);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(0u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(0u, s_ctx.event_count);
  slot->poll_result = ESY_OK;
  slot->send_dma_result = ESY_ERR_NOT_SUPPORTED;
  uint32_t polls = slot->poll_count;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(polls + 1u, slot->poll_count);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, s_ctx.events[0]);
  TEST_ASSERT_EQUAL(first, s_ctx.event_ids[0]);
  slot->send_dma_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, slot->send_dma_count);
  uint8_t sequence = last_dma_sequence(NULL);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  receive_ack(sequence);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_uart_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(2u, s_ctx.event_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_BIN_TP_EVENT_ACK, s_ctx.events[1]);
  TEST_ASSERT_EQUAL(second, s_ctx.event_ids[1]);
}
