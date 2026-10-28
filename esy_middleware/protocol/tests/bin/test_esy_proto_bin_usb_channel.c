/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_bin_usb_channel.h"

#include <string.h>

#include "tusb.h"
#include "fake_esy_hal_system.h"
#include "unity.h"

#define TEST_TX_PAYLOAD_MAX 16u
#define TEST_RX_PAYLOAD_MAX 32u
#define TEST_TX_BLOCK_COUNT 4u
#define TEST_TX_QUEUE_STORAGE_BYTES                                \
  ((TEST_TX_PAYLOAD_MAX + ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES) * \
   TEST_TX_BLOCK_COUNT)
#define TEST_TX_WORK_BYTES      (TEST_TX_PAYLOAD_MAX + ESY_PROTO_BIN_OVERHEAD_BYTES)
#define TEST_RX_WORK_BYTES      (TEST_RX_PAYLOAD_MAX + ESY_PROTO_BIN_OVERHEAD_BYTES)
#define TEST_USB_LINE_STATE_DTR 0x01u

typedef struct {
  esy_usb_cdc_t cdc;
  esy_proto_bin_usb_channel_t channel;
  esy_proto_bin_usb_channel_static_resources_t resources;
  uint8_t tx_queue_storage[TEST_TX_QUEUE_STORAGE_BYTES];
  uint8_t tx_work_buf[TEST_TX_WORK_BYTES];
  uint8_t rx_work_buf[TEST_RX_WORK_BYTES];
  bool reject_tx_during_reset;
  bool try_deinit_on_data;
  esy_err_t callback_deinit_result;
  uint32_t rejected_reservations;
} test_ctx_t;

static test_ctx_t s_ctx;
static uint32_t s_data_count;
static uint32_t s_event_count;
static uint32_t s_last_transaction_id;
static esy_proto_bin_tp_event_t s_last_event;
static uint8_t s_last_payload[TEST_RX_PAYLOAD_MAX];
static size_t s_last_payload_len;

static void data_cb(const esy_proto_bin_frame_t *frame, void *user_data) {
  const uint8_t *payload;

  TEST_ASSERT_EQUAL_PTR(&s_ctx, user_data);
  payload = esy_proto_bin_frame_get_data(frame);
  s_last_payload_len = esy_proto_bin_frame_get_data_len(frame);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(s_last_payload), s_last_payload_len);
  memcpy(s_last_payload, payload, s_last_payload_len);
  s_data_count++;
  if (s_ctx.try_deinit_on_data) {
    s_ctx.callback_deinit_result = esy_proto_bin_usb_channel_deinit(&s_ctx.channel);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  }
}

static void event_cb(esy_proto_bin_tp_event_t event, uint32_t transaction_id,
                     void *user_data) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, user_data);
  s_last_event = event;
  s_last_transaction_id = transaction_id;
  s_event_count++;
  if (s_ctx.reject_tx_during_reset && event == ESY_PROTO_BIN_TP_EVENT_IO_ERROR) {
    esy_proto_bin_session_t *transport =
        esy_proto_bin_usb_channel_get_session(&s_ctx.channel);
    TEST_ASSERT_NULL(esy_proto_bin_session_get_tx_buffer(transport, NULL));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                            esy_proto_bin_session_commit_tx(transport, 1u, NULL));
    s_ctx.rejected_reservations++;
  }
}

