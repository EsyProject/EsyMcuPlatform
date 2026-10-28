/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_modbus_uart_worker.h"

#include <string.h>

#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_system.h"
#include "fake_esy_hal_uart.h"
#include "unity.h"

static esy_uart_t s_uart;
static uint32_t s_slave_request_count;
static esy_proto_modbus_uart_master_worker_storage_t s_master_storage;
static esy_proto_modbus_uart_slave_worker_storage_t s_slave_storage;
static esy_prio_slot_t s_master_slots[2];
static esy_proto_modbus_payload_t s_master_payloads[2];
static _Alignas(ESY_OS_TASK_STACK_ALIGN) uint8_t s_master_task_stack[1024];
static _Alignas(ESY_OS_TASK_STACK_ALIGN) uint8_t s_slave_task_stack[1024];
static esy_os_task_static_storage_t s_master_task_control_storage;
static esy_os_task_static_storage_t s_slave_task_control_storage;
static uint32_t s_master_complete_count;
static esy_proto_modbus_uart_master_worker_t *s_complete_worker;
static esy_proto_modbus_req_status_t s_master_status;
static uint8_t s_master_unit_id;
static esy_proto_modbus_func_t s_master_func_code;
static uint8_t s_master_resp_pdu[8];
static uint16_t s_master_resp_pdu_len;
static void *s_master_user_ctx;
static esy_proto_modbus_req_status_t s_master_statuses[4];
static uint8_t s_master_unit_ids[4];
static esy_proto_modbus_func_t s_master_func_codes[4];
static uint16_t s_master_resp_pdu_lens[4];

extern void esy_proto_modbus_uart_master_worker_test_iteration(
    esy_proto_modbus_uart_master_worker_t *gw);
extern void esy_proto_modbus_uart_slave_worker_test_iteration(
    esy_proto_modbus_uart_slave_worker_t *gw);

static esy_err_t fake_slave_request(esy_proto_modbus_uart_slave_worker_t *slave,
                                    uint8_t unit_id,
                                    esy_proto_modbus_func_t func_code,
                                    const uint8_t *req_pdu,
                                    uint16_t req_pdu_len,
                                    uint8_t *resp_pdu_buf,
                                    uint16_t *resp_pdu_len,
                                    uint16_t resp_pdu_buf_max,
                                    void *user_ctx) {
  (void)slave;
  (void)unit_id;
  (void)req_pdu;
  (void)req_pdu_len;
  (void)user_ctx;

  TEST_ASSERT_NOT_NULL(resp_pdu_buf);
  TEST_ASSERT_NOT_NULL(resp_pdu_len);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT16(1u, resp_pdu_buf_max);

  resp_pdu_buf[0] = (uint8_t)func_code;
  *resp_pdu_len = 1u;
  s_slave_request_count++;
  return ESY_OK;
}

static esy_proto_modbus_request_t make_master_request(void) {
  esy_proto_modbus_request_t req;

  memset(&req, 0, sizeof(req));
  req.unit_id = 0x11u;
  req.func_code = ESY_PROTO_MODBUS_FUNC_READ_COILS;
  req.pdu[0] = (uint8_t)ESY_PROTO_MODBUS_FUNC_READ_COILS;
  req.pdu[1] = 0x00u;
  req.pdu_len = 2u;
  req.priority = ESY_PROTO_MODBUS_PRIORITY_MEDIUM;
  return req;
}

static void *make_misaligned_stack_buffer(uint8_t *storage) {
  if (((uintptr_t)storage % ESY_OS_TASK_STACK_ALIGN) == 0u) {
    return &storage[1];
  }

  return storage;
}

static void master_complete_callback(
    esy_proto_modbus_uart_master_worker_t *channel,
    esy_proto_modbus_req_status_t status,
    uint8_t unit_id,
    esy_proto_modbus_func_t func_code,
    const uint8_t *resp_pdu,
    uint16_t resp_pdu_len,
    void *user_ctx) {
  s_complete_worker = channel;

  s_master_complete_count++;
  s_master_status = status;
  s_master_unit_id = unit_id;
  s_master_func_code = func_code;
  s_master_resp_pdu_len = resp_pdu_len;
  s_master_user_ctx = user_ctx;

  if (s_master_complete_count <=
      (sizeof(s_master_statuses) / sizeof(s_master_statuses[0]))) {
    uint32_t idx = s_master_complete_count - 1u;
    s_master_statuses[idx] = status;
    s_master_unit_ids[idx] = unit_id;
    s_master_func_codes[idx] = func_code;
    s_master_resp_pdu_lens[idx] = resp_pdu_len;
  }

  if (resp_pdu != NULL && resp_pdu_len <= sizeof(s_master_resp_pdu)) {
    memcpy(s_master_resp_pdu, resp_pdu, resp_pdu_len);
  }
}

static void attach_master_storage(
    esy_proto_modbus_uart_master_worker_config_t *cfg) {
  cfg->static_resources.slot_storage = s_master_slots;
  cfg->static_resources.slot_storage_count = 2u;
  cfg->static_resources.payload_storage = s_master_payloads;
  cfg->static_resources.payload_storage_count = 2u;
  cfg->static_resources.task_stack_buffer = s_master_task_stack;
  cfg->static_resources.task_stack_buffer_size = sizeof(s_master_task_stack);
  cfg->static_resources.task_control_storage = &s_master_task_control_storage;
}

static void attach_slave_storage(
    esy_proto_modbus_uart_slave_worker_config_t *cfg) {
  cfg->static_resources.task_stack_buffer = s_slave_task_stack;
  cfg->static_resources.task_stack_buffer_size = sizeof(s_slave_task_stack);
  cfg->static_resources.task_control_storage = &s_slave_task_control_storage;
}

static void build_rtu_frame(uint8_t *buf,
                            uint16_t *out_len,
                            uint8_t unit_id,
                            const uint8_t *pdu,
                            uint16_t pdu_len) {
  esy_proto_modbus_frame_t frame;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_frame_init(&frame, buf, 64u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_proto_modbus_build_rtu_frame(&frame, unit_id, pdu, pdu_len));
  *out_len = (uint16_t)frame.len;
}

