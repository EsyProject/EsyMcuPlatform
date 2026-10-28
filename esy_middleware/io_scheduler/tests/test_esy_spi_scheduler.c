/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_spi_scheduler.h"

#include <string.h>

#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_spi.h"
#include "unity.h"

static esy_spi_t s_spi;
static uint32_t s_spi_post_count;
static esy_spi_scheduler_storage_t s_spi_scheduler_storage;
static esy_spi_scheduler_request_t s_spi_reqs[2];
static uint8_t s_spi_tx_buffers[2][8];
static uint8_t s_spi_rx_buffers[2][8];
static _Alignas(ESY_OS_TASK_STACK_ALIGN) uint8_t s_spi_task_stack[256];
static esy_os_task_static_storage_t s_spi_task_control_storage;

extern void esy_spi_scheduler_test_iteration(esy_spi_scheduler_t *gw);

static void spi_post_callback(esy_err_t result, uint8_t *rx_buf, void *arg) {
  TEST_ASSERT_EQUAL(ESY_OK, result);
  TEST_ASSERT_NOT_NULL(rx_buf);
  TEST_ASSERT_EQUAL_PTR(&s_spi_post_count, arg);
  s_spi_post_count++;
}

static void spi_ignore_result(esy_err_t result, uint8_t *rx, void *arg) {
  (void)result;
  (void)rx;
  (void)arg;
}

static esy_spi_scheduler_config_t make_spi_cfg(void) {
  esy_spi_scheduler_config_t cfg;
  esy_spi_scheduler_default_cfg(&cfg);
  cfg.tx_alloc_size = 8u;
  cfg.rx_alloc_size = 8u;
  cfg.block_count = 2u;
  cfg.task_stack_size = 256u;
  cfg.task_priority = 4u;
  cfg.task_core_affinity = 2u;
  cfg.task_name = "spi_scheduler";
  return cfg;
}

static void *make_misaligned_stack_buffer(uint8_t *storage) {
  if (((uintptr_t)storage % ESY_OS_TASK_STACK_ALIGN) == 0u) {
    return &storage[1];
  }

  return storage;
}

static void attach_spi_storage(esy_spi_scheduler_config_t *cfg) {
  cfg->static_resources.request_storage = s_spi_reqs;
  cfg->static_resources.request_storage_count = 2u;
  cfg->static_resources.tx_buffer_storage = (uint8_t *)s_spi_tx_buffers;
  cfg->static_resources.tx_buffer_storage_size = sizeof(s_spi_tx_buffers);
  cfg->static_resources.rx_buffer_storage = (uint8_t *)s_spi_rx_buffers;
  cfg->static_resources.rx_buffer_storage_size = sizeof(s_spi_rx_buffers);
  cfg->static_resources.task_stack_buffer = s_spi_task_stack;
  cfg->static_resources.task_stack_buffer_size = sizeof(s_spi_task_stack);
  cfg->static_resources.task_control_storage = &s_spi_task_control_storage;
}


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
    esy_spi_scheduler_test_iteration(entry->arg);
  }
  fake_esy_os_task_set_current(caller);
  s_scheduling = false;
}

void setUp(void) {
  s_scheduling = false;
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_os_task_set_delay_hook(schedule_workers);
  fake_esy_hal_spi_reset();
  memset(&s_spi_scheduler_storage, 0, sizeof(s_spi_scheduler_storage));
  memset(&s_spi_task_control_storage, 0, sizeof(s_spi_task_control_storage));
  s_spi = (esy_spi_t){0};
  s_spi_post_count = 0u;
}

void tearDown(void) {}

void test_spi_default_cfg_is_ready_to_use(void) {
  esy_spi_scheduler_config_t cfg;

  esy_spi_scheduler_default_cfg(&cfg);

  TEST_ASSERT_EQUAL_UINT32(0u, cfg.tx_alloc_size);
  TEST_ASSERT_EQUAL_UINT32(0u, cfg.rx_alloc_size);
  TEST_ASSERT_EQUAL_UINT32(4u, cfg.block_count);
  TEST_ASSERT_EQUAL_UINT32(512u, cfg.task_stack_size);
  TEST_ASSERT_NULL(cfg.static_resources.task_stack_buffer);
  TEST_ASSERT_EQUAL_UINT32(0u, cfg.static_resources.task_stack_buffer_size);
  TEST_ASSERT_NULL(cfg.static_resources.task_control_storage);
  TEST_ASSERT_EQUAL_UINT32(1u, cfg.task_priority);
  TEST_ASSERT_EQUAL_UINT8(ESY_CORE_ALL, cfg.task_core_affinity);
}

