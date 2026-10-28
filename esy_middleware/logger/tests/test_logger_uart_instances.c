/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <string.h>
#include "esy_middleware/logger/esy_logger_backend_uart.h"
#include "esy_logger_backend_uart_internal.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_uart.h"
#include "fake_esy_hal_dma.h"
#include "fake_esy_hal_system.h"

#define TEST_BLOCK_SIZE 32u
#define TEST_BLOCK_COUNT 2u
#define TEST_STACK_SIZE 256u
#define TEST_INSTANCE_COUNT 2u

typedef struct {
  esy_logger_backend_t backend;
  esy_uart_t uart;
  ESY_LOGGER_BACKEND_UART_STATIC_STORAGE(os_storage, TEST_BLOCK_COUNT,
                                        TEST_BLOCK_SIZE, TEST_STACK_SIZE);
  esy_logger_backend_uart_static_resources_t os_resources;
  esy_logger_backend_uart_config_t os_config;
  ESY_LOGGER_BACKEND_UART_CORE_STATIC_STORAGE(core_storage, TEST_BLOCK_COUNT,
                                             TEST_BLOCK_SIZE);
  esy_logger_backend_uart_core_resources_t core_resources;
  esy_logger_backend_uart_core_config_t core_config;
  fake_esy_os_task_entry_t *worker;
} uart_instance_fixture_t;

typedef struct {
  uart_instance_fixture_t instances[TEST_INSTANCE_COUNT];
} uart_instances_test_ctx_t;

static uart_instances_test_ctx_t s_ctx;

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_uart_reset();
  fake_esy_hal_dma_reset();
  fake_esy_hal_system_reset();
  for (unsigned i = 0u; i < TEST_INSTANCE_COUNT; ++i) {
    uart_instance_fixture_t *instance = &s_ctx.instances[i];
    esy_uart_config_t uart_cfg;
    esy_hal_uart_default_cfg(&uart_cfg);
    uart_cfg.phys_index = (uint8_t)i;
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&instance->uart, &uart_cfg));
    esy_logger_backend_uart_default_cfg(&instance->os_config);
    ESY_LOGGER_BACKEND_UART_BIND_STATIC_RESOURCES(&instance->os_resources,
                                                 instance->os_storage);
    instance->os_config.uart = &instance->uart;
    instance->os_config.block_size = TEST_BLOCK_SIZE;
    instance->os_config.pool_block_count = TEST_BLOCK_COUNT;
    instance->os_config.dma_short_threshold = 0u;
    instance->os_config.static_resources = &instance->os_resources;
    esy_logger_backend_uart_core_default_cfg(&instance->core_config);
    ESY_LOGGER_BACKEND_UART_CORE_BIND_STATIC_RESOURCES(&instance->core_resources,
                                                      instance->core_storage);
    instance->core_config.uart = &instance->uart;
    instance->core_config.block_size = TEST_BLOCK_SIZE;
    instance->core_config.pool_block_count = TEST_BLOCK_COUNT;
    instance->core_config.dma_short_threshold = 0u;
    instance->core_config.static_resources = &instance->core_resources;
    fake_esy_hal_uart_set_send_dma_result(&instance->uart, ESY_OK);
  }
}

void tearDown(void) {
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_critical_nesting());
}

static void init_instances(bool with_os) {
  for (unsigned i = 0u; i < TEST_INSTANCE_COUNT; ++i) {
    uart_instance_fixture_t *instance = &s_ctx.instances[i];
    TEST_ASSERT_EQUAL(ESY_OK, with_os ?
        esy_logger_backend_uart_init(&instance->backend, &instance->os_config) :
        esy_logger_backend_uart_core_init(&instance->backend, &instance->core_config));
    if (with_os) instance->worker = fake_esy_os_task_get_last_created();
  }
  TEST_ASSERT_TRUE(s_ctx.instances[0].backend.ctx != s_ctx.instances[1].backend.ctx);
}

static uint8_t *borrow_block(esy_logger_backend_t *backend) {
  uint8_t *buffer = NULL;
  size_t capacity = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, backend->acquire_buffer(backend, &buffer, &capacity));
  TEST_ASSERT_EQUAL_UINT(TEST_BLOCK_SIZE, capacity);
  return buffer;
}

static void queue_text(unsigned instance_index, const char *text) {
  esy_logger_backend_t *backend = &s_ctx.instances[instance_index].backend;
  uint8_t *buffer = borrow_block(backend);
  memcpy(buffer, text, strlen(text));
  TEST_ASSERT_EQUAL(ESY_OK, backend->submit_buffer(backend, buffer, strlen(text)));
}

static void kick(unsigned instance_index) {
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(
      &s_ctx.instances[instance_index].backend));
}

static void close_instances(void) {
  for (unsigned i = 0u; i < TEST_INSTANCE_COUNT; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.instances[i].backend));
  }
}