static void feed_master_frame_after_idle_gap(
    esy_proto_modbus_uart_master_worker_t *gw,
    const uint8_t *frame,
    uint16_t frame_len,
    uint32_t rx_tick_ms,
    uint32_t idle_tick_ms) {
  fake_esy_hal_uart_push_rx_stream(&s_uart, frame, frame_len);
  fake_esy_hal_system_set_tick(rx_tick_ms);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_system_set_tick(idle_tick_ms);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
}

static void feed_slave_frame_after_idle_gap(
    esy_proto_modbus_uart_slave_worker_t *gw,
    const uint8_t *frame,
    uint16_t frame_len,
    uint32_t rx_tick_ms,
    uint32_t idle_tick_ms) {
  fake_esy_hal_uart_push_rx_stream(&s_uart, frame, frame_len);
  fake_esy_hal_system_set_tick(rx_tick_ms);
  esy_proto_modbus_uart_slave_worker_test_iteration(gw);
  fake_esy_hal_system_set_tick(idle_tick_ms);
  esy_proto_modbus_uart_slave_worker_test_iteration(gw);
}

extern void esy_proto_modbus_uart_slave_worker_test_control(esy_proto_modbus_uart_slave_worker_t *gw);
extern void esy_proto_modbus_uart_master_worker_test_control(esy_proto_modbus_uart_master_worker_t *gw);

/* Schedule one worker slice while a public lifecycle/command operation waits. */
static bool s_scheduling;
static void schedule_workers(uint32_t ms) {
  (void)ms;
  if (s_scheduling) return;
  s_scheduling = true;
  esy_os_task_t *caller = fake_esy_os_task_get_current();
  fake_esy_os_task_entry_t *table = fake_esy_os_task_get_table();
  for (uint32_t i = 0; i < fake_esy_os_task_get_table_size(); ++i) {
    fake_esy_os_task_entry_t *entry = &table[i];
    if (!entry->used || entry->deleted || entry->parked) continue;
    fake_esy_os_task_set_current(entry->task_handle);
    if (strstr(entry->name, "master")) esy_proto_modbus_uart_master_worker_test_control(entry->arg);
    else esy_proto_modbus_uart_slave_worker_test_control(entry->arg);
  }
  fake_esy_os_task_set_current(caller);
  s_scheduling = false;
}

void setUp(void) {
  s_scheduling = false;
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_os_task_set_delay_hook(schedule_workers);
  fake_esy_hal_system_reset();
  fake_esy_hal_uart_reset();
  memset(&s_master_storage, 0, sizeof(s_master_storage));
  memset(&s_slave_storage, 0, sizeof(s_slave_storage));
  memset(&s_master_task_control_storage, 0, sizeof(s_master_task_control_storage));
  memset(&s_slave_task_control_storage, 0, sizeof(s_slave_task_control_storage));
  memset(s_master_resp_pdu, 0, sizeof(s_master_resp_pdu));
  memset(s_master_statuses, 0, sizeof(s_master_statuses));
  memset(s_master_unit_ids, 0, sizeof(s_master_unit_ids));
  memset(s_master_func_codes, 0, sizeof(s_master_func_codes));
  memset(s_master_resp_pdu_lens, 0, sizeof(s_master_resp_pdu_lens));
  s_uart = (esy_uart_t){0};
  s_slave_request_count = 0u;
  s_master_complete_count = 0u;
  s_complete_worker = NULL;
  s_master_status = ESY_PROTO_MODBUS_REQ_STATUS_OK;
  s_master_unit_id = 0u;
  s_master_func_code = (esy_proto_modbus_func_t)0u;
  s_master_resp_pdu_len = 0u;
  s_master_user_ctx = NULL;
}

void tearDown(void) {}

void test_modbus_master_default_cfg_is_ready_to_customize(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();

  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_FRAME_RTU, cfg.frame_type);
  TEST_ASSERT_NULL(cfg.uart);
  TEST_ASSERT_EQUAL_UINT32(100u, cfg.timeout_ms);
  TEST_ASSERT_EQUAL_UINT8(0u, cfg.retry_max);
  TEST_ASSERT_EQUAL_UINT32(10u, cfg.rtu_frame_timeout_ms);
  TEST_ASSERT_EQUAL_UINT16(0u, cfg.queue_depth);
  TEST_ASSERT_NULL(cfg.static_resources.task_stack_buffer);
  TEST_ASSERT_EQUAL_UINT32(0u, cfg.static_resources.task_stack_buffer_size);
  TEST_ASSERT_NULL(cfg.static_resources.task_control_storage);
  TEST_ASSERT_EQUAL_STRING("esy_proto_modbus_uart_master_worker", cfg.task_name);
}

void test_modbus_master_static_storage_macro_declares_bindable_resources(void) {
  ESY_PROTO_MODBUS_UART_MASTER_WORKER_STATIC_STORAGE(master_storage, 2u, 2u, 1024u);
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();

  cfg.static_resources.slot_storage = master_storage.slot_storage;
  cfg.static_resources.slot_storage_count =
      sizeof(master_storage.slot_storage) /
      sizeof(master_storage.slot_storage[0]);
  cfg.static_resources.payload_storage = master_storage.payload_storage;
  cfg.static_resources.payload_storage_count =
      sizeof(master_storage.payload_storage) /
      sizeof(master_storage.payload_storage[0]);
  cfg.static_resources.task_stack_buffer = master_storage.task_stack_buffer;
  cfg.static_resources.task_stack_buffer_size =
      sizeof(master_storage.task_stack_buffer);
  cfg.static_resources.task_control_storage =
      &master_storage.task_control_storage;

  TEST_ASSERT_EQUAL_UINT16(2u, cfg.static_resources.slot_storage_count);
  TEST_ASSERT_EQUAL_UINT16(2u, cfg.static_resources.payload_storage_count);
  TEST_ASSERT_NOT_NULL(cfg.static_resources.task_stack_buffer);
  TEST_ASSERT_NOT_NULL(cfg.static_resources.task_control_storage);
}

void test_modbus_master_create_rejects_invalid_config(void) {
  esy_proto_modbus_uart_master_worker_t *gw =
      (esy_proto_modbus_uart_master_worker_t *)0x1;
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  cfg.static_resources.slot_storage = s_master_slots;
  cfg.static_resources.slot_storage_count = 2u;
  cfg.static_resources.payload_storage = s_master_payloads;
  cfg.static_resources.payload_storage_count = 2u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_master_worker_create(
                              NULL, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, NULL));

  cfg.uart = &s_uart;
  cfg.queue_depth = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_NULL(gw);
}