void test_spi_static_storage_macro_declares_bindable_resources(void) {
  ESY_SPI_SCHEDULER_STATIC_STORAGE(scheduler_storage, 2u, 16u, 16u, 256u);
  esy_spi_scheduler_config_t cfg = make_spi_cfg();

  cfg.static_resources.request_storage = scheduler_storage.request_storage;
  cfg.static_resources.request_storage_count =
      sizeof(scheduler_storage.request_storage) /
      sizeof(scheduler_storage.request_storage[0]);
  cfg.static_resources.tx_buffer_storage = scheduler_storage.tx_buffer_storage;
  cfg.static_resources.tx_buffer_storage_size =
      sizeof(scheduler_storage.tx_buffer_storage);
  cfg.static_resources.rx_buffer_storage = scheduler_storage.rx_buffer_storage;
  cfg.static_resources.rx_buffer_storage_size =
      sizeof(scheduler_storage.rx_buffer_storage);
  cfg.static_resources.task_stack_buffer = scheduler_storage.task_stack_buffer;
  cfg.static_resources.task_stack_buffer_size =
      sizeof(scheduler_storage.task_stack_buffer);
  cfg.static_resources.task_control_storage =
      &scheduler_storage.task_control_storage;

  TEST_ASSERT_EQUAL_UINT32(2u, cfg.static_resources.request_storage_count);
  TEST_ASSERT_EQUAL_UINT32(16u, cfg.static_resources.tx_buffer_storage_size);
  TEST_ASSERT_EQUAL_UINT32(16u, cfg.static_resources.rx_buffer_storage_size);
  TEST_ASSERT_NOT_NULL(cfg.static_resources.task_stack_buffer);
  TEST_ASSERT_NOT_NULL(cfg.static_resources.task_control_storage);
}

void test_spi_create_rejects_invalid_arguments(void) {
  esy_spi_scheduler_t *gw = (esy_spi_scheduler_t *)0x1;
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_create(NULL, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_create(&s_spi, NULL, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, NULL));

  cfg.block_count = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
}

void test_spi_create_rejects_misaligned_task_stack(void) {
  uint8_t raw_stack[256u + ESY_OS_TASK_STACK_ALIGN];
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  esy_spi_scheduler_t *gw = (esy_spi_scheduler_t *)0x1;
  attach_spi_storage(&cfg);
  cfg.static_resources.task_stack_buffer =
      make_misaligned_stack_buffer(raw_stack);
  cfg.static_resources.task_stack_buffer_size = 256u;

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
}

void test_spi_create_submit_and_destroy(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_NOT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_create_static_count());

  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  TEST_ASSERT_NOT_NULL(task);
  TEST_ASSERT_EQUAL_STRING("spi_scheduler", task->name);
  TEST_ASSERT_TRUE(task->created_static);
  TEST_ASSERT_EQUAL_PTR(s_spi_task_stack, task->stack_buffer);
  TEST_ASSERT_EQUAL_PTR(&s_spi_task_control_storage, task->static_storage);
  TEST_ASSERT_EQUAL_UINT8(2u, task->core_affinity);

  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  TEST_ASSERT_NOT_NULL(req);
  TEST_ASSERT_NOT_NULL(req->tx_alloc_buf);
  TEST_ASSERT_NOT_NULL(req->rx_alloc_buf);
  TEST_ASSERT_EQUAL_PTR(req->tx_alloc_buf, req->tx_buf);
  TEST_ASSERT_EQUAL_PTR(req->rx_alloc_buf, req->rx_buf);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_submit(gw, req));

  req->len = 4u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_submit(gw, req));
  TEST_ASSERT_EQUAL_UINT32(1u, task->notify_give_count);

  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_delete_count());
}

