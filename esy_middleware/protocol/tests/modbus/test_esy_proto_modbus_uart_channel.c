/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_modbus_uart_channel.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_system.h"
#include "fake_esy_hal_uart.h"
#include "unity.h"
#include <string.h>

typedef struct {
  esy_uart_t uart;
  esy_proto_modbus_uart_master_channel_storage_t master_storage;
  esy_proto_modbus_uart_slave_channel_storage_t slave_storage;
  ESY_PROTO_MODBUS_UART_MASTER_CHANNEL_STATIC_STORAGE(queues, 2u, 2u);
  esy_proto_modbus_uart_slave_channel_t *slave;
  uint32_t completions, requests;
  esy_proto_modbus_req_status_t status;
  bool reenter;
} modbus_core_test_ctx_t;
static modbus_core_test_ctx_t s_ctx;

static esy_proto_modbus_request_t request(void) {
  esy_proto_modbus_request_t req = {0};
  req.unit_id = 0x11u;
  req.func_code = ESY_PROTO_MODBUS_FUNC_READ_COILS;
  req.pdu[0] = req.func_code;
  req.pdu_len = 1u;
  return req;
}
static void complete(esy_proto_modbus_uart_master_channel_t *gw,
    esy_proto_modbus_req_status_t status, uint8_t unit, esy_proto_modbus_func_t func,
    const uint8_t *pdu, uint16_t length, void *arg) {
  (void)unit; (void)func; (void)pdu; (void)length;
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  ++s_ctx.completions;
  s_ctx.status = status;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_channel_poll(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_channel_destroy(gw));
  if (s_ctx.reenter) {
    s_ctx.reenter = false;
    esy_proto_modbus_request_t next = request();
    TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_submit(gw, &next));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_channel_abort_all(gw, true));
  }
}
static esy_err_t slave_request(esy_proto_modbus_uart_slave_channel_t *slave, uint8_t unit,
    esy_proto_modbus_func_t func, const uint8_t *pdu, uint16_t length,
    uint8_t *response, uint16_t *response_length, uint16_t maximum, void *arg) {
  (void)slave; (void)unit; (void)pdu; (void)length;
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_GREATER_THAN_UINT16(0u, maximum);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_slave_channel_destroy(s_ctx.slave));
  ++s_ctx.requests;
  response[0] = (uint8_t)func;
  *response_length = 1u;
  return ESY_OK;
}
static esy_proto_modbus_uart_master_channel_config_t master_config(void) {
  esy_proto_modbus_uart_master_channel_config_t cfg = esy_proto_modbus_uart_master_channel_default_cfg();
  cfg.uart = &s_ctx.uart;
  cfg.queue_depth = 2u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_complete = complete;
  cfg.user_ctx = &s_ctx;
  cfg.static_resources.slot_storage = s_ctx.queues.slot_storage;
  cfg.static_resources.slot_storage_count = 2u;
  cfg.static_resources.payload_storage = s_ctx.queues.payload_storage;
  cfg.static_resources.payload_storage_count = 2u;
  return cfg;
}
static esy_proto_modbus_uart_slave_channel_config_t slave_config(void) {
  esy_proto_modbus_uart_slave_channel_config_t cfg = esy_proto_modbus_uart_slave_channel_default_cfg();
  cfg.uart = &s_ctx.uart;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_request = slave_request;
  cfg.user_ctx = &s_ctx;
  return cfg;
}
static uint16_t frame(uint8_t *data, uint8_t unit) {
  esy_proto_modbus_frame_t f;
  const uint8_t pdu[] = {1u, 1u, 0xaau};
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_frame_init(&f, data, 32u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_build_rtu_frame(&f, unit, pdu, sizeof(pdu)));
  return (uint16_t)f.len;
}
void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_system_reset();
  fake_esy_hal_uart_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
}
void tearDown(void) {
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_total_delay_ms());
}

void test_modbus_core_master_copies_request_and_completes_on_owner_after_gap(void) {
  esy_proto_modbus_uart_master_channel_config_t cfg = master_config();
  esy_proto_modbus_uart_master_channel_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  esy_proto_modbus_request_t req = request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_submit(gw, &req));
  memset(&req, 0, sizeof(req));
  TEST_ASSERT_EQUAL(1u, esy_proto_modbus_uart_master_channel_get_queue_depth(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_poll(gw));
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  uint8_t data[32]; uint16_t length = frame(data, 0x11u);
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, data, length);
  fake_esy_hal_system_set_tick(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_poll(gw));
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  fake_esy_hal_system_set_tick(3u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_poll(gw));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_ctx.status);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_channel_poll(gw));
}

void test_modbus_core_abort_callback_submission_survives_and_shutdown_rejects_work(void) {
  esy_proto_modbus_uart_master_channel_config_t cfg = master_config();
  esy_proto_modbus_uart_master_channel_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  esy_proto_modbus_request_t req = request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_submit(gw, &req));
  s_ctx.reenter = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_abort_all(gw, true));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(1u, esy_proto_modbus_uart_master_channel_get_queue_depth(gw));
  fake_esy_hal_uart_slot_t *uart = fake_esy_hal_uart_find(&s_ctx.uart);
  uart->rx_stream_stop_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_uart_master_channel_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_channel_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_channel_abort_all(gw, true));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_uart_master_channel_poll(gw));
  uart->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_destroy(gw));
  TEST_ASSERT_EQUAL(2u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_ctx.status);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_destroy(gw));
}