void test_modbus_master_create_rejects_misaligned_task_stack(void) {
  uint8_t raw_stack[1024u + ESY_OS_TASK_STACK_ALIGN];
  esy_proto_modbus_uart_master_worker_t *gw =
      (esy_proto_modbus_uart_master_worker_t *)0x1;
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  attach_master_storage(&cfg);
  cfg.static_resources.task_stack_buffer =
      make_misaligned_stack_buffer(raw_stack);
  cfg.static_resources.task_stack_buffer_size = 1024u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
}

void test_modbus_master_create_submit_abort_and_destroy(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  esy_proto_modbus_request_t req = make_master_request();

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  attach_master_storage(&cfg);
  cfg.task_name = "modbus_master";

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_NOT_NULL(gw);
  TEST_ASSERT_TRUE(esy_proto_modbus_uart_master_worker_is_initialized(gw));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_UINT16(0u,
                           esy_proto_modbus_uart_master_worker_get_queue_depth(gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req));
  TEST_ASSERT_EQUAL_UINT16(1u,
                           esy_proto_modbus_uart_master_worker_get_queue_depth(gw));

  req.on_complete = (esy_proto_modbus_request_complete_cb)0x1;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_abort_all(gw, true));

  esy_proto_modbus_uart_master_worker_destroy(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_delete_count());
}

void test_modbus_master_create_uses_caller_owned_storage_without_malloc(void) {
  esy_proto_modbus_uart_master_worker_storage_t storage = {0};
  esy_prio_slot_t slots[2];
  esy_proto_modbus_payload_t payloads[2];
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.static_resources.slot_storage = slots;
  cfg.static_resources.slot_storage_count = 2u;
  cfg.static_resources.payload_storage = payloads;
  cfg.static_resources.payload_storage_count = 2u;
  cfg.static_resources.task_stack_buffer = s_master_task_stack;
  cfg.static_resources.task_stack_buffer_size = sizeof(s_master_task_stack);
  cfg.static_resources.task_control_storage = &s_master_task_control_storage;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &storage, &gw));
  TEST_ASSERT_NOT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);
}

void test_modbus_master_create_rejects_small_queue_storage(void) {
  esy_proto_modbus_uart_master_worker_storage_t storage = {0};
  esy_prio_slot_t slots[1];
  esy_proto_modbus_payload_t payloads[2];
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.static_resources.slot_storage = slots;
  cfg.static_resources.slot_storage_count = 1u;
  cfg.static_resources.payload_storage = payloads;
  cfg.static_resources.payload_storage_count = 2u;
  cfg.static_resources.task_stack_buffer = s_master_task_stack;
  cfg.static_resources.task_stack_buffer_size = sizeof(s_master_task_stack);
  cfg.static_resources.task_control_storage = &s_master_task_control_storage;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);
}

void test_modbus_master_create_rejects_missing_queue_storage(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw =
      (esy_proto_modbus_uart_master_worker_t *)0x1;

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);
}