void test_spi_convenience_buffer_submit_uses_reserved_block(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint8_t *tx_buf = NULL;
  uint8_t *rx_buf = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_spi_scheduler_get_write_buffer_with_arg(&tx_buf, &rx_buf, gw));
  TEST_ASSERT_NOT_NULL(tx_buf);
  TEST_ASSERT_NOT_NULL(rx_buf);
  tx_buf[0] = 0xA5u;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_spi_scheduler_submit_result_with_arg(tx_buf, rx_buf, 1u, NULL, NULL, spi_ignore_result,
                                      NULL, gw));

  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  TEST_ASSERT_EQUAL_UINT32(1u, task->notify_give_count);

  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_convenience_api_reports_reserve_and_submit_errors(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint8_t *tx_buf = NULL;
  uint8_t *rx_buf = NULL;
  uint8_t *reserved_tx = NULL;
  uint8_t *reserved_rx = NULL;
  uint8_t foreign_tx = 0x5Au;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_get_write_buffer_with_arg(
                              NULL, NULL, gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_spi_scheduler_get_write_buffer_with_arg(
                              &tx_buf, &rx_buf, gw));
  TEST_ASSERT_NOT_NULL(tx_buf);
  TEST_ASSERT_NOT_NULL(rx_buf);
  reserved_tx = tx_buf;
  reserved_rx = rx_buf;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_spi_scheduler_get_write_buffer_with_arg(
                              &tx_buf, &rx_buf, gw));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_spi_scheduler_submit_result_with_arg(
                              &foreign_tx, reserved_rx, 1u, NULL, NULL, spi_ignore_result, NULL,
                              gw));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_submit_result_with_arg(
                              reserved_tx, reserved_rx, 0u, NULL, NULL, spi_ignore_result,
                              NULL, gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_spi_scheduler_submit_result_with_arg(
                              reserved_tx, reserved_rx, 1u, NULL, NULL, spi_ignore_result,
                              NULL, gw));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_spi_scheduler_submit_result_with_arg(
                              reserved_tx, reserved_rx, 1u, NULL, NULL, spi_ignore_result,
                              NULL, gw));

  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_convenience_api_can_submit_without_prior_buffer_reserve(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint8_t tx_buf[] = {0x11u};
  uint8_t rx_buf[1] = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_spi_scheduler_submit_result_with_arg(
                              tx_buf, rx_buf, sizeof(tx_buf), NULL, NULL, spi_ignore_result,
                              NULL, gw));

  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  TEST_ASSERT_EQUAL_UINT32(1u, task->notify_give_count);

  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_quiesce_requires_queued_requests_to_drain_before_resume(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  esy_spi_scheduler_t *gw = NULL;
  uint8_t tx_buf[] = {0x11u};
  uint8_t rx_buf[1] = {0};

  attach_spi_storage(&cfg);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_spi_scheduler_submit_result_with_arg(
                  tx_buf, rx_buf, sizeof(tx_buf), NULL, NULL, spi_ignore_result, NULL, gw));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_spi_scheduler_quiesce(gw));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_spi_get_tx_count(&s_spi));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_quiesce(gw));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_spi_scheduler_submit_result_with_arg(
      tx_buf, rx_buf, sizeof(tx_buf), NULL, NULL, spi_ignore_result, NULL, gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_resume(gw));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_create_rolls_back_when_task_create_fails(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = (esy_spi_scheduler_t *)0x1;

  fake_esy_os_task_set_create_result(ESY_ERR_FAIL);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_FAIL,
                          esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_delete_count());
}

void test_spi_iteration_runs_blocking_transfer_and_post_callback(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint8_t rx_data[] = {0xA1u, 0xB2u};

  fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_NOT_SUPPORTED);
  fake_esy_hal_spi_set_rx_data(&s_spi, rx_data, sizeof(rx_data));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));

  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  TEST_ASSERT_NOT_NULL(req);
  req->tx_buf[0] = 0x11u;
  req->tx_buf[1] = 0x22u;
  req->len = 2u;
  req->result_callback = spi_post_callback;
  req->result_callback_arg = &s_spi_post_count;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_submit(gw, req));

  esy_spi_scheduler_test_iteration(gw);

  TEST_ASSERT_EQUAL_UINT32(1u, s_spi_post_count);
  TEST_ASSERT_EQUAL_UINT8(0xA1u, req->rx_buf[0]);
  TEST_ASSERT_EQUAL_UINT8(0xB2u, req->rx_buf[1]);
  TEST_ASSERT_NOT_NULL(esy_spi_scheduler_get_write_block(gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_destroy_drains_pending_dma_before_releasing_resources(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;

  fake_esy_hal_spi_set_transfer_dma_async(true);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));

  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  TEST_ASSERT_NOT_NULL(req);
  req->len = 6u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_submit(gw, req));

  esy_spi_scheduler_test_iteration(gw);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_delete_count());
  fake_esy_hal_spi_complete_last_dma(&s_spi);
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_task_get_delete_count());
}

void test_spi_submit_requires_current_outstanding_write_block(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));

  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  TEST_ASSERT_NOT_NULL(req);
  TEST_ASSERT_NULL(esy_spi_scheduler_get_write_block(gw));

  req->len = 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_submit(gw, req));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_spi_scheduler_submit(gw, req));

  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_convenience_api_holds_single_outstanding_block_until_submit(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint8_t *tx_buf = NULL;
  uint8_t *rx_buf = NULL;
  uint8_t *second_tx = (uint8_t *)0x1;
  uint8_t *second_rx = (uint8_t *)0x1;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_spi_scheduler_get_write_buffer_with_arg(&tx_buf, &rx_buf, gw));
  TEST_ASSERT_NOT_NULL(tx_buf);
  TEST_ASSERT_NOT_NULL(rx_buf);

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_spi_scheduler_get_write_buffer_with_arg(&second_tx, &second_rx, gw));
  TEST_ASSERT_NULL(second_tx);
  TEST_ASSERT_NULL(second_rx);

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_spi_scheduler_submit_result_with_arg(tx_buf, rx_buf, 1u, NULL, NULL, spi_ignore_result,
                                      NULL, gw));
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_NOT_NULL(esy_spi_scheduler_get_write_block(gw));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_create_uses_caller_owned_storage_without_malloc(void) {
  esy_spi_scheduler_storage_t storage = {0};
  esy_spi_scheduler_request_t reqs[2];
  uint8_t tx_buffers[2][8];
  uint8_t rx_buffers[2][8];
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;

  cfg.static_resources.request_storage = reqs;
  cfg.static_resources.request_storage_count = 2u;
  cfg.static_resources.tx_buffer_storage = (uint8_t *)tx_buffers;
  cfg.static_resources.tx_buffer_storage_size = sizeof(tx_buffers);
  cfg.static_resources.rx_buffer_storage = (uint8_t *)rx_buffers;
  cfg.static_resources.rx_buffer_storage_size = sizeof(rx_buffers);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_spi_scheduler_create(&s_spi, &cfg, &storage, &gw));
  TEST_ASSERT_NOT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);

  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  TEST_ASSERT_NOT_NULL(req);
  TEST_ASSERT_TRUE(req >= &reqs[0] && req < &reqs[2]);
  TEST_ASSERT_TRUE(req->tx_buf >= &tx_buffers[0][0] &&
                   req->tx_buf < (&tx_buffers[0][0] + sizeof(tx_buffers)));
  TEST_ASSERT_TRUE(req->rx_buf >= &rx_buffers[0][0] &&
                   req->rx_buf < (&rx_buffers[0][0] + sizeof(rx_buffers)));
  req->len = 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_submit(gw, req));

  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);
}