static void fill_config(esy_proto_bin_usb_channel_config_t *config) {
  esy_proto_bin_usb_channel_default_cfg(config);
  s_ctx.resources.tx_queue_storage = s_ctx.tx_queue_storage;
  s_ctx.resources.tx_queue_storage_size = sizeof(s_ctx.tx_queue_storage);
  s_ctx.resources.tx_work_buf = s_ctx.tx_work_buf;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.tx_work_buf);
  s_ctx.resources.rx_work_buf = s_ctx.rx_work_buf;
  s_ctx.resources.rx_work_buf_size = sizeof(s_ctx.rx_work_buf);
  config->cdc = &s_ctx.cdc;
  config->tx_payload_max = TEST_TX_PAYLOAD_MAX;
  config->rx_payload_max = TEST_RX_PAYLOAD_MAX;
  config->tx_completion_timeout_ms = 100u;
  config->acknowledgement_timeout_ms = 100u;
  config->retries = 0u;
  config->tx_queue_block_count = TEST_TX_BLOCK_COUNT;
  config->data_cb = data_cb;
  config->event_cb = event_cb;
  config->callback_user_data = &s_ctx;
  config->static_resources = &s_ctx.resources;
}

static void init_cdc(void) {
  esy_usb_cdc_config_t config;

  esy_usb_cdc_default_config(&config);
  config.rx_cb = NULL;
  config.discard_on_disconnect = true;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc, &config));
}

static fake_tinyusb_cdc_itf_state_t *connect_peer(void) {
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(0u);

  TEST_ASSERT_NOT_NULL(itf);
  itf->connected = true;
  itf->line_state = TEST_USB_LINE_STATE_DTR;
  return itf;
}

static void close_peer(void) {
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(0u);
  itf->line_state = 0u;
  tud_cdc_line_state_cb(0u, false, false);
}

static void build_frame(uint8_t *storage, size_t storage_size, uint8_t command,
                        uint8_t sequence, const uint8_t *payload,
                        size_t payload_len, size_t *frame_len) {
  esy_proto_bin_frame_t frame;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_proto_bin_frame_init(&frame, storage, storage_size));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_proto_bin_frame_put_header(&frame, command, sequence));
  if (payload_len > 0u) {
    TEST_ASSERT_EQUAL_INT32(
        ESY_OK, esy_proto_bin_frame_put_data(&frame, payload, payload_len));
  }
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_frame_finish(&frame));
  *frame_len = frame.len;
}

void setUp(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_deinit(&s_ctx.channel));
  esy_usb_cdc_deinit(&s_ctx.cdc);
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_tinyusb_reset();
  fake_esy_hal_system_reset();
  init_cdc();
  s_data_count = 0u;
  s_event_count = 0u;
  s_last_transaction_id = 0u;
  s_last_event = ESY_PROTO_BIN_TP_EVENT_IO_ERROR;
  s_last_payload_len = 0u;
  memset(s_last_payload, 0, sizeof(s_last_payload));
}

void tearDown(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_deinit(&s_ctx.channel));
  esy_usb_cdc_deinit(&s_ctx.cdc);
}

void test_init_exposes_shared_transport_without_owning_cdc(void) {
  esy_proto_bin_usb_channel_config_t config;

  fill_config(&config);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  TEST_ASSERT_NOT_NULL(esy_proto_bin_usb_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_NOT_NULL(s_ctx.cdc.ctx);

  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_NOT_NULL(s_ctx.cdc.ctx);
}

void test_poll_pumps_usb_and_sends_queued_frame(void) {
  esy_proto_bin_usb_channel_config_t config;
  esy_proto_bin_session_t *transport;
  fake_tinyusb_cdc_itf_state_t *itf;
  uint8_t *buffer;
  uint32_t capacity;
  const uint8_t payload[] = {0x41u, 0x42u};

  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  transport = esy_proto_bin_usb_channel_get_session(&s_ctx.channel);
  buffer = esy_proto_bin_session_get_tx_buffer(transport, &capacity);
  TEST_ASSERT_NOT_NULL(buffer);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(sizeof(payload), capacity);
  memcpy(buffer, payload, sizeof(payload));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_proto_bin_session_commit_tx(transport, sizeof(payload), NULL));

  itf = connect_peer();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));

  TEST_ASSERT_EQUAL_UINT32(1u, itf->task_count);
  TEST_ASSERT_EQUAL_UINT32(1u, itf->write_count);
  TEST_ASSERT_EQUAL_UINT32(1u, itf->flush_count);
}