void test_modbus_master_iteration_completes_response_from_uart_idle_gap(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  esy_proto_modbus_request_t req = make_master_request();
  fake_esy_hal_uart_slot_t *slot = NULL;
  uint8_t response[64];
  uint16_t response_len = 0u;
  const uint8_t response_pdu[] = {
      ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x01u, 0xAAu};
  int user_ctx;

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_complete = master_complete_callback;
  cfg.user_ctx = &user_ctx;
  attach_master_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req));

  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);

  slot = fake_esy_hal_uart_find(&s_uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_NOT_NULL(slot->last_dma_src);
  TEST_ASSERT_GREATER_THAN_UINT32(0u, slot->last_dma_len);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP,
                    esy_proto_modbus_uart_master_worker_get_state(gw));

  fake_esy_hal_uart_complete_last_dma(&s_uart);
  build_rtu_frame(response, &response_len, 0x11u, response_pdu,
                  sizeof(response_pdu));
  fake_esy_hal_uart_push_rx_stream(&s_uart, response, response_len);

  fake_esy_hal_system_set_tick(11u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, s_master_complete_count);

  fake_esy_hal_system_set_tick(13u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_master_status);
  TEST_ASSERT_EQUAL_UINT8(0x11u, s_master_unit_id);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_FUNC_READ_COILS, s_master_func_code);
  TEST_ASSERT_EQUAL_UINT16(sizeof(response_pdu), s_master_resp_pdu_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(response_pdu, s_master_resp_pdu,
                                sizeof(response_pdu));
  TEST_ASSERT_EQUAL_PTR(&user_ctx, s_master_user_ctx);
  TEST_ASSERT_EQUAL_UINT16(0u,
                           esy_proto_modbus_uart_master_worker_get_queue_depth(gw));
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_IDLE,
                    esy_proto_modbus_uart_master_worker_get_state(gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_master_iteration_accepts_exception_response(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  esy_proto_modbus_request_t req = make_master_request();
  uint8_t response[64];
  uint16_t response_len = 0u;
  const uint8_t exception_pdu[] = {0x81u, 0x02u};

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_complete = master_complete_callback;
  attach_master_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req));

  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_complete_last_dma(&s_uart);

  build_rtu_frame(response, &response_len, 0x11u, exception_pdu,
                  sizeof(exception_pdu));
  feed_master_frame_after_idle_gap(gw, response, response_len, 11u, 13u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_EXCEPTION, s_master_status);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_FUNC_READ_COILS, s_master_func_code);
  TEST_ASSERT_EQUAL_UINT16(sizeof(exception_pdu), s_master_resp_pdu_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(exception_pdu, s_master_resp_pdu,
                                sizeof(exception_pdu));
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_IDLE,
                    esy_proto_modbus_uart_master_worker_get_state(gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_master_iteration_reports_crc_bad_frame(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  esy_proto_modbus_request_t req = make_master_request();
  uint8_t response[64];
  uint16_t response_len = 0u;
  const uint8_t response_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x01u};

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_complete = master_complete_callback;
  attach_master_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req));

  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_complete_last_dma(&s_uart);

  build_rtu_frame(response, &response_len, 0x11u, response_pdu,
                  sizeof(response_pdu));
  response[response_len - 1u] ^= 0x55u;
  feed_master_frame_after_idle_gap(gw, response, response_len, 11u, 13u);

  TEST_ASSERT_EQUAL_UINT32(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_BAD_FRAME, s_master_status);
  TEST_ASSERT_EQUAL_UINT16(0u, s_master_resp_pdu_len);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_IDLE,
                    esy_proto_modbus_uart_master_worker_get_state(gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_master_iteration_ignores_wrong_unit_then_times_out(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  esy_proto_modbus_request_t req = make_master_request();
  uint8_t response[64];
  uint16_t response_len = 0u;
  const uint8_t response_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x01u};

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.timeout_ms = 4u;
  cfg.on_complete = master_complete_callback;
  attach_master_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req));

  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_complete_last_dma(&s_uart);

  build_rtu_frame(response, &response_len, 0x22u, response_pdu,
                  sizeof(response_pdu));
  feed_master_frame_after_idle_gap(gw, response, response_len, 11u, 13u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP,
                    esy_proto_modbus_uart_master_worker_get_state(gw));

  fake_esy_hal_system_set_tick(15u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT, s_master_status);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_master_iteration_retries_timeout_before_failing(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  esy_proto_modbus_request_t req = make_master_request();
  fake_esy_hal_uart_slot_t *slot = NULL;

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.timeout_ms = 3u;
  cfg.retry_max = 1u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_complete = master_complete_callback;
  attach_master_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req));

  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  slot = fake_esy_hal_uart_find(&s_uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);

  fake_esy_hal_system_set_tick(13u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(2u, slot->send_dma_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_master_complete_count);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);

  fake_esy_hal_system_set_tick(16u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT, s_master_status);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_IDLE,
                    esy_proto_modbus_uart_master_worker_get_state(gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_master_iteration_starts_next_request_after_response(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg =
      esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  esy_proto_modbus_request_t req_a = make_master_request();
  esy_proto_modbus_request_t req_b = make_master_request();
  fake_esy_hal_uart_slot_t *slot = NULL;
  uint8_t response[64];
  uint16_t response_len = 0u;
  const uint8_t response_a_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x01u};
  const uint8_t response_b_pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x01u};

  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_complete = master_complete_callback;
  attach_master_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  req_b.unit_id = 0x12u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_create(
                              &cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req_a));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_submit(gw, &req_b));

  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  slot = fake_esy_hal_uart_find(&s_uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);

  build_rtu_frame(response, &response_len, 0x11u, response_a_pdu,
                  sizeof(response_a_pdu));
  feed_master_frame_after_idle_gap(gw, response, response_len, 11u, 13u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL_UINT8(0x11u, s_master_unit_ids[0]);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  fake_esy_hal_system_set_tick(14u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(2u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP,
                    esy_proto_modbus_uart_master_worker_get_state(gw));
  fake_esy_hal_uart_complete_last_dma(&s_uart);

  build_rtu_frame(response, &response_len, 0x12u, response_b_pdu,
                  sizeof(response_b_pdu));
  feed_master_frame_after_idle_gap(gw, response, response_len, 14u, 16u);
  TEST_ASSERT_EQUAL_UINT32(2u, s_master_complete_count);
  TEST_ASSERT_EQUAL_UINT8(0x12u, s_master_unit_ids[1]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_master_statuses[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_master_statuses[1]);
  TEST_ASSERT_EQUAL_UINT16(0u,
                           esy_proto_modbus_uart_master_worker_get_queue_depth(gw));
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_MASTER_STATE_IDLE,
                    esy_proto_modbus_uart_master_worker_get_state(gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_slave_default_cfg_is_ready_to_customize(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();

  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_FRAME_RTU, cfg.frame_type);
  TEST_ASSERT_NULL(cfg.uart);
  TEST_ASSERT_EQUAL_UINT8(1u, cfg.unit_id);
  TEST_ASSERT_EQUAL_UINT32(10u, cfg.rtu_frame_timeout_ms);
  TEST_ASSERT_NULL(cfg.on_request);
  TEST_ASSERT_NULL(cfg.static_resources.task_stack_buffer);
  TEST_ASSERT_EQUAL_UINT32(0u, cfg.static_resources.task_stack_buffer_size);
  TEST_ASSERT_NULL(cfg.static_resources.task_control_storage);
  TEST_ASSERT_EQUAL_STRING("esy_proto_modbus_uart_slave_worker", cfg.task_name);
}

void test_modbus_slave_static_storage_macro_declares_bindable_resources(void) {
  ESY_PROTO_MODBUS_UART_SLAVE_WORKER_STATIC_STORAGE(slave_storage, 1024u);
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();

  cfg.static_resources.task_stack_buffer = slave_storage.task_stack_buffer;
  cfg.static_resources.task_stack_buffer_size =
      sizeof(slave_storage.task_stack_buffer);
  cfg.static_resources.task_control_storage =
      &slave_storage.task_control_storage;

  TEST_ASSERT_EQUAL_UINT32(1024u, cfg.static_resources.task_stack_buffer_size);
  TEST_ASSERT_NOT_NULL(cfg.static_resources.task_stack_buffer);
  TEST_ASSERT_NOT_NULL(cfg.static_resources.task_control_storage);
}

void test_modbus_slave_create_get_addr_and_destroy(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();
  esy_proto_modbus_uart_slave_worker_t *gw = NULL;

  cfg.uart = &s_uart;
  cfg.unit_id = 0x22u;
  cfg.on_request = fake_slave_request;
  cfg.task_name = "modbus_slave";
  attach_slave_storage(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &s_slave_storage, &gw));
  TEST_ASSERT_NOT_NULL(gw);
  TEST_ASSERT_TRUE(esy_proto_modbus_uart_slave_worker_is_initialized(gw));
  TEST_ASSERT_EQUAL_UINT8(0x22u, esy_proto_modbus_uart_slave_worker_get_addr(gw));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_UINT32(0u, s_slave_request_count);

  esy_proto_modbus_uart_slave_worker_destroy(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_delete_count());
}

void test_modbus_slave_create_rejects_misaligned_task_stack(void) {
  uint8_t raw_stack[1024u + ESY_OS_TASK_STACK_ALIGN];
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();
  esy_proto_modbus_uart_slave_worker_t *gw =
      (esy_proto_modbus_uart_slave_worker_t *)0x1;

  cfg.uart = &s_uart;
  cfg.on_request = fake_slave_request;
  attach_slave_storage(&cfg);
  cfg.static_resources.task_stack_buffer =
      make_misaligned_stack_buffer(raw_stack);
  cfg.static_resources.task_stack_buffer_size = 1024u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &s_slave_storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
}

void test_modbus_slave_create_uses_caller_owned_storage_without_malloc(void) {
  esy_proto_modbus_uart_slave_worker_storage_t storage = {0};
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();
  esy_proto_modbus_uart_slave_worker_t *gw = NULL;

  cfg.uart = &s_uart;
  cfg.unit_id = 0x22u;
  cfg.on_request = fake_slave_request;
  attach_slave_storage(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &storage, &gw));
  TEST_ASSERT_NOT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT8(0x22u, esy_proto_modbus_uart_slave_worker_get_addr(gw));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_slave_worker_destroy(gw));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);
}