void test_spi_create_rejects_small_caller_owned_storage(void) {
  esy_spi_scheduler_storage_t storage = {0};
  esy_spi_scheduler_request_t reqs[1];
  uint8_t tx_buffers[1];
  uint8_t rx_buffers[1];
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;

  cfg.static_resources.request_storage = reqs;
  cfg.static_resources.request_storage_count = 1u;
  cfg.static_resources.tx_buffer_storage = tx_buffers;
  cfg.static_resources.tx_buffer_storage_size = sizeof(tx_buffers);
  cfg.static_resources.rx_buffer_storage = rx_buffers;
  cfg.static_resources.rx_buffer_storage_size = sizeof(rx_buffers);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_create(&s_spi, &cfg, &storage, &gw));
  TEST_ASSERT_NULL(gw);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);
}

void test_spi_scheduler_null_and_empty_handle_paths_are_safe(void) {
  esy_spi_scheduler_config_t cfg;
  uint8_t *tx_buf = (uint8_t *)0x1;
  uint8_t *rx_buf = (uint8_t *)0x1;

  esy_spi_scheduler_default_cfg(NULL);
  esy_spi_scheduler_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_UINT32(4u, cfg.block_count);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_spi_scheduler_destroy(NULL));
  TEST_ASSERT_NULL(esy_spi_scheduler_get_write_block(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_submit(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_get_write_buffer_with_arg(
                              &tx_buf, NULL, NULL));
  TEST_ASSERT_NULL(tx_buf);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_get_write_buffer_with_arg(
                              NULL, &rx_buf, NULL));
  TEST_ASSERT_NULL(rx_buf);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_spi_scheduler_submit_result_with_arg(
                              NULL, NULL, 1u, NULL, NULL, spi_ignore_result, NULL, NULL));
  esy_spi_scheduler_test_iteration(NULL);
}

typedef struct {
  unsigned prepared;
  unsigned completed;
  esy_err_t prepare_result;
  esy_err_t completion_result;
} prepared_fixture_t;

static esy_err_t prepared_begin(void *arg) {
  prepared_fixture_t *fixture = arg;
  fixture->prepared++;
  return fixture->prepare_result;
}

static void prepared_complete(esy_err_t result, void *arg) {
  prepared_fixture_t *fixture = arg;
  fixture->completed++;
  fixture->completion_result = result;
}