void test_ack_timeout_starts_after_usb_write_queue_releases_frame(void) {
  esy_proto_bin_usb_channel_config_t config;
  esy_proto_bin_session_t *transport;
  fake_tinyusb_cdc_itf_state_t *itf;
  uint8_t *buffer;
  uint32_t capacity;

  fill_config(&config);
  config.tx_completion_timeout_ms = 10u;
  config.acknowledgement_timeout_ms = 10u;
  config.retries = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  transport = esy_proto_bin_usb_channel_get_session(&s_ctx.channel);
  buffer = esy_proto_bin_session_get_tx_buffer(transport, &capacity);
  TEST_ASSERT_NOT_NULL(buffer);
  TEST_ASSERT_GREATER_THAN_UINT32(0u, capacity);
  buffer[0] = 0x43u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_session_commit_tx(transport, 1u, NULL));
  itf = connect_peer();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));

  fake_esy_hal_system_set_tick(9u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_event_count);

  itf->write_available = FAKE_TINYUSB_FIFO_BYTES;
  fake_esy_hal_system_set_tick(10u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  fake_esy_hal_system_set_tick(19u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_event_count);

  fake_esy_hal_system_set_tick(20u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_event_count);
  TEST_ASSERT_EQUAL_INT(ESY_PROTO_BIN_TP_EVENT_TIMEOUT, s_last_event);
}

void test_poll_defers_tx_until_one_complete_frame_fits(void) {
  esy_proto_bin_usb_channel_config_t config;
  esy_proto_bin_session_t *transport;
  fake_tinyusb_cdc_itf_state_t *itf;
  uint8_t *buffer;
  uint32_t capacity;

  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  transport = esy_proto_bin_usb_channel_get_session(&s_ctx.channel);
  buffer = esy_proto_bin_session_get_tx_buffer(transport, &capacity);
  TEST_ASSERT_NOT_NULL(buffer);
  buffer[0] = 0x48u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_session_commit_tx(transport, 1u, NULL));
  itf = connect_peer();
  itf->write_available =
      TEST_TX_PAYLOAD_MAX + ESY_PROTO_BIN_OVERHEAD_BYTES - 1u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, itf->write_count);

  itf->write_available = TEST_TX_PAYLOAD_MAX + ESY_PROTO_BIN_OVERHEAD_BYTES;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, itf->write_count);
}

void test_poll_delivers_data_and_acknowledges_frame(void) {
  esy_proto_bin_usb_channel_config_t config;
  fake_tinyusb_cdc_itf_state_t *itf;
  const uint8_t payload[] = {0x51u, 0x52u, 0x53u};
  uint8_t frame_storage[64];
  size_t frame_len;

  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  itf = connect_peer();
  build_frame(frame_storage, sizeof(frame_storage), ESY_PROTO_BIN_CMD_DATA, 7u,
              payload, sizeof(payload), &frame_len);
  fake_tinyusb_cdc_set_rx_data(0u, frame_storage, (uint32_t)frame_len);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));

  TEST_ASSERT_EQUAL_UINT32(1u, s_data_count);
  TEST_ASSERT_EQUAL_UINT32(sizeof(payload), s_last_payload_len);
  TEST_ASSERT_EQUAL_MEMORY(payload, s_last_payload, sizeof(payload));
  TEST_ASSERT_EQUAL_UINT32(1u, itf->write_count);
}