void test_modbus_slave_create_rejects_invalid_config(void) {
  esy_proto_modbus_uart_slave_worker_t *gw =
      (esy_proto_modbus_uart_slave_worker_t *)0x1;
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_slave_worker_create(
                              NULL, &s_slave_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &s_slave_storage, NULL));

  cfg.uart = &s_uart;
  cfg.on_request = NULL;
  attach_slave_storage(&cfg);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &s_slave_storage, &gw));
  TEST_ASSERT_NULL(gw);
}

void test_modbus_slave_iteration_dispatches_frame_after_idle_gap(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();
  esy_proto_modbus_uart_slave_worker_t *gw = NULL;
  uint8_t frame[64];
  uint16_t frame_len = 0u;
  const uint8_t pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x00u};

  cfg.uart = &s_uart;
  cfg.unit_id = 0x22u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_request = fake_slave_request;
  attach_slave_storage(&cfg);

  build_rtu_frame(frame, &frame_len, 0x22u, pdu, sizeof(pdu));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &s_slave_storage, &gw));

  fake_esy_hal_uart_push_rx_stream(&s_uart, frame, frame_len);
  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_uart_slave_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, s_slave_request_count);

  fake_esy_hal_system_set_tick(12u);
  esy_proto_modbus_uart_slave_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, s_slave_request_count);
  TEST_ASSERT_EQUAL_UINT32(2u, fake_esy_os_task_get_total_delay_ms());

  esy_proto_modbus_uart_slave_worker_destroy(gw);
}

void test_modbus_slave_iteration_ignores_crc_bad_frame(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();
  esy_proto_modbus_uart_slave_worker_t *gw = NULL;
  fake_esy_hal_uart_slot_t *slot = NULL;
  uint8_t frame[64];
  uint16_t frame_len = 0u;
  const uint8_t pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x00u};

  cfg.uart = &s_uart;
  cfg.unit_id = 0x22u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_request = fake_slave_request;
  attach_slave_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);

  build_rtu_frame(frame, &frame_len, 0x22u, pdu, sizeof(pdu));
  frame[frame_len - 1u] ^= 0x5Au;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &s_slave_storage, &gw));
  feed_slave_frame_after_idle_gap(gw, frame, frame_len, 10u, 12u);

  slot = fake_esy_hal_uart_find(&s_uart);
  TEST_ASSERT_EQUAL_UINT32(0u, s_slave_request_count);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT32(0u, slot->send_dma_count);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_modbus_uart_slave_worker_destroy(gw));
}

void test_modbus_slave_iteration_ignores_wrong_unit_id(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();
  esy_proto_modbus_uart_slave_worker_t *gw = NULL;
  fake_esy_hal_uart_slot_t *slot = NULL;
  uint8_t frame[64];
  uint16_t frame_len = 0u;
  const uint8_t pdu[] = {ESY_PROTO_MODBUS_FUNC_READ_COILS, 0x00u};

  cfg.uart = &s_uart;
  cfg.unit_id = 0x22u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_request = fake_slave_request;
  attach_slave_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);

  build_rtu_frame(frame, &frame_len, 0x33u, pdu, sizeof(pdu));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &s_slave_storage, &gw));
  feed_slave_frame_after_idle_gap(gw, frame, frame_len, 10u, 12u);

  slot = fake_esy_hal_uart_find(&s_uart);
  TEST_ASSERT_EQUAL_UINT32(0u, s_slave_request_count);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT32(0u, slot->send_dma_count);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_modbus_uart_slave_worker_destroy(gw));
}

void test_modbus_slave_iteration_suppresses_broadcast_response(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg =
      esy_proto_modbus_uart_slave_worker_default_cfg();
  esy_proto_modbus_uart_slave_worker_t *gw = NULL;
  fake_esy_hal_uart_slot_t *slot = NULL;
  uint8_t frame[64];
  uint16_t frame_len = 0u;
  const uint8_t pdu[] = {ESY_PROTO_MODBUS_FUNC_WRITE_SINGLE_COIL, 0x00u};

  cfg.uart = &s_uart;
  cfg.unit_id = 0x22u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_request = fake_slave_request;
  attach_slave_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);

  build_rtu_frame(frame, &frame_len, 0x00u, pdu, sizeof(pdu));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_proto_modbus_uart_slave_worker_create(
                              &cfg, &s_slave_storage, &gw));
  feed_slave_frame_after_idle_gap(gw, frame, frame_len, 10u, 12u);

  slot = fake_esy_hal_uart_find(&s_uart);
  TEST_ASSERT_EQUAL_UINT32(1u, s_slave_request_count);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT32(0u, slot->send_dma_count);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_proto_modbus_uart_slave_worker_destroy(gw));
}