void test_prepared_request_rejects_without_callbacks_or_reservation_leak(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  prepared_fixture_t fixture = {0};
  uint8_t pixels[4] = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_spi_scheduler_submit_prepared_with_arg(pixels, 4, NULL, prepared_complete, &fixture, gw));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_quiesce(gw));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY,
      esy_spi_scheduler_submit_prepared_with_arg(pixels, 4, prepared_begin, prepared_complete, &fixture, gw));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_resume(gw));
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_spi_scheduler_submit_prepared_with_arg(pixels, 4, prepared_begin, prepared_complete, &fixture, gw));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY,
      esy_spi_scheduler_submit_prepared_with_arg(pixels, 4, prepared_begin, prepared_complete, &fixture, gw));
  TEST_ASSERT_EQUAL_UINT(0, fixture.prepared);
  TEST_ASSERT_EQUAL_UINT(0, fixture.completed);
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT(1, fixture.prepared);
  TEST_ASSERT_EQUAL_UINT(1, fixture.completed);
  TEST_ASSERT_EQUAL_INT(ESY_OK, fixture.completion_result);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_prepared_request_reports_prepare_blocking_and_dma_failures_once(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint8_t pixels[8] = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  for (unsigned scenario = 0; scenario < 7; ++scenario) {
    prepared_fixture_t fixture = {0};
    esy_err_t expected = ESY_ERR_IO;
    fake_esy_hal_spi_reset();
    switch (scenario) {
    case 0: fixture.prepare_result = ESY_ERR_IO; break;
    case 1: fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_BUSY); expected = ESY_ERR_BUSY; break;
    case 2: fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_NOT_SUPPORTED);
            fake_esy_hal_spi_fail_write_after(0, ESY_ERR_IO); break;
    case 3: fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_NOT_SUPPORTED);
            fake_esy_hal_spi_fail_write_after(0, 3); break;
    case 4: fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_NOT_SUPPORTED); expected = ESY_OK; break;
    case 5: fake_esy_hal_spi_set_transfer_dma_async(true); break;
    case 6: expected = ESY_OK; break;
    }
    TEST_ASSERT_EQUAL_INT(ESY_OK,
        esy_spi_scheduler_submit_prepared_with_arg(pixels, 8, prepared_begin, prepared_complete, &fixture, gw));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
    esy_spi_scheduler_test_iteration(gw);
    if (scenario == 5) {
      const esy_spi_dma_transfer_result_t short_result = {
          .status = ESY_SPI_DMA_TRANSFER_OK, .tx_transferred = 3u};
      fake_esy_hal_spi_complete_last_dma_with_result(&s_spi, &short_result);
      TEST_ASSERT_EQUAL_UINT(0, fixture.completed);
    }
    esy_spi_scheduler_test_iteration(gw);
    TEST_ASSERT_EQUAL_UINT(1, fixture.prepared);
    TEST_ASSERT_EQUAL_UINT(1, fixture.completed);
    TEST_ASSERT_EQUAL_INT(expected, fixture.completion_result);
    if (scenario == 0 || scenario == 1) {
      TEST_ASSERT_EQUAL_UINT(0, fake_esy_hal_spi_get_tx_count(&s_spi));
    }
  }
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_prepared_raw_request_validates_callback_pair_and_tx_only_contract(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  prepared_fixture_t fixture = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_create(
      &s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  TEST_ASSERT_NOT_NULL(req);
  req->len = 4;
  req->prepare = prepared_begin;
  req->prepared_arg = &fixture;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_spi_scheduler_submit(gw, req));
  req->prepared_done = prepared_complete;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_spi_scheduler_submit(gw, req));
  req->rx_buf = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_submit(gw, req));
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL_UINT(1, fixture.prepared);
  TEST_ASSERT_EQUAL_UINT(1, fixture.completed);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_destroy(gw));
}

static esy_err_t s_terminal_result;
static size_t s_terminal_bytes;
static void capture_spi_result(esy_err_t result, uint8_t *buf, void *arg) {
  esy_spi_scheduler_request_t *req = arg;
  TEST_ASSERT_EQUAL_PTR(req->result == ESY_OK ? req->rx_buf : NULL, buf);
  TEST_ASSERT_EQUAL(req->result, result);
  s_terminal_result = result;
  s_terminal_bytes = req->transferred;
  ++s_spi_post_count;
}

void test_spi_one_direction_blocking_and_busy_dma_report_terminal_results(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  for (unsigned i = 0u; i < 3u; ++i) {
    esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
    req->len = i == 2u ? 6u : 2u;
    if (i == 0u) req->rx_buf = NULL;
    if (i == 1u) req->tx_buf = NULL;
    req->result_callback = capture_spi_result;
    req->result_callback_arg = req;
    if (i == 0u) fake_esy_hal_spi_fail_write_after(0u, 1);
    if (i == 1u) fake_esy_hal_spi_set_rx_data(&s_spi, (uint8_t[]){1, 2}, 2u);
    if (i == 2u) fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_BUSY);
    TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit(gw, req));
    esy_spi_scheduler_test_iteration(gw);
    esy_spi_scheduler_test_iteration(gw);
    TEST_ASSERT_EQUAL(i + 1u, s_spi_post_count);
    TEST_ASSERT_EQUAL(i == 0u ? ESY_ERR_IO : (i == 1u ? ESY_OK : ESY_ERR_BUSY), s_terminal_result);
    TEST_ASSERT_EQUAL(i == 0u ? 1u : (i == 1u ? 2u : 0u), s_terminal_bytes);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_single_direction_dma_passes_only_the_requested_descriptor(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint16_t tx[4] = {0}, rx[4] = {0};
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg,
      &s_spi_scheduler_storage, &gw));
  for (unsigned i = 0u; i < 4u; ++i) {
    esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
    req->len = sizeof(tx);
    req->tx_buf = (i & 1u) != 0u ? NULL : (uint8_t *)tx;
    req->rx_buf = (i & 1u) != 0u ? (uint8_t *)rx : NULL;
    req->transfer_size = i < 2u ? ESY_DMA_TRANSFER_SIZE_8 : ESY_DMA_TRANSFER_SIZE_16;
    req->result_callback = capture_spi_result;
    req->result_callback_arg = req;
    TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit(gw, req));
    esy_spi_scheduler_test_iteration(gw);
    esy_spi_scheduler_test_iteration(gw);
    TEST_ASSERT_EQUAL_UINT(i + 1u, s_spi_post_count);
    TEST_ASSERT_EQUAL(ESY_OK, s_terminal_result);
    TEST_ASSERT_EQUAL_UINT(sizeof(tx), s_terminal_bytes);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_destroy_waits_for_park_and_repeated_create_cannot_clear_live_storage(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL, *duplicate = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &duplicate));
  fake_esy_os_task_set_delay_hook(NULL);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
  fake_esy_os_task_set_delay_hook(schedule_workers);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