void test_ack_event_keeps_transport_transaction_identity(void) {
  esy_proto_bin_usb_channel_config_t config;
  esy_proto_bin_session_t *transport;
  uint8_t *buffer;
  uint32_t capacity;
  uint32_t transaction_id;
  uint8_t ack_storage[32];
  size_t ack_len;

  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  (void)connect_peer();
  transport = esy_proto_bin_usb_channel_get_session(&s_ctx.channel);
  buffer = esy_proto_bin_session_get_tx_buffer(transport, &capacity);
  TEST_ASSERT_NOT_NULL(buffer);
  buffer[0] = 0x61u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_session_commit_tx(
                                      transport, 1u, &transaction_id));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));

  build_frame(ack_storage, sizeof(ack_storage), ESY_PROTO_BIN_CMD_ACK, 1u, NULL,
              0u, &ack_len);
  fake_tinyusb_cdc_set_rx_data(0u, ack_storage, (uint32_t)ack_len);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));

  TEST_ASSERT_EQUAL_UINT32(1u, s_event_count);
  TEST_ASSERT_EQUAL_INT(ESY_PROTO_BIN_TP_EVENT_ACK, s_last_event);
  TEST_ASSERT_EQUAL_UINT32(transaction_id, s_last_transaction_id);
}

void test_init_rejects_missing_resources(void) {
  esy_proto_bin_usb_channel_config_t config;

  fill_config(&config);
  config.static_resources = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
}

void test_reopen_between_polls_delivers_reused_sequence_and_preserves_new_rx(void) {
  esy_proto_bin_usb_channel_config_t config;
  uint8_t frame[64];
  size_t frame_len;
  const uint8_t first[] = {0x21u};
  const uint8_t second[] = {0x22u};
  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  fake_tinyusb_cdc_itf_state_t *itf = connect_peer();
  build_frame(frame, sizeof(frame), ESY_PROTO_BIN_CMD_DATA, 7u, first,
              sizeof(first), &frame_len);
  fake_tinyusb_cdc_set_rx_data(0u, frame, (uint32_t)frame_len);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_data_count);
  TEST_ASSERT_EQUAL_UINT32(1u, itf->write_count);

  close_peer();
  (void)connect_peer();
  build_frame(frame, sizeof(frame), ESY_PROTO_BIN_CMD_DATA, 7u, second,
              sizeof(second), &frame_len);
  fake_tinyusb_cdc_set_rx_data(0u, frame, (uint32_t)frame_len);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, s_data_count);
  TEST_ASSERT_EQUAL_MEMORY(second, s_last_payload, sizeof(second));
  TEST_ASSERT_EQUAL_UINT32(2u, itf->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, itf->available);
}

void test_disconnect_discards_inflight_and_queued_tx_and_cancels_reservation(void) {
  esy_proto_bin_usb_channel_config_t config;
  uint32_t ids[2];
  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  fake_tinyusb_cdc_itf_state_t *itf = connect_peer();
  esy_proto_bin_session_t *transport =
      esy_proto_bin_usb_channel_get_session(&s_ctx.channel);
  for (uint32_t i = 0u; i < 2u; ++i) {
    uint8_t *tx = esy_proto_bin_session_get_tx_buffer(transport, NULL);
    TEST_ASSERT_NOT_NULL(tx);
    tx[0] = (uint8_t)i;
    TEST_ASSERT_EQUAL_INT32(ESY_OK,
                            esy_proto_bin_session_commit_tx(transport, 1u, &ids[i]));
  }
  TEST_ASSERT_NOT_NULL(esy_proto_bin_session_get_tx_buffer(transport, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, itf->write_count);

  s_ctx.reject_tx_during_reset = true;
  close_peer();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, s_event_count);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.rejected_reservations);
  TEST_ASSERT_EQUAL_INT(ESY_PROTO_BIN_TP_EVENT_IO_ERROR, s_last_event);
  TEST_ASSERT_EQUAL_UINT32(ids[1], s_last_transaction_id);
  TEST_ASSERT_EQUAL_UINT32(FAKE_TINYUSB_FIFO_BYTES, itf->write_available);
  (void)connect_peer();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, itf->write_count);
  TEST_ASSERT_TRUE(esy_proto_bin_session_is_idle(transport));
  uint8_t *tx = esy_proto_bin_session_get_tx_buffer(transport, NULL);
  TEST_ASSERT_NOT_NULL(tx);
  tx[0] = 0x55u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_session_commit_tx(transport, 1u, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, itf->write_count);
}