void test_modbus_invalid_frame_and_gap_do_not_poison_first_use_storage(void) {
  esy_proto_modbus_uart_master_worker_config_t master = esy_proto_modbus_uart_master_worker_default_cfg();
  esy_proto_modbus_uart_slave_worker_config_t slave = esy_proto_modbus_uart_slave_worker_default_cfg();
  master.uart = slave.uart = &s_uart;
  master.queue_depth = 2u;
  slave.on_request = fake_slave_request;
  attach_master_storage(&master);
  attach_slave_storage(&slave);
  esy_proto_modbus_uart_master_worker_t *mgw = NULL;
  esy_proto_modbus_uart_slave_worker_t *sgw = NULL;
  master.rtu_frame_timeout_ms = slave.rtu_frame_timeout_ms = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_worker_create(&master, &s_master_storage, &mgw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_slave_worker_create(&slave, &s_slave_storage, &sgw));
  master.rtu_frame_timeout_ms = slave.rtu_frame_timeout_ms = 0u;
  master.frame_type = slave.frame_type = ESY_PROTO_MODBUS_FRAME_TCP;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_master_worker_create(&master, &s_master_storage, &mgw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_proto_modbus_uart_slave_worker_create(&slave, &s_slave_storage, &sgw));
  master.frame_type = slave.frame_type = ESY_PROTO_MODBUS_FRAME_RTU;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&master, &s_master_storage, &mgw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(mgw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_create(&slave, &s_slave_storage, &sgw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_destroy(sgw));
}

void test_modbus_overflow_discards_valid_tail_until_idle_gap_and_recovers(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg = esy_proto_modbus_uart_slave_worker_default_cfg();
  cfg.uart = &s_uart;
  cfg.unit_id = 0x22u;
  cfg.on_request = fake_slave_request;
  cfg.rtu_frame_timeout_ms = 2u;
  attach_slave_storage(&cfg);
  esy_proto_modbus_uart_slave_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_create(&cfg, &s_slave_storage, &gw));
  uint8_t frame[64], stream[ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN + 80u];
  uint16_t length;
  build_rtu_frame(frame, &length, 0x22u, (uint8_t[]){ESY_PROTO_MODBUS_FUNC_READ_COILS, 0}, 2u);
  size_t prefix = ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN + 17u;
  memset(stream, 0x55, prefix);
  memcpy(stream + prefix, frame, length);
  fake_esy_hal_uart_push_rx_stream(&s_uart, stream, prefix + length);
  fake_esy_hal_system_set_tick(UINT32_MAX - 1u);
  esy_proto_modbus_uart_slave_worker_test_iteration(gw);
  esy_proto_modbus_uart_slave_worker_test_iteration(gw);
  fake_esy_hal_system_set_tick(0u);
  esy_proto_modbus_uart_slave_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(0u, s_slave_request_count);
  feed_slave_frame_after_idle_gap(gw, frame, length, 2u, 4u);
  TEST_ASSERT_EQUAL(1u, s_slave_request_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_destroy(gw));
}

static esy_proto_modbus_uart_master_worker_t *s_command_worker;
static esy_err_t s_nested_submit_result;
static void submit_during_pending_command(uint32_t ms) {
  esy_proto_modbus_request_t req = make_master_request();
  s_nested_submit_result = esy_proto_modbus_uart_master_worker_submit(s_command_worker, &req);
  schedule_workers(ms);
}