static void spi_attempt_lifecycle_during_callback(void *arg) {
  esy_spi_scheduler_t *gw = arg;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_quiesce(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_spi_scheduler_destroy(gw));
}

void test_spi_callback_excludes_quiesce_and_self_destroy_and_rolls_back_wakeup(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  req->len = 2u;
  req->pre_callback = spi_attempt_lifecycle_during_callback;
  req->pre_callback_arg = gw;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit(gw, req));
  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  uint32_t notifications = task->notify_give_count;
  fake_esy_os_task_set_current(task->task_handle);
  esy_spi_scheduler_test_iteration(gw);
  fake_esy_os_task_set_current(NULL);
  TEST_ASSERT_GREATER_THAN(notifications, task->notify_give_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_quiesce(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_resume(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_spi_storage_alignment_and_bounds_are_enforced(void) {
  struct { char prefix; esy_spi_scheduler_storage_t storage; } nested = {0};
  TEST_ASSERT_EQUAL(0u, (uintptr_t)&nested.storage % _Alignof(esy_block_ring_t));
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &nested.storage, &gw));
  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  req->len = 9u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_spi_scheduler_submit(gw, req));
  req->len = 3u;
  req->transfer_size = ESY_DMA_TRANSFER_SIZE_16;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_spi_scheduler_submit(gw, req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_rejected_convenience_submit_releases_internal_reservation(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint8_t tx = 1;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_spi_scheduler_submit_result_with_arg(&tx, NULL, UINT32_MAX, NULL, NULL, spi_ignore_result, NULL, gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit_result_with_arg(&tx, NULL, 1u, NULL, NULL, spi_ignore_result, NULL, gw));
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}
static void result_done(esy_err_t result, uint8_t *rx, void *arg) {
  (void)arg;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, result);
  TEST_ASSERT_NULL(rx);
  s_spi_post_count++;
}
void test_result_submit_reports_failure_and_requires_drain(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  uint8_t tx[2] = {0}, rx[2];
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_spi_scheduler_submit_result_with_arg(tx, rx, 2, NULL, NULL, NULL, NULL, gw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_spi_scheduler_submit_result_with_arg(tx, rx, UINT32_MAX, NULL, NULL, result_done, NULL, gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit_result_with_arg(tx, rx, 2, NULL, NULL, result_done, NULL, gw));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
  fake_esy_hal_spi_fail_read_after(0, ESY_ERR_IO);
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL(1u, s_spi_post_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}
static esy_err_t complete_from_poll(esy_spi_t *spi) {
  fake_esy_hal_spi_complete_last_dma(spi);
  return ESY_OK;
}
void test_worker_polls_backend_before_waiting_for_terminal_callback(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  uint8_t tx[8] = {0};
  prepared_fixture_t fixture = {0};
  fake_esy_hal_spi_set_transfer_dma_async(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit_prepared_with_arg(tx, sizeof(tx), prepared_begin, prepared_complete, &fixture, gw));
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL(0u, fixture.completed);
  fake_esy_hal_spi_set_poll_hook(&s_spi, complete_from_poll);
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL(1u, fixture.completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

static esy_spi_scheduler_core_config_t make_core_cfg(void) {
  esy_spi_scheduler_config_t os = make_spi_cfg();
  attach_spi_storage(&os);
  esy_spi_scheduler_core_config_t cfg;
  esy_spi_scheduler_core_default_cfg(&cfg);
  cfg.tx_alloc_size = os.tx_alloc_size;
  cfg.rx_alloc_size = os.rx_alloc_size;
  cfg.block_count = os.block_count;
  cfg.static_resources.request_storage = os.static_resources.request_storage;
  cfg.static_resources.request_storage_count = os.static_resources.request_storage_count;
  cfg.static_resources.tx_buffer_storage = os.static_resources.tx_buffer_storage;
  cfg.static_resources.tx_buffer_storage_size = os.static_resources.tx_buffer_storage_size;
  cfg.static_resources.rx_buffer_storage = os.static_resources.rx_buffer_storage;
  cfg.static_resources.rx_buffer_storage_size = os.static_resources.rx_buffer_storage_size;
  return cfg;
}

void test_sized_spi_reservation_checks_capacity_without_claiming_a_request(void) {
  esy_spi_scheduler_core_config_t cfg = make_core_cfg();
  esy_spi_scheduler_t *gw = NULL;
  uint8_t *tx = s_spi_tx_buffers[0];
  uint8_t *rx = s_spi_rx_buffers[0];
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_create(
      &s_spi, &cfg, &s_spi_scheduler_storage, &gw));

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 1u, NULL));
  TEST_ASSERT_NULL(tx);
  TEST_ASSERT_NULL(rx);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_spi_scheduler_get_read_write_buffer_with_arg(NULL, &rx, 1u, gw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, NULL, 1u, gw));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 0u, gw));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 9u, gw));
  TEST_ASSERT_NULL(tx);
  TEST_ASSERT_NULL(rx);

  TEST_ASSERT_EQUAL(ESY_OK,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 8u, gw));
  uint8_t *reserved_tx = tx;
  uint8_t *reserved_rx = rx;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 1u, gw));
  TEST_ASSERT_NULL(tx);
  TEST_ASSERT_NULL(rx);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit_result_with_arg(
      reserved_tx, reserved_rx, 8u, NULL, NULL, spi_ignore_result, NULL, gw));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 1u, gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 8u, gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_spi_scheduler_get_read_write_buffer_with_arg(&tx, &rx, 1u, gw));
}