void test_reopen_discards_partial_parser_and_old_fifo_without_losing_new_frame(void) {
  esy_proto_bin_usb_channel_config_t config;
  uint8_t frame[64];
  size_t frame_len;
  const uint8_t payload[] = {0x31u, 0x32u};
  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  fake_tinyusb_cdc_itf_state_t *itf = connect_peer();
  build_frame(frame, sizeof(frame), ESY_PROTO_BIN_CMD_DATA, 1u, payload,
              sizeof(payload), &frame_len);
  fake_tinyusb_cdc_set_rx_data(0u, frame, 4u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_data_count);
  fake_tinyusb_cdc_set_rx_data(0u, frame + 4u, (uint32_t)frame_len - 4u);
  close_peer();
  TEST_ASSERT_EQUAL_UINT32(0u, itf->available);
  (void)connect_peer();
  fake_tinyusb_cdc_set_rx_data(0u, frame, (uint32_t)frame_len);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_data_count);
  TEST_ASSERT_EQUAL_MEMORY(payload, s_last_payload, sizeof(payload));
}

void test_physical_disconnect_clears_deferred_ack_and_ignores_closed_rx(void) {
  esy_proto_bin_usb_channel_config_t config;
  uint8_t frame[64];
  size_t frame_len;
  const uint8_t payload[] = {0x41u};
  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  fake_tinyusb_cdc_itf_state_t *itf = connect_peer();
  itf->write_available = 0u;
  build_frame(frame, sizeof(frame), ESY_PROTO_BIN_CMD_DATA, 2u, payload,
              sizeof(payload), &frame_len);
  fake_tinyusb_cdc_set_rx_data(0u, frame, (uint32_t)frame_len);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_data_count);
  TEST_ASSERT_EQUAL_UINT32(0u, itf->write_count);
  itf->connected = false;
  fake_tinyusb_cdc_set_rx_data(0u, frame, (uint32_t)frame_len);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(1u, s_data_count);
  TEST_ASSERT_EQUAL_UINT32(0u, itf->available);
  fake_esy_hal_system_set_tick(10000u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, s_event_count);
  (void)connect_peer();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(0u, itf->write_count);
  fake_tinyusb_cdc_set_rx_data(0u, frame, (uint32_t)frame_len);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT32(2u, s_data_count);
  TEST_ASSERT_EQUAL_UINT32(1u, itf->write_count);
}

void test_init_rejects_cdc_without_explicit_session_isolation(void) {
  esy_proto_bin_usb_channel_config_t config;
  esy_usb_cdc_config_t cdc_config;
  esy_usb_cdc_deinit(&s_ctx.cdc);
  esy_usb_cdc_default_config(&cdc_config);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_usb_cdc_init(&s_ctx.cdc, &cdc_config));
  fill_config(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
}

void test_usb_deinit_releases_copied_frame_without_fake_ack_or_fifo_drain(void) {
  esy_proto_bin_usb_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  fake_tinyusb_cdc_itf_state_t *itf = connect_peer();
  esy_proto_bin_session_t *session = esy_proto_bin_usb_channel_get_session(&s_ctx.channel);
  uint8_t *buffer = esy_proto_bin_session_get_tx_buffer(session, NULL);
  TEST_ASSERT_NOT_NULL(buffer);
  buffer[0] = 0x41u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(session, 1u, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT(1u, itf->write_count);
  uint32_t fifo_available = itf->write_available;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_deinit(&s_ctx.channel));
  TEST_ASSERT_NULL(esy_proto_bin_usb_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT(fifo_available, itf->write_available);
  TEST_ASSERT_EQUAL_UINT(0u, s_event_count);
  TEST_ASSERT_TRUE(esy_usb_cdc_connected(&s_ctx.cdc));
}

void test_usb_deinit_during_callback_retains_the_live_session(void) {
  esy_proto_bin_usb_channel_config_t config;
  fill_config(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  connect_peer();
  s_ctx.try_deinit_on_data = true;
  uint8_t frame[TEST_RX_WORK_BYTES];
  size_t length;
  const uint8_t payload = 0x41u;
  build_frame(frame, sizeof(frame), ESY_PROTO_BIN_CMD_DATA, 1u, &payload, 1u, &length);
  fake_tinyusb_cdc_set_rx_data(0u, frame, (uint32_t)length);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.callback_deinit_result);
  TEST_ASSERT_NOT_NULL(esy_proto_bin_usb_channel_get_session(&s_ctx.channel));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_deinit(&s_ctx.channel));
}