void test_modbus_commands_are_copied_serialized_and_timeout_without_side_effects(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = esy_proto_modbus_uart_master_worker_default_cfg();
  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  attach_master_storage(&cfg);
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  esy_proto_modbus_request_t req = make_master_request();
  fake_esy_os_task_set_delay_hook(NULL);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  TEST_ASSERT_EQUAL(0u, esy_proto_modbus_uart_master_worker_get_queue_depth(gw));
  s_command_worker = gw;
  fake_esy_os_task_set_delay_hook(submit_during_pending_command);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_nested_submit_result);
  TEST_ASSERT_EQUAL(1u, esy_proto_modbus_uart_master_worker_get_queue_depth(gw));
  memset(&req, 0, sizeof(req));
  fake_esy_os_task_set_delay_hook(schedule_workers);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_abort_all(gw, false));
  TEST_ASSERT_EQUAL(0u, esy_proto_modbus_uart_master_worker_get_queue_depth(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_dma_error_shutdown_rejects_submit_until_park_and_allows_recreate(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = esy_proto_modbus_uart_master_worker_default_cfg();
  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  attach_master_storage(&cfg);
  esy_proto_modbus_uart_master_worker_t *gw = NULL, *duplicate = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &duplicate));
  esy_proto_modbus_request_t req = make_master_request();
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_fail_last_dma(&s_uart);
  fake_esy_os_task_set_delay_hook(NULL);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_worker_abort_all(gw, true));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
  fake_esy_os_task_set_delay_hook(schedule_workers);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_FALSE(esy_proto_modbus_uart_master_worker_is_initialized(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

static esy_os_task_t *s_expected_owner;
static bool s_expect_shutdown;
static void owner_completion(esy_proto_modbus_uart_master_worker_t *gw,
    esy_proto_modbus_req_status_t status, uint8_t unit, esy_proto_modbus_func_t func,
    const uint8_t *pdu, uint16_t len, void *ctx) {
  (void)status; (void)unit; (void)func; (void)pdu; (void)len; (void)ctx;
  TEST_ASSERT_TRUE(esy_os_task_is_current(s_expected_owner));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_master_worker_destroy(gw));
  esy_proto_modbus_request_t next = make_master_request();
  esy_err_t result = esy_proto_modbus_uart_master_worker_submit(gw, &next);
  TEST_ASSERT_EQUAL(s_expect_shutdown ? ESY_ERR_STATE : ESY_OK, result);
  ++s_master_complete_count;
}

void test_modbus_abort_callback_runs_on_owner_and_can_submit_reentrantly(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = esy_proto_modbus_uart_master_worker_default_cfg();
  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.on_complete = owner_completion;
  s_expect_shutdown = false;
  attach_master_storage(&cfg);
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  s_expected_owner = fake_esy_os_task_get_last_created()->task_handle;
  esy_proto_modbus_request_t req = make_master_request();
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_abort_all(gw, true));
  TEST_ASSERT_EQUAL(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(1u, esy_proto_modbus_uart_master_worker_get_queue_depth(gw));
  s_expect_shutdown = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_master_requires_idle_create_and_drains_before_releasing_storage(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = esy_proto_modbus_uart_master_worker_default_cfg();
  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  attach_master_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  slot->tx_busy = true;
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  TEST_ASSERT_NULL(gw);
  slot->tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  esy_proto_modbus_request_t req = make_master_request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_NOT_NULL(slot->last_dma_callback);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_modbus_slave_dma_error_and_repeated_lifecycle_release_safely(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg = esy_proto_modbus_uart_slave_worker_default_cfg();
  cfg.uart = &s_uart;
  cfg.unit_id = 0x22u;
  cfg.rtu_frame_timeout_ms = 2u;
  cfg.on_request = fake_slave_request;
  attach_slave_storage(&cfg);
  esy_proto_modbus_uart_slave_worker_t *gw = NULL, *duplicate = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_create(&cfg, &s_slave_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_proto_modbus_uart_slave_worker_create(&cfg, &s_slave_storage, &duplicate));
  uint8_t frame[64];
  uint16_t length;
  build_rtu_frame(frame, &length, 0x22u, (uint8_t[]){ESY_PROTO_MODBUS_FUNC_READ_COILS, 0}, 2u);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  feed_slave_frame_after_idle_gap(gw, frame, length, 1u, 3u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_slave_worker_destroy(gw));
  fake_esy_hal_uart_fail_last_dma(&s_uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_create(&cfg, &s_slave_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_destroy(gw));
}

static esy_proto_modbus_uart_master_worker_config_t regression_config(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = esy_proto_modbus_uart_master_worker_default_cfg();
  cfg.uart = &s_uart;
  cfg.queue_depth = 2u;
  cfg.on_complete = master_complete_callback;
  cfg.rtu_frame_timeout_ms = 2u;
  attach_master_storage(&cfg);
  return cfg;
}

void test_master_async_tx_failure_completes_without_response_timeout(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  esy_proto_modbus_request_t req = make_master_request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_fail_last_dma(&s_uart);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, s_master_status);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_broadcast_waits_for_terminal_callback_even_when_hardware_is_idle(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  esy_proto_modbus_request_t req = make_master_request(); req.unit_id = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  slot->tx_busy = false;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(0u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_NOT_NULL(slot->last_dma_callback_arg);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_EQUAL(2u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_master_statuses[0]);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_master_statuses[1]);
}

void test_queued_abort_and_destroy_each_notify_once(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  esy_proto_modbus_request_t req = make_master_request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_abort_all(gw, false));
  TEST_ASSERT_EQUAL(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_master_status);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_EQUAL(2u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, s_master_status);
}

void test_rx_start_failure_rolls_back_and_permits_retry(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  slot->rx_stream_start_result = ESY_ERR_NOT_SUPPORTED;
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_NULL(slot->rx_stream_storage);
  slot->rx_stream_start_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_failed_task_creation_retains_handle_if_rx_cleanup_fails(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  slot->rx_stream_stop_result = ESY_ERR_IO;
  fake_esy_os_task_set_create_result(ESY_ERR_NO_MEM);
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  TEST_ASSERT_NOT_NULL(gw);
  TEST_ASSERT_FALSE(esy_proto_modbus_uart_master_worker_is_initialized(gw));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_EQUAL(0u, s_master_complete_count);
  TEST_ASSERT_NOT_NULL(slot->rx_stream_storage);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_EQUAL(0u, s_master_complete_count);
  slot->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_NULL(slot->rx_stream_storage);
  fake_esy_os_task_set_create_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_running_rx_stop_failure_retains_worker_until_retry(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  slot->rx_stream_stop_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_NOT_NULL(slot->rx_stream_storage);
  slot->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
  TEST_ASSERT_NULL(slot->rx_stream_storage);
}

void test_rx_stream_overflow_discards_frame_and_retries_failed_recovery(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  slot->rx_stream_callback(&s_uart, ESY_UART_RX_STREAM_EVENT_OVERFLOW, slot->rx_stream_user_data);
  slot->rx_stream_stop_result = ESY_ERR_IO;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(1u, slot->rx_stream_start_count);
  slot->rx_stream_stop_result = ESY_OK;
  slot->rx_stream_start_result = ESY_ERR_IO;
  slot->retain_failed_rx_start = true;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_NOT_NULL(slot->rx_stream_storage);
  uint32_t starts = slot->rx_stream_start_count;
  slot->rx_stream_stop_result = ESY_ERR_TIMEOUT;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(starts, slot->rx_stream_start_count);
  slot->rx_stream_stop_result = ESY_OK;
  slot->rx_stream_start_result = ESY_OK;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_NOT_NULL(slot->rx_stream_storage);
  fake_esy_hal_system_set_tick(3u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(0u, slot->receive_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_rx_stream_is_consumed_while_tx_callback_is_pending(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  esy_proto_modbus_request_t req = make_master_request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  uint8_t frame[64]; uint16_t length;
  build_rtu_frame(frame, &length, req.unit_id, (uint8_t[]){1u, 1u, 0x55u}, 3u);
  fake_esy_hal_uart_push_rx_stream(&s_uart, frame, length);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  TEST_ASSERT_EQUAL(0u, slot->rx_stream_used);
  TEST_ASSERT_EQUAL(0u, slot->receive_count);
  fake_esy_hal_system_set_tick(3u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(0u, s_master_complete_count);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(1u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_PROTO_MODBUS_REQ_STATUS_OK, s_master_status);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_rx_read_failures_reset_partial_frame_before_recovery(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  slot->rx_stream_peek_result = ESY_ERR_IO;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  slot->rx_stream_peek_result = ESY_OK;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(2u, slot->rx_stream_start_count);
  fake_esy_hal_system_set_tick(3u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  uint8_t data[] = {1u, 2u, 3u};
  fake_esy_hal_uart_push_rx_stream(&s_uart, data, sizeof(data));
  slot->rx_stream_consume_result = ESY_ERR_IO;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  slot->rx_stream_consume_result = ESY_OK;
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(3u, slot->rx_stream_start_count);
  TEST_ASSERT_EQUAL(0u, s_master_complete_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}

void test_slave_failed_task_creation_keeps_rx_storage_until_cleanup_succeeds(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg = esy_proto_modbus_uart_slave_worker_default_cfg();
  cfg.uart = &s_uart;
  cfg.on_request = fake_slave_request;
  attach_slave_storage(&cfg);
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_uart);
  slot->rx_stream_stop_result = ESY_ERR_IO;
  fake_esy_os_task_set_create_result(ESY_ERR_FAIL);
  esy_proto_modbus_uart_slave_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_proto_modbus_uart_slave_worker_create(&cfg, &s_slave_storage, &gw));
  TEST_ASSERT_NOT_NULL(gw);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_proto_modbus_uart_slave_worker_destroy(gw));
  slot->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_slave_worker_destroy(gw));
  TEST_ASSERT_NULL(slot->rx_stream_storage);
}

void test_broadcast_turnaround_defers_next_tx_across_tick_wrap(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  cfg.rtu_frame_timeout_ms = 10u;
  cfg.broadcast_turnaround_ms = 20u;
  esy_proto_modbus_uart_master_worker_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(&cfg, &s_master_storage, &gw));
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  esy_proto_modbus_request_t req = make_master_request();
  req.unit_id = 0u;
  req.func_code = ESY_PROTO_MODBUS_FUNC_WRITE_SINGLE_COIL;
  req.pdu[0] = req.func_code; req.pdu[1]=0; req.pdu[2]=1; req.pdu[3]=0xff; req.pdu[4]=0; req.pdu_len=5;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(gw, &req));
  fake_esy_hal_system_set_tick(UINT32_MAX - 10u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_uart_find(&s_uart)->send_dma_count);
  fake_esy_hal_system_set_tick(9u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_uart_find(&s_uart)->send_dma_count);
  fake_esy_hal_system_set_tick(10u);
  esy_proto_modbus_uart_master_worker_test_iteration(gw);
  TEST_ASSERT_EQUAL(2u, fake_esy_hal_uart_find(&s_uart)->send_dma_count);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(gw));
}


void test_worker_owns_distinct_channel_storage_and_preserves_typed_callback(void) {
  esy_uart_t independent_uart = {0};
  esy_proto_modbus_uart_master_channel_storage_t channel_storage = {0};
  ESY_PROTO_MODBUS_UART_MASTER_CHANNEL_STATIC_STORAGE(queues, 2u, 2u);
  esy_proto_modbus_uart_master_channel_config_t channel_cfg =
      esy_proto_modbus_uart_master_channel_default_cfg();
  channel_cfg.uart = &independent_uart;
  channel_cfg.queue_depth = 2u;
  channel_cfg.static_resources.slot_storage = queues.slot_storage;
  channel_cfg.static_resources.slot_storage_count = 2u;
  channel_cfg.static_resources.payload_storage = queues.payload_storage;
  channel_cfg.static_resources.payload_storage_count = 2u;
  esy_proto_modbus_uart_master_channel_t *channel = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_create(
      &channel_cfg, &channel_storage, &channel));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_create_static_count());

  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  esy_proto_modbus_uart_master_worker_t *worker = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(
      &cfg, &s_master_storage, &worker));
  TEST_ASSERT_EQUAL(1u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_NOT_EQUAL((uintptr_t)channel, (uintptr_t)worker);
  esy_proto_modbus_request_t req = make_master_request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(worker, &req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_abort_all(worker, false));
  TEST_ASSERT_EQUAL_PTR(worker, s_complete_worker);
  TEST_ASSERT_EQUAL(1u, s_master_complete_count);
  TEST_ASSERT_TRUE(esy_proto_modbus_uart_master_channel_is_initialized(channel));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(worker));
  TEST_ASSERT_TRUE(esy_proto_modbus_uart_master_channel_is_initialized(channel));
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_channel_destroy(channel));
}

static void external_destroy_during_completion(esy_proto_modbus_uart_master_worker_t *worker,
    esy_proto_modbus_req_status_t status, uint8_t unit, esy_proto_modbus_func_t function,
    const uint8_t *response, uint16_t response_length, void *arg) {
  (void)status; (void)unit; (void)function; (void)response; (void)response_length; (void)arg;
  esy_os_task_t *owner = fake_esy_os_task_get_current();
  TEST_ASSERT_NOT_NULL(owner);
  fake_esy_os_task_set_current(NULL);
  fake_esy_os_task_set_delay_hook(NULL);
  /* Simulate another task preempting an owner slice. Stop waits for that slice. */
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_proto_modbus_uart_master_worker_destroy(worker));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_NOT_NULL(fake_esy_hal_uart_find(&s_uart)->rx_stream_storage);
  fake_esy_os_task_set_current(owner);
  ++s_master_complete_count;
}

