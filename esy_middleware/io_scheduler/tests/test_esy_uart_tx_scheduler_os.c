/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "unity.h"
#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler_os.h"
#include "fake_esy_hal_system.h"
#include "fake_esy_hal_uart.h"
#include "fake_esy_os.h"
#include "fake_esy_os_queue.h"
#include "fake_esy_os_task.h"
#include <string.h>

extern void esy_uart_tx_scheduler_os_test_iteration(void *arg);
#define TEST_QUEUE_CAPACITY 2u
#define TEST_STACK_BYTES 1024u

typedef struct {
  esy_uart_tx_scheduler_os_t adapter;
  esy_uart_t uart;
  esy_uart_tx_scheduler_tx_request_t requests[TEST_QUEUE_CAPACITY];
  esy_os_queue_static_storage_t queue;
  esy_os_task_static_storage_t task;
  _Alignas(ESY_OS_TASK_STACK_ALIGN) uint8_t stack[TEST_STACK_BYTES];
  esy_uart_tx_scheduler_os_config_t config;
  unsigned callbacks;
  esy_uart_tx_scheduler_tx_status_t last_status;
  esy_err_t last_error;
  bool scheduling;
  bool assert_worker_callback;
} uart_os_test_ctx_t;
static uart_os_test_ctx_t s_ctx;