void test_core_scheduler_limits_each_process_to_one_new_request(void) {
  esy_spi_scheduler_core_config_t cfg = make_core_cfg();
  esy_spi_scheduler_request_t requests[4];
  uint8_t tx[32], rx[32];
  cfg.block_count = 4u;
  cfg.static_resources.request_storage = requests;
  cfg.static_resources.request_storage_count = 4u;
  cfg.static_resources.tx_buffer_storage = tx;
  cfg.static_resources.tx_buffer_storage_size = sizeof(tx);
  cfg.static_resources.rx_buffer_storage = rx;
  cfg.static_resources.rx_buffer_storage_size = sizeof(rx);
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  for (unsigned i = 0u; i < 2u; ++i) {
    esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
    TEST_ASSERT_NOT_NULL(req);
    req->len = 1u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit(gw, req));
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_spi_get_tx_count(&s_spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(2u, fake_esy_hal_spi_get_tx_count(&s_spi));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

void test_core_scheduler_busy_destroy_keeps_dma_until_retired_by_process(void) {
  esy_spi_scheduler_core_config_t cfg = make_core_cfg();
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  fake_esy_hal_spi_set_transfer_dma_async(true);
  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  req->len = 8u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit(gw, req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_NULL(esy_spi_scheduler_get_write_block(gw));
  fake_esy_hal_spi_complete_last_dma(&s_spi);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_spi_scheduler_core_process(gw));
}

void test_core_process_rejects_worker_owned_scheduler_without_consuming_requests(void) {
  esy_spi_scheduler_config_t cfg = make_spi_cfg();
  attach_spi_storage(&cfg);
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg, &s_spi_scheduler_storage, &gw));
  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  req->len = 1u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit(gw, req));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_spi_get_tx_count(&s_spi));
  esy_spi_scheduler_test_iteration(gw);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