void test_external_destroy_during_owner_slice_waits_for_cleanup_before_parking(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = regression_config();
  cfg.on_complete = external_destroy_during_completion;
  esy_proto_modbus_uart_master_worker_t *worker = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_create(
      &cfg, &s_master_storage, &worker));
  esy_os_task_t *caller = fake_esy_os_task_get_current();
  esy_os_task_t *owner = fake_esy_os_task_get_last_created()->task_handle;
  esy_proto_modbus_request_t req = make_master_request();
  req.unit_id = 0u;
  req.func_code = ESY_PROTO_MODBUS_FUNC_WRITE_SINGLE_COIL;
  req.pdu[0] = req.func_code; req.pdu[1] = 0u; req.pdu[2] = 1u;
  req.pdu[3] = 0xffu; req.pdu[4] = 0u; req.pdu_len = 5u;
  fake_esy_hal_uart_set_send_dma_result(&s_uart, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_submit(worker, &req));
  fake_esy_os_task_set_current(owner);
  esy_proto_modbus_uart_master_worker_test_iteration(worker);
  fake_esy_hal_uart_complete_last_dma(&s_uart);
  esy_proto_modbus_uart_master_worker_test_iteration(worker);
  TEST_ASSERT_EQUAL(1u, s_master_complete_count);
  TEST_ASSERT_FALSE(fake_esy_os_task_get_last_created()->parked);
  TEST_ASSERT_TRUE(esy_proto_modbus_uart_master_worker_is_initialized(worker));
  TEST_ASSERT_NOT_NULL(fake_esy_hal_uart_find(&s_uart)->rx_stream_storage);
  fake_esy_os_task_set_current(caller);
  fake_esy_os_task_set_delay_hook(schedule_workers);
  TEST_ASSERT_EQUAL(ESY_OK, esy_proto_modbus_uart_master_worker_destroy(worker));
  TEST_ASSERT_EQUAL(1u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_NULL(fake_esy_hal_uart_find(&s_uart)->rx_stream_storage);
}