void test_modbus_core_destroy_retains_dma_until_terminal_then_releases(void) {
  esy_proto_modbus_uart_master_channel_config_t cfg = master_config();
  esy_proto_modbus_uart_master_channel_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  esy_proto_modbus_request_t req = request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_poll(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_channel_destroy(gw));
  TEST_ASSERT_NOT_NULL(fake_esy_hal_uart_find(&s_ctx.uart)->last_dma_callback_arg);
  fake_esy_hal_uart_fail_last_dma(&s_ctx.uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_poll(gw));
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_ctx.status);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_destroy(gw));
}

void test_modbus_core_slave_observes_gap_across_wrap_and_retains_tx(void) {
  esy_proto_modbus_uart_slave_channel_config_t cfg = slave_config();
  TEST_ASSERT_EQUAL(1u, cfg.unit_id);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_create(&cfg, &s_ctx.slave_storage, &s_ctx.slave));
  uint8_t data[32]; uint16_t length = frame(data, cfg.unit_id);
  fake_esy_hal_system_set_tick(UINT32_MAX);
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, data, length);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  fake_esy_hal_system_set_tick(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_slave_channel_destroy(s_ctx.slave));
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_destroy(s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
}

void test_modbus_core_rx_budget_and_stream_loss_recovery_are_bounded(void) {
  esy_proto_modbus_uart_slave_channel_config_t cfg = slave_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_create(&cfg, &s_ctx.slave_storage, &s_ctx.slave));
  uint8_t data[400] = {0};
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, data, sizeof(data));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  fake_esy_hal_uart_slot_t *uart = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_EQUAL(sizeof(data) - (ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN + 16u), uart->rx_stream_used);
  (void)esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave);
  fake_esy_hal_system_set_tick(3u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(0u, s_ctx.requests);
  uart->rx_stream_callback(&s_ctx.uart, ESY_UART_RX_STREAM_EVENT_ERROR, uart->rx_stream_user_data);
  uart->rx_stream_stop_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  uart->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  fake_esy_hal_system_set_tick(6u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  uint8_t valid[32]; uint16_t length = frame(valid, cfg.unit_id);
  fake_esy_hal_uart_push_rx_stream(&s_ctx.uart, valid, length);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  fake_esy_hal_system_set_tick(8u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_poll(s_ctx.slave));
  TEST_ASSERT_EQUAL(1u, s_ctx.requests);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_destroy(s_ctx.slave));
}

void test_modbus_core_rejects_invalid_lifecycle_and_isr_calls(void) {
  esy_proto_modbus_uart_master_channel_config_t cfg = master_config();
  esy_proto_modbus_uart_slave_channel_config_t scfg = slave_config();
  esy_proto_modbus_uart_master_channel_t *gw = NULL, *duplicate = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_poll(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_slave_channel_poll(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_create(NULL, &s_ctx.master_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_slave_channel_create(NULL, &s_ctx.slave_storage, &s_ctx.slave));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &duplicate));
  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  esy_proto_modbus_request_t req = request();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_poll(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_abort_all(gw, true));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_slave_channel_create(&scfg, &s_ctx.slave_storage, &s_ctx.slave));
  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_destroy(NULL));
}

void test_modbus_core_rejects_invalid_queue_bounds_alignment_and_timeout_before_writes(void) {
  esy_proto_modbus_uart_master_channel_config_t cfg = master_config();
  esy_proto_modbus_uart_master_channel_t *gw = NULL;
  cfg.timeout_ms = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  cfg = master_config();
  cfg.queue_depth = (uint16_t)INT16_MAX + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  cfg = master_config();
  cfg.static_resources.slot_storage = (esy_prio_slot_t *)((uint8_t *)s_ctx.queues.slot_storage + 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  cfg = master_config();
  cfg.static_resources.payload_storage = (esy_proto_modbus_payload_t *)((uint8_t *)s_ctx.queues.payload_storage + 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_NULL(fake_esy_hal_uart_find(&s_ctx.uart)->rx_stream_storage);
  cfg = master_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_create(&cfg, &s_ctx.master_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_destroy(gw));
}

void test_channel_failed_rx_start_retains_only_channel_resources_until_retry(void) {
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->rx_stream_start_result = ESY_ERR_IO;
  slot->retain_failed_rx_start = true;
  slot->rx_stream_stop_result = ESY_ERR_TIMEOUT;
  esy_proto_modbus_uart_master_channel_config_t cfg = master_config();
  esy_proto_modbus_uart_master_channel_t *master = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_uart_master_channel_create(
      &cfg, &s_ctx.master_storage, &master));
  TEST_ASSERT_NOT_NULL(master);
  TEST_ASSERT_NOT_NULL(slot->rx_stream_callback);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_uart_master_channel_destroy(master));
  slot->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_destroy(master));
  TEST_ASSERT_NULL(slot->rx_stream_callback);
  esy_proto_modbus_uart_slave_channel_config_t slave_cfg = slave_config();
  slot->rx_stream_stop_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_uart_slave_channel_create(
      &slave_cfg, &s_ctx.slave_storage, &s_ctx.slave));
  TEST_ASSERT_NOT_NULL(s_ctx.slave);
  TEST_ASSERT_NOT_NULL(slot->rx_stream_callback);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_proto_modbus_uart_slave_channel_destroy(s_ctx.slave));
  slot->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_channel_destroy(s_ctx.slave));
  TEST_ASSERT_NULL(slot->rx_stream_callback);
}