static void assert_usb_storage_rejected(const esy_proto_bin_usb_channel_config_t *config) {
  esy_proto_bin_usb_channel_t before = s_ctx.channel;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_proto_bin_usb_channel_init(&s_ctx.channel, config));
  TEST_ASSERT_EQUAL_MEMORY(&before, &s_ctx.channel, sizeof(before));
  fake_tinyusb_cdc_itf_state_t *itf = fake_tinyusb_cdc_get_itf(0u);
  TEST_ASSERT_EQUAL_UINT(0u, itf->task_count);
  TEST_ASSERT_EQUAL_UINT(0u, itf->write_count);
  TEST_ASSERT_EQUAL_UINT(0u, itf->read_count);
  memset(&s_ctx.channel, 0, sizeof(s_ctx.channel));
}

void test_usb_init_rejects_buffer_over_whole_channel_prefix_and_can_retry(void) {
  esy_proto_bin_usb_channel_config_t config;
  fill_config(&config);
  config.tx_payload_max = 1u;
  s_ctx.resources.tx_work_buf = s_ctx.channel.priv.bytes;
  s_ctx.resources.tx_work_buf_size = 1u + ESY_PROTO_BIN_OVERHEAD_BYTES;
  assert_usb_storage_rejected(&config);
  s_ctx.resources.tx_work_buf = s_ctx.tx_work_buf;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.tx_work_buf);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_init(&s_ctx.channel, &config));
  connect_peer();
  esy_proto_bin_session_t *session = esy_proto_bin_usb_channel_get_session(&s_ctx.channel);
  uint8_t *buffer = esy_proto_bin_session_get_tx_buffer(session, NULL);
  TEST_ASSERT_NOT_NULL(buffer);
  *buffer = 0x41u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_session_commit_tx(session, 1u, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_bin_usb_channel_poll(&s_ctx.channel));
  TEST_ASSERT_EQUAL_UINT(1u, fake_tinyusb_cdc_get_itf(0u)->write_count);
}

void test_usb_init_rejects_descriptor_aliases_and_buffer_overflow(void) {
  esy_proto_bin_usb_channel_config_t config;
  fill_config(&config);
  s_ctx.resources.tx_work_buf = (uint8_t *)&config;
  s_ctx.resources.tx_work_buf_size = sizeof(config);
  assert_usb_storage_rejected(&config);
  s_ctx.resources.tx_work_buf = (uint8_t *)&s_ctx.resources;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.resources);
  assert_usb_storage_rejected(&config);
  s_ctx.resources.tx_work_buf = s_ctx.tx_work_buf;
  s_ctx.resources.tx_work_buf_size = sizeof(s_ctx.tx_work_buf);
  memcpy(s_ctx.channel.priv.bytes, &config, sizeof(config));
  assert_usb_storage_rejected((const esy_proto_bin_usb_channel_config_t *)
      (const void *)s_ctx.channel.priv.bytes);
  memcpy(s_ctx.channel.priv.bytes, &s_ctx.resources, sizeof(s_ctx.resources));
  config.static_resources = (const esy_proto_bin_usb_channel_static_resources_t *)
      (const void *)s_ctx.channel.priv.bytes;
  assert_usb_storage_rejected(&config);
  config.static_resources = &s_ctx.resources;
  s_ctx.resources.tx_work_buf = (uint8_t *)(uintptr_t)(UINTPTR_MAX - 4u);
  assert_usb_storage_rejected(&config);
}