static void worker_iteration(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_get_last_created();
  if (entry == NULL || entry->deleted || entry->parked) return;
  esy_os_task_t *caller = fake_esy_os_task_get_current();
  fake_esy_os_task_set_current(entry->task_handle);
  esy_uart_tx_scheduler_os_test_iteration(entry->arg);
  fake_esy_os_task_set_current(caller);
}
static void schedule_worker(uint32_t ms) {
  (void)ms;
  if (s_ctx.scheduling) return;
  s_ctx.scheduling = true;
  worker_iteration();
  s_ctx.scheduling = false;
}
void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_os_queue_reset();
  fake_esy_hal_uart_reset();
  fake_esy_hal_system_reset();
  s_ctx.config = (esy_uart_tx_scheduler_os_config_t){
      .task_name = "uart-tx", .task_stack_size = TEST_STACK_BYTES,
      .task_priority = 2u, .task_core_affinity = 0u,
      .static_resources = {.requests = s_ctx.requests,
          .request_count = TEST_QUEUE_CAPACITY, .queue_control_storage = &s_ctx.queue,
          .task_stack_buffer = s_ctx.stack, .task_stack_buffer_size = sizeof(s_ctx.stack),
          .task_control_storage = &s_ctx.task}};
  esy_uart_config_t config = {.baudrate = 115200u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&s_ctx.uart, &config));
}
void tearDown(void) {}
static void completed(const uint8_t *data, size_t length,
    esy_uart_tx_scheduler_tx_status_t status, esy_err_t error, void *arg) {
  TEST_ASSERT_NOT_NULL(data);
  TEST_ASSERT_GREATER_THAN(0u, length);
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  if (s_ctx.assert_worker_callback) {
    TEST_ASSERT_TRUE(esy_os_task_is_current(fake_esy_os_task_get_last_created()->task_handle));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_uart_tx_scheduler_os_request_stop(&s_ctx.adapter, true));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_os_destroy(&s_ctx.adapter));
  }
  ++s_ctx.callbacks;
  s_ctx.last_status = status;
  s_ctx.last_error = error;
}
static esy_uart_tx_scheduler_tx_request_t request_for(const uint8_t *data, size_t length) {
  return (esy_uart_tx_scheduler_tx_request_t){.data = data, .length = length,
      .on_complete = completed, .user_data = &s_ctx, .timeout_ms = 37u};
}
static void create_adapter(void) {
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_uart_tx_scheduler_os_create(&s_ctx.adapter, &s_ctx.uart, &s_ctx.config));
}
static void stop_and_destroy(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_request_stop(&s_ctx.adapter, false));
  fake_esy_os_task_set_delay_hook(schedule_worker);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_destroy(&s_ctx.adapter));
}
void test_uart_os_create_validates_static_resources_and_rolls_back_task_failure(void) {
  s_ctx.config.static_resources.task_stack_buffer = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_uart_tx_scheduler_os_create(&s_ctx.adapter, &s_ctx.uart, &s_ctx.config));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_queue_get_observations()->create_count);
  s_ctx.config.static_resources.task_stack_buffer = s_ctx.stack;
  fake_esy_os_task_set_park_supported(false);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
      esy_uart_tx_scheduler_os_create(&s_ctx.adapter, &s_ctx.uart, &s_ctx.config));
  fake_esy_os_task_set_park_supported(true);
  fake_esy_os_task_set_create_result(ESY_ERR_NO_MEM);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,
      esy_uart_tx_scheduler_os_create(&s_ctx.adapter, &s_ctx.uart, &s_ctx.config));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_queue_get_observations()->delete_count);
  fake_esy_os_task_set_create_result(ESY_OK);
  create_adapter();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_uart_tx_scheduler_os_create(&s_ctx.adapter, &s_ctx.uart, &s_ctx.config));
  stop_and_destroy();
  create_adapter();
  stop_and_destroy();
}
void test_uart_os_submission_copies_descriptor_and_worker_automatically_runs_timed_fallback(void) {
  uint8_t data[] = {1u, 2u, 3u};
  create_adapter();
  esy_uart_tx_scheduler_tx_request_t request = request_for(data, sizeof(data));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_submit(&s_ctx.adapter, &request));
  request.data = NULL;
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_EQUAL_UINT32(0u, slot->send_count);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.callbacks);
  s_ctx.assert_worker_callback = true;
  worker_iteration();
  worker_iteration();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_OK, s_ctx.last_status);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_timeout_count);
  TEST_ASSERT_EQUAL_UINT32(37u, slot->last_timeout_ms);
  TEST_ASSERT_EQUAL_MEMORY(data, slot->last_tx, sizeof(data));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_queue_get_observations()->last_send_timeout_ms);
  stop_and_destroy();
}
void test_uart_os_queue_full_rejects_without_callback_or_losing_accepted_requests(void) {
  uint8_t byte = 0x42u;
  create_adapter();
  esy_uart_tx_scheduler_tx_request_t request = request_for(&byte, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_submit(&s_ctx.adapter, &request));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_submit(&s_ctx.adapter, &request));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_uart_tx_scheduler_os_submit(&s_ctx.adapter, &request));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_request_stop(&s_ctx.adapter, true));
  worker_iteration();
  worker_iteration();
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, s_ctx.last_status);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_uart_find(&s_ctx.uart)->send_count);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_os_submit(&s_ctx.adapter, &request));
  fake_esy_os_task_set_delay_hook(schedule_worker);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_destroy(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.callbacks);
}
void test_uart_os_partial_synchronous_failure_releases_request_once_without_retry(void) {
  uint8_t bytes[4] = {0};
  create_adapter();
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->override_send_result = true;
  slot->send_result = 2;
  esy_uart_tx_scheduler_tx_request_t request = request_for(bytes, sizeof(bytes));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_submit(&s_ctx.adapter, &request));
  worker_iteration();
  worker_iteration();
  worker_iteration();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_ERROR, s_ctx.last_status);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_timeout_count);
  stop_and_destroy();
}
void test_uart_os_dma_abort_failure_retains_worker_queue_and_buffer_until_recovery(void) {
  uint8_t byte = 1u;
  create_adapter();
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->send_dma_result = ESY_OK;
  esy_uart_tx_scheduler_tx_request_t request = request_for(&byte, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_submit(&s_ctx.adapter, &request));
  worker_iteration();
  TEST_ASSERT_TRUE(slot->dma_pending);
  slot->abort_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_request_abort(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_request_stop(&s_ctx.adapter, false));
  fake_esy_os_task_set_delay_hook(schedule_worker);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_os_destroy(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.callbacks);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_queue_get_observations()->delete_count);
  TEST_ASSERT_TRUE(slot->dma_pending);
  slot->abort_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_destroy(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, s_ctx.last_status);
  TEST_ASSERT_FALSE(slot->dma_pending);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_queue_get_observations()->delete_count);
}
void test_uart_os_controls_reject_isr_and_destroy_requires_explicit_shutdown(void) {
  uint8_t byte = 0u;
  create_adapter();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_os_destroy(&s_ctx.adapter));
  esy_uart_tx_scheduler_tx_request_t request = request_for(&byte, 1u);
  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_uart_tx_scheduler_os_submit(&s_ctx.adapter, &request));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_uart_tx_scheduler_os_request_abort(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_os_destroy(&s_ctx.adapter));
  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  stop_and_destroy();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_os_destroy(&s_ctx.adapter));
}