static void retained_prepared_complete(esy_err_t result, void *arg) {
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(&s_spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_tx(&s_spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_rx(&s_spi));
  prepared_complete(result, arg);
}

static void process_retained_scheduler(esy_spi_scheduler_t *gw, bool worker) {
  if (worker) esy_spi_scheduler_test_iteration(gw);
  else TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
}

void test_failed_submit_keeps_prepared_resources_and_slot_until_poll_retires_it(void) {
  for (unsigned worker = 0u; worker < 2u; ++worker) {
    setUp();
    esy_spi_scheduler_t *gw = NULL;
    if (worker != 0u) {
      esy_spi_scheduler_config_t cfg = make_spi_cfg();
      attach_spi_storage(&cfg);
      TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&s_spi, &cfg,
          &s_spi_scheduler_storage, &gw));
    } else {
      esy_spi_scheduler_core_config_t cfg = make_core_cfg();
      TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_create(&s_spi, &cfg,
          &s_spi_scheduler_storage, &gw));
    }
    uint8_t pixels[8] = {0};
    prepared_fixture_t fixture = {0};
    fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_IO);
    fake_esy_hal_spi_set_failed_submit_retains_borrow(true);
    fake_esy_hal_spi_set_poll_result(ESY_ERR_BUSY);
    fake_esy_hal_spi_set_abort_result(ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit_prepared_with_arg(
        pixels, sizeof(pixels), prepared_begin, retained_prepared_complete,
        &fixture, gw));
    process_retained_scheduler(gw, worker != 0u);
    TEST_ASSERT_EQUAL_UINT(1u, fixture.prepared);
    TEST_ASSERT_EQUAL_UINT(0u, fixture.completed);
    TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&s_spi));
    TEST_ASSERT_EQUAL_PTR(pixels, fake_esy_hal_spi_get_borrowed_tx(&s_spi));
    TEST_ASSERT_NULL(esy_spi_scheduler_get_write_block(gw));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_quiesce(gw));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
    for (unsigned retry = 0u; retry < 2u; ++retry) {
      process_retained_scheduler(gw, worker != 0u);
      TEST_ASSERT_EQUAL_UINT(0u, fixture.completed);
      TEST_ASSERT_EQUAL_PTR(pixels, fake_esy_hal_spi_get_borrowed_tx(&s_spi));
      TEST_ASSERT_NULL(esy_spi_scheduler_get_write_block(gw));
      TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
    }
    TEST_ASSERT_EQUAL_UINT(3u, fake_esy_hal_spi_get_poll_count(&s_spi));
    TEST_ASSERT_EQUAL_UINT(2u, fake_esy_hal_spi_get_abort_count(&s_spi));
    fake_esy_hal_spi_set_poll_result(ESY_OK);
    process_retained_scheduler(gw, worker != 0u);
    TEST_ASSERT_EQUAL_UINT(1u, fixture.prepared);
    TEST_ASSERT_EQUAL_UINT(1u, fixture.completed);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, fixture.completion_result);
    TEST_ASSERT_EQUAL_UINT(2u, fake_esy_hal_spi_get_abort_count(&s_spi));
    /* Retirement restores the only usable slot; no buffer was recycled early. */
    TEST_ASSERT_NOT_NULL(esy_spi_scheduler_get_write_block(gw));
    fake_esy_hal_spi_complete_last_dma(&s_spi);
    process_retained_scheduler(gw, worker != 0u);
    TEST_ASSERT_EQUAL_UINT(1u, fixture.completed);
    TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
  }
}

void test_failed_busy_unsupported_submit_never_falls_back_and_finishes_after_abort(void) {
  esy_spi_scheduler_core_config_t cfg = make_core_cfg();
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_create(&s_spi, &cfg,
      &s_spi_scheduler_storage, &gw));
  uint8_t pixels[8] = {0};
  prepared_fixture_t fixture = {0};
  fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_NOT_SUPPORTED);
  fake_esy_hal_spi_set_failed_submit_retains_borrow(true);
  fake_esy_hal_spi_set_poll_result(ESY_ERR_BUSY);
  fake_esy_hal_spi_set_abort_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit_prepared_with_arg(
      pixels, sizeof(pixels), prepared_begin, retained_prepared_complete,
      &fixture, gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL_UINT(0u, fixture.completed);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_spi_get_tx_count(&s_spi));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
  fake_esy_hal_spi_set_abort_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL_UINT(1u, fixture.completed);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, fixture.completion_result);
  TEST_ASSERT_EQUAL_UINT(2u, fake_esy_hal_spi_get_abort_count(&s_spi));
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(&s_spi));
  fake_esy_hal_spi_complete_last_dma(&s_spi);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL_UINT(1u, fixture.completed);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_spi_get_tx_count(&s_spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_create(&s_spi, &cfg,
      &s_spi_scheduler_storage, &gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}

static void retained_result_complete(esy_err_t result, uint8_t *rx, void *arg) {
  TEST_ASSERT_NULL(rx);
  retained_prepared_complete(result, arg);
}

void test_failed_full_duplex_submit_keeps_both_buffers_until_successful_abort(void) {
  esy_spi_scheduler_core_config_t cfg = make_core_cfg();
  esy_spi_scheduler_t *gw = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_create(&s_spi, &cfg,
      &s_spi_scheduler_storage, &gw));
  uint8_t tx[8] = {0}, rx[8] = {0};
  prepared_fixture_t fixture = {0};
  fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_IO);
  fake_esy_hal_spi_set_failed_submit_retains_borrow(true);
  fake_esy_hal_spi_set_poll_result(ESY_ERR_BUSY);
  fake_esy_hal_spi_set_abort_result(ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_submit_result_with_arg(tx, rx,
      sizeof(tx), NULL, NULL, retained_result_complete, &fixture, gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL_UINT(0u, fixture.completed);
  TEST_ASSERT_EQUAL_PTR(tx, fake_esy_hal_spi_get_borrowed_tx(&s_spi));
  TEST_ASSERT_EQUAL_PTR(rx, fake_esy_hal_spi_get_borrowed_rx(&s_spi));
  TEST_ASSERT_NULL(esy_spi_scheduler_get_write_block(gw));
  fake_esy_hal_spi_set_abort_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL_UINT(1u, fixture.completed);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, fixture.completion_result);
  fake_esy_hal_spi_complete_last_dma(&s_spi);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_core_process(gw));
  TEST_ASSERT_EQUAL_UINT(1u, fixture.completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
}