void test_dual_core_uart_dma_completion_recycles_only_its_own_pool(void) {
  init_instances(false);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_create_static_count());
  queue_text(0u, "first uart");
  queue_text(1u, "second uart");
  kick(0u);
  kick(1u);
  for (unsigned i = 0u; i < TEST_INSTANCE_COUNT; ++i) {
    fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.instances[i].uart);
    TEST_ASSERT_EQUAL_PTR(s_ctx.instances[i].backend.ctx, slot->last_dma_callback_arg);
    TEST_ASSERT_EQUAL_PTR(s_ctx.instances[i].backend.ctx, slot->last_dma_error_callback_arg);
  }
  TEST_ASSERT_EQUAL_MEMORY("first uart", fake_esy_hal_uart_find(&s_ctx.instances[0].uart)->last_dma_src, 10u);
  TEST_ASSERT_EQUAL_MEMORY("second uart", fake_esy_hal_uart_find(&s_ctx.instances[1].uart)->last_dma_src, 11u);

  uint8_t *remaining[TEST_INSTANCE_COUNT];
  for (unsigned i = 0u; i < TEST_INSTANCE_COUNT; ++i) {
    esy_logger_backend_t *backend = &s_ctx.instances[i].backend;
    remaining[i] = borrow_block(backend);
    uint8_t *extra = NULL;
    size_t capacity = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, backend->acquire_buffer(backend, &extra, &capacity));
  }
  fake_esy_hal_uart_complete_last_dma(&s_ctx.instances[0].uart);
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.instances[0].backend));
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.instances[1].backend));
  uint8_t *released = borrow_block(&s_ctx.instances[0].backend);
  uint8_t *extra = NULL;
  size_t capacity = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.instances[1].backend.acquire_buffer(
      &s_ctx.instances[1].backend, &extra, &capacity));
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.instances[0].backend.submit_buffer(
      &s_ctx.instances[0].backend, released, 0u));
  fake_esy_hal_uart_fail_last_dma(&s_ctx.instances[1].uart);
  for (unsigned i = 0u; i < TEST_INSTANCE_COUNT; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.instances[i].backend.submit_buffer(
        &s_ctx.instances[i].backend, remaining[i], 0u));
  }
  close_instances();
}

void test_dual_os_uart_workers_and_shutdown_are_independent(void) {
  init_instances(true);
  uart_instance_fixture_t *first = &s_ctx.instances[0];
  uart_instance_fixture_t *second = &s_ctx.instances[1];
  TEST_ASSERT_TRUE(first->worker->task_handle != second->worker->task_handle);
  TEST_ASSERT_EQUAL_PTR(first->backend.ctx, first->worker->arg);
  TEST_ASSERT_EQUAL_PTR(second->backend.ctx, second->worker->arg);
  queue_text(0u, "first");
  TEST_ASSERT_EQUAL_UINT(1u, first->worker->notify_give_count);
  TEST_ASSERT_EQUAL_UINT(0u, second->worker->notify_give_count);
  queue_text(1u, "second");
  kick(0u);
  kick(1u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&first->backend));
  fake_esy_hal_uart_complete_last_dma(&first->uart);
  TEST_ASSERT_EQUAL_UINT(2u, first->worker->notify_give_count);
  TEST_ASSERT_EQUAL_UINT(1u, second->worker->notify_give_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&first->backend));
  TEST_ASSERT_TRUE(first->worker->deleted);
  TEST_ASSERT_FALSE(second->worker->deleted);
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_is_dma_busy(&second->backend));
  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  fake_esy_hal_uart_complete_last_dma(&second->uart);
  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_UINT(1u, second->worker->notify_give_from_isr_count);
  queue_text(1u, "still running");
  kick(1u);
  TEST_ASSERT_EQUAL_MEMORY("still running", fake_esy_hal_uart_find(&second->uart)->last_dma_src, 13u);
  fake_esy_hal_uart_complete_last_dma(&second->uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_init(&first->backend, &first->os_config));
  queue_text(0u, "restarted");
  kick(0u);
  fake_esy_hal_uart_complete_last_dma(&first->uart);
  close_instances();
}

void test_quiescing_one_uart_preserves_its_queue_while_other_uart_sends(void) {
  init_instances(false);
  queue_text(0u, "held");
  queue_text(1u, "running");
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_quiesce(&s_ctx.instances[0].backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.instances[0].backend, 1u));
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_uart_find(&s_ctx.instances[0].uart)->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.instances[1].backend, 1u));
  fake_esy_hal_uart_complete_last_dma(&s_ctx.instances[1].uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.instances[1].backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_resume(&s_ctx.instances[0].backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.instances[0].backend, 1u));
  TEST_ASSERT_EQUAL_MEMORY("held", fake_esy_hal_uart_find(&s_ctx.instances[0].uart)->last_dma_src, 4u);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.instances[0].uart);
  close_instances();
}

void test_outstanding_buffer_lease_blocks_only_its_own_uart_shutdown_and_reinit(void) {
  init_instances(false);
  uart_instance_fixture_t *first = &s_ctx.instances[0];
  uart_instance_fixture_t *second = &s_ctx.instances[1];
  uint8_t *borrowed = borrow_block(&first->backend);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, second->backend.submit_buffer(&second->backend, borrowed, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&first->backend));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_logger_backend_uart_core_init(&first->backend, &first->core_config));
  queue_text(1u, "unaffected");
  kick(1u);
  fake_esy_hal_uart_complete_last_dma(&second->uart);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, first->backend.submit_buffer(&first->backend, borrowed, 0u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&first->backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_init(&first->backend, &first->core_config));
  queue_text(0u, "new lease");
  kick(0u);
  fake_esy_hal_uart_complete_last_dma(&first->uart);
  close_instances();
}

void test_live_context_storage_reuse_is_rejected_and_succeeds_after_owner_deinit(void) {
  uart_instance_fixture_t *first = &s_ctx.instances[0];
  uart_instance_fixture_t *second = &s_ctx.instances[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_init(&first->backend, &first->core_config));
  second->core_resources.context_storage = first->core_resources.context_storage;
  second->core_storage.pool_index_stacks[0] = UINT32_C(0xA5A5A5A5);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_logger_backend_uart_core_init(&second->backend, &second->core_config));
  TEST_ASSERT_NULL(second->backend.ctx);
  TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xA5A5A5A5), second->core_storage.pool_index_stacks[0]);
  queue_text(0u, "owner alive");
  kick(0u);
  fake_esy_hal_uart_complete_last_dma(&first->uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&first->backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_init(&second->backend, &second->core_config));
  queue_text(1u, "new owner");
  kick(1u);
  fake_esy_hal_uart_complete_last_dma(&second->uart);
  close_instances();
}

void test_foreign_copied_and_swapped_uart_handles_do_not_access_another_context(void) {
  init_instances(false);
  esy_logger_backend_t copy = s_ctx.instances[0].backend;
  uint8_t *buffer = NULL;
  size_t capacity = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, copy.acquire_buffer(&copy, &buffer, &capacity));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_logger_backend_uart_deinit(&copy));
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_try_kick_one(&copy));
  copy.ctx = s_ctx.instances[1].backend.ctx;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_logger_backend_uart_quiesce(&copy));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_logger_backend_uart_resume(&copy));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_logger_backend_uart_core_process(&copy, 1u));
  esy_logger_backend_t foreign = {.ctx = (void *)(uintptr_t)1u};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_logger_backend_uart_deinit(&foreign));
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_initialized(&foreign));
  queue_text(0u, "first intact");
  queue_text(1u, "second intact");
  kick(0u);
  kick(1u);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.instances[0].uart);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.instances[1].uart);
  close_instances();
}

void test_second_uart_worker_creation_failure_preserves_first_and_allows_retry(void) {
  uart_instance_fixture_t *first = &s_ctx.instances[0];
  uart_instance_fixture_t *second = &s_ctx.instances[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_init(&first->backend, &first->os_config));
  fake_esy_os_task_set_create_static_failure(2u, ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_logger_backend_uart_init(&second->backend, &second->os_config));
  TEST_ASSERT_NULL(second->backend.ctx);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&second->backend));
  queue_text(0u, "first survives");
  kick(0u);
  fake_esy_hal_uart_complete_last_dma(&first->uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_init(&second->backend, &second->os_config));
  queue_text(1u, "second retries");
  kick(1u);
  fake_esy_hal_uart_complete_last_dma(&second->uart);
  close_instances();
}

void test_uart_context_storage_is_required_and_alignment_is_checked_before_access(void) {
  uart_instance_fixture_t *first = &s_ctx.instances[0];
  first->core_resources.context_storage = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_logger_backend_uart_core_init(&first->backend, &first->core_config));
  first->core_resources.context_storage = (esy_logger_backend_uart_context_storage_t *)
      (first->core_storage.context_storage.bytes + 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_logger_backend_uart_core_init(&first->backend, &first->core_config));
  first->core_resources.context_storage = &first->core_storage.context_storage;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_init(&first->backend, &first->core_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&first->backend));
}

void test_failed_dma_retirement_on_one_uart_keeps_other_uart_running(void) {
  init_instances(false);
  uart_instance_fixture_t *first = &s_ctx.instances[0];
  uart_instance_fixture_t *second = &s_ctx.instances[1];
  fake_esy_hal_uart_slot_t *failed = fake_esy_hal_uart_find(&first->uart);
  failed->send_dma_result = ESY_ERR_IO;
  failed->retain_failed_submission = true;
  failed->abort_result = ESY_ERR_IO;
  queue_text(0u, "retained");
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_try_kick_one(&first->backend));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&first->backend));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_logger_backend_uart_core_process(&first->backend, 1u));
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_is_dma_busy(&first->backend));
  queue_text(1u, "independent");
  kick(1u);
  fake_esy_hal_uart_complete_last_dma(&second->uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&second->backend));
  failed->abort_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&first->backend, 1u));
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&first->backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&first->backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_init(&first->backend, &first->core_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&first->backend));
}
