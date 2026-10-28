/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "unity.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "esy_middleware/logger/esy_logger.h"
#include "esy_middleware/logger/esy_logger_backend_uart.h"
#include "esy_middleware/logger/esy_logger_backend_printf.h"
#include "esy_middleware/logger/esy_logger_backend_rtt.h"
#include "esy_logger_backend_uart_internal.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_uart.h"
#include "fake_esy_hal_dma.h"
#include "fake_esy_hal_system.h"
#include "fake_segger_rtt.h"

#define TEST_BLOCK_SIZE 32u
#define TEST_BLOCK_COUNT 4u
#define TEST_STACK_SIZE 256u

typedef struct {
  esy_logger_backend_t backend;
  esy_logger_backend_binding_t binding;
  esy_logger_backend_binding_t replacement_binding;
  esy_uart_t uart;
  ESY_LOGGER_BACKEND_UART_STATIC_STORAGE(uart_storage, TEST_BLOCK_COUNT, TEST_BLOCK_SIZE, TEST_STACK_SIZE);
  esy_logger_backend_uart_static_resources_t uart_resources;
  esy_logger_backend_uart_config_t uart_config;
  ESY_LOGGER_BACKEND_PRINTF_STATIC_STORAGE(printf_storage, 2u, TEST_BLOCK_COUNT, TEST_BLOCK_SIZE, TEST_STACK_SIZE);
  esy_logger_backend_printf_static_resources_t printf_resources;
  esy_logger_backend_printf_config_t printf_config;
  ESY_LOGGER_BACKEND_RTT_STATIC_STORAGE(rtt_storage, TEST_BLOCK_SIZE);
  esy_logger_backend_rtt_static_resources_t rtt_resources;
  esy_logger_backend_rtt_config_t rtt_config;
  jmp_buf task_exit;
  esy_err_t close_result;
  esy_err_t bind_result;
  esy_err_t detach_result;
  bool close_on_write;
  bool in_critical;
  FILE *saved_stdout;
  FILE *capture_stream;
  char output[2048];
  size_t output_size;
  uint8_t *borrowed;
  size_t borrowed_size;
} logger_test_ctx_t;

static logger_test_ctx_t s_ctx;

static esy_err_t bind_test_backend(esy_logger_backend_binding_t *binding) {
  *binding = (esy_logger_backend_binding_t){
      .backend = &s_ctx.backend, .filter_level = ESY_LOG_LEVEL_VERBOSE};
  const esy_logger_static_resources_t resources = {
      .bindings = binding, .binding_count = 1u};
  const esy_logger_config_t config = {
      .static_resources = &resources, .filter_level = ESY_LOG_LEVEL_INFO};
  return esy_logger_init(&config);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_uart_reset();
  fake_esy_hal_dma_reset();
  fake_esy_hal_system_reset();
  fake_segger_rtt_reset();
  esy_uart_config_t cfg;
  esy_hal_uart_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_uart_init(&s_ctx.uart, &cfg));
  esy_logger_backend_uart_default_cfg(&s_ctx.uart_config);
  ESY_LOGGER_BACKEND_UART_BIND_STATIC_RESOURCES(&s_ctx.uart_resources, s_ctx.uart_storage);
  s_ctx.uart_config.uart = &s_ctx.uart;
  s_ctx.uart_config.static_resources = &s_ctx.uart_resources;
  s_ctx.uart_config.block_size = TEST_BLOCK_SIZE;
  s_ctx.uart_config.pool_block_count = TEST_BLOCK_COUNT;
  s_ctx.uart_config.dma_short_threshold = 0u;
  esy_logger_backend_printf_default_cfg(&s_ctx.printf_config);
  ESY_LOGGER_BACKEND_PRINTF_BIND_STATIC_RESOURCES(&s_ctx.printf_resources, s_ctx.printf_storage);
  s_ctx.printf_config.static_resources = &s_ctx.printf_resources;
  s_ctx.printf_config.block_size = TEST_BLOCK_SIZE;
  s_ctx.printf_config.pool_block_count = TEST_BLOCK_COUNT;
  esy_logger_backend_rtt_default_cfg(&s_ctx.rtt_config);
  ESY_LOGGER_BACKEND_RTT_BIND_STATIC_RESOURCES(&s_ctx.rtt_resources, s_ctx.rtt_storage);
  s_ctx.rtt_config.static_resources = &s_ctx.rtt_resources;
}

void tearDown(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_detach(&s_ctx.backend));
  if (s_ctx.saved_stdout != NULL) stdout = s_ctx.saved_stdout;
  if (s_ctx.capture_stream != NULL) fclose(s_ctx.capture_stream);
  fake_esy_os_task_set_notify_take_hook(NULL);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_critical_nesting());
}

static void queue_block(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.backend.acquire_buffer(&s_ctx.backend, &s_ctx.borrowed, &s_ctx.borrowed_size));
  memcpy(s_ctx.borrowed, "0123456789abcdef", 16u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.backend.submit_buffer(&s_ctx.backend, s_ctx.borrowed, 16u));
}

static void exit_on_wait(void) { longjmp(s_ctx.task_exit, 1); }

static void run_sender_until_wait(void) {
  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  fake_esy_os_task_set_current(task->task_handle);
  fake_esy_os_task_set_notify_take_hook(exit_on_wait);
  fake_esy_os_task_set_park_hook(exit_on_wait);
  if (setjmp(s_ctx.task_exit) == 0) task->entry(task->arg);
  fake_esy_os_task_set_notify_take_hook(NULL);
  fake_esy_os_task_set_park_hook(NULL);
}

static void recover_after_delay(uint32_t ms) {
  TEST_ASSERT_TRUE(ms > 0u);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->send_dma_result = ESY_ERR_NOT_SUPPORTED;
  slot->override_send_result = false;
}

void test_dma_busy_and_state_back_off_then_retry_without_another_log(void) {
  for (unsigned i = 0u; i < 2u; ++i) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
    queue_block();
    fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, i == 0u ? ESY_ERR_BUSY : ESY_ERR_STATE);
    fake_esy_os_task_set_delay_hook(recover_after_delay);
    uint32_t before = fake_esy_os_task_get_total_delay_ms();
    run_sender_until_wait();
    TEST_ASSERT_TRUE(fake_esy_os_task_get_total_delay_ms() > before);
    TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.backend));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  }
}

void test_short_send_busy_and_partial_write_retry_without_new_notifications(void) {
  s_ctx.uart_config.dma_short_threshold = TEST_BLOCK_SIZE;
  for (unsigned i = 0u; i < 2u; ++i) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
    queue_block();
    fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
    slot->override_send_result = true;
    slot->send_result = i == 0u ? ESY_ERR_BUSY : 4;
    fake_esy_os_task_set_delay_hook(recover_after_delay);
    run_sender_until_wait();
    if (i != 0u) {
      TEST_ASSERT_EQUAL_UINT(12u, slot->last_tx_len);
      TEST_ASSERT_EQUAL_MEMORY("456789abcdef", slot->last_tx, 12u);
    }
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  }
}

void test_dma_error_releases_busy_state_and_callback_uses_actual_context(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
  for (unsigned isr = 0u; isr < 2u; ++isr) {
    queue_block();
    TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&s_ctx.backend));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_logger_backend_uart_quiesce(&s_ctx.backend));
    fake_esy_hal_system_get_ctx()->is_in_isr = isr != 0u;
    fake_esy_hal_uart_fail_last_dma(&s_ctx.uart);
    fake_esy_hal_system_get_ctx()->is_in_isr = false;
    TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.backend));
  }
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_os_task_get_last_created()->notify_give_from_isr_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_quiesce(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_logger_backend_uart_quiesce(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_resume(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_logger_backend_uart_resume(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

static void close_during_send(esy_uart_t *uart) {
  (void)uart;
  s_ctx.in_critical |= fake_esy_os_task_get_critical_nesting() != 0u;
  s_ctx.close_result = esy_logger_backend_uart_deinit(&s_ctx.backend);
}

void test_deinit_during_blocking_send_preserves_task_and_io_until_return(void) {
  s_ctx.uart_config.dma_short_threshold = TEST_BLOCK_SIZE;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  queue_block();
  fake_esy_hal_uart_find(&s_ctx.uart)->send_hook = close_during_send;
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.close_result);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_FALSE(s_ctx.in_critical);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_os_task_get_delete_count());
}

static void close_during_notification(esy_os_task_t *task) {
  (void)task;
  s_ctx.close_result = esy_logger_backend_uart_deinit(&s_ctx.backend);
}

void test_deinit_cannot_overtake_completion_notification(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  queue_block();
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&s_ctx.backend));
  fake_esy_os_task_set_notify_give_hook(close_during_notification);
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  fake_esy_os_task_set_notify_give_hook(NULL);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.close_result);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

static void synchronous_dma(esy_uart_t *uart) {
  fake_esy_hal_uart_complete_last_dma(uart);
  close_during_send(uart);
}

void test_synchronous_dma_completion_keeps_send_call_lease(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  queue_block();
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->send_dma_result = ESY_OK;
  slot->send_dma_hook = synchronous_dma;
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.close_result);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_last_created()->notify_give_from_isr_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

void test_all_backends_preserve_borrowed_buffers_during_stop_and_support_reinit(void) {
  for (unsigned kind = 0u; kind < 3u; ++kind) {
    for (unsigned cycle = 0u; cycle < 2u; ++cycle) {
      esy_err_t init = kind == 0u ? esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config) :
          kind == 1u ? esy_logger_backend_printf_init(&s_ctx.backend, &s_ctx.printf_config) :
                       esy_logger_backend_rtt_init(&s_ctx.backend, &s_ctx.rtt_config);
      TEST_ASSERT_EQUAL_INT(ESY_OK, init);
      TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.backend.acquire_buffer(&s_ctx.backend, &s_ctx.borrowed, &s_ctx.borrowed_size));
      esy_err_t (*close)(esy_logger_backend_t *) = kind == 0u ? esy_logger_backend_uart_deinit :
          kind == 1u ? esy_logger_backend_printf_deinit : esy_logger_backend_rtt_deinit;
      TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, close(&s_ctx.backend));
      uint8_t *other = NULL;
      size_t size = 0u;
      TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.backend.acquire_buffer(&s_ctx.backend, &other, &size));
      TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.backend.submit_buffer(&s_ctx.backend, s_ctx.borrowed, 0u));
      TEST_ASSERT_EQUAL_INT(ESY_OK, close(&s_ctx.backend));
      TEST_ASSERT_EQUAL_INT(ESY_OK, close(&s_ctx.backend));
      TEST_ASSERT_NULL(s_ctx.backend.ctx);
    }
  }
}

static void reenter_frontend(void) {
  s_ctx.bind_result = bind_test_backend(&s_ctx.replacement_binding);
  s_ctx.detach_result = esy_logger_detach(&s_ctx.backend);
  s_ctx.close_result = esy_logger_backend_rtt_deinit(&s_ctx.backend);
  esy_logger_set_filter_level(ESY_LOG_LEVEL_VERBOSE);
  esy_log("nested", ESY_LOG_LEVEL_INFO, "must drop");
}

void test_frontend_rebind_detach_deinit_and_reentrant_logging_preserve_transaction(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_init(&s_ctx.backend, &s_ctx.rtt_config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&s_ctx.binding));
  fake_segger_rtt_get_state()->write_hook = reenter_frontend;
  esy_log("A", ESY_LOG_LEVEL_INFO, "abcdefghijklmnopqrstuvwxyz0123456789");
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.bind_result);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.detach_result);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.close_result);
  TEST_ASSERT_EQUAL_MEMORY("[A][I] abcdefghijklmnopqrstuvwxyz0123456789\n", fake_segger_rtt_get_state()->capture, 44u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_deinit(&s_ctx.backend));
  size_t writes = fake_segger_rtt_get_state()->write_count;
  esy_log("A", ESY_LOG_LEVEL_INFO, "detached");
  TEST_ASSERT_EQUAL_UINT(writes, fake_segger_rtt_get_state()->write_count);
}

static void close_rtt_during_write(void) {
  s_ctx.close_result = esy_logger_backend_rtt_deinit(&s_ctx.backend);
  s_ctx.in_critical |= fake_esy_os_task_get_critical_nesting() != 0u;
}

void test_rtt_raw_write_holds_scratch_lease_until_provider_returns(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_init(&s_ctx.backend, &s_ctx.rtt_config));
  fake_segger_rtt_get_state()->write_hook = close_rtt_during_write;
  queue_block();
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.close_result);
  TEST_ASSERT_FALSE(s_ctx.in_critical);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_deinit(&s_ctx.backend));
}

/* Host-only stdio capture executes the real printf task, including its I/O
 * lease. The cookie callback can model deinit on a peer while fwrite runs. */
static ssize_t capture_write(void *cookie, const char *data, size_t len) {
  logger_test_ctx_t *ctx = cookie;
  ctx->in_critical |= fake_esy_os_task_get_critical_nesting() != 0u ||
      fake_esy_hal_system_get_ctx()->irq_status != 0u;
  if (ctx->close_on_write) ctx->close_result = esy_logger_backend_printf_deinit(&ctx->backend);
  if (ctx->output_size + len >= sizeof(ctx->output)) return -1;
  memcpy(ctx->output + ctx->output_size, data, len);
  ctx->output_size += len;
  ctx->output[ctx->output_size] = '\0';
  return (ssize_t)len;
}

static void begin_capture(void) {
  if (s_ctx.capture_stream == NULL) {
    cookie_io_functions_t io = {.write = capture_write};
    s_ctx.capture_stream = fopencookie(&s_ctx, "w", io);
    TEST_ASSERT_NOT_NULL(s_ctx.capture_stream);
    (void)setvbuf(s_ctx.capture_stream, NULL, _IONBF, 0u);
  }
  s_ctx.saved_stdout = stdout;
  stdout = s_ctx.capture_stream;
}

static void capture_sender(void) {
  begin_capture();
  run_sender_until_wait();
  stdout = s_ctx.saved_stdout;
}

static void migrate_on_submit(esy_os_task_t *task) {
  (void)task;
  fake_esy_hal_system_get_ctx()->core_id ^= 1u;
}

void test_printf_orders_long_lines_across_cores_and_producer_migration(void) {
  fake_esy_hal_system_get_ctx()->core_count = 2u;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_init(&s_ctx.backend, &s_ctx.printf_config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&s_ctx.binding));
  esy_log("A", ESY_LOG_LEVEL_INFO, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  fake_esy_hal_system_get_ctx()->core_id = 1u;
  esy_log("B", ESY_LOG_LEVEL_INFO, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  fake_esy_os_task_set_notify_give_hook(migrate_on_submit);
  esy_log("C", ESY_LOG_LEVEL_INFO, "cccccccccccccccccccccccccccccccccccccccc");
  fake_esy_os_task_set_notify_give_hook(NULL);
  capture_sender();
  TEST_ASSERT_EQUAL_STRING("[A][I] aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n[B][I] bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n[C][I] cccccccccccccccccccccccccccccccccccccccc\n", s_ctx.output);
  TEST_ASSERT_FALSE(s_ctx.in_critical);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&s_ctx.backend));
}

void test_printf_fifo_order_survives_sequence_wrap(void) {
  fake_esy_hal_system_get_ctx()->core_count = 2u;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_init(&s_ctx.backend, &s_ctx.printf_config));
  /* Advance via public submissions, without exposing private counters. */
  for (uint32_t i = 0u; i < UINT16_MAX - 1u; ++i) {
    queue_block();
    capture_sender();
    s_ctx.output_size = 0u;
  }
  /* The older ring is visited second by the consumer; the third block wraps. */
  fake_esy_hal_system_get_ctx()->core_id = 1u;
  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&s_ctx.binding));
  esy_log("A", ESY_LOG_LEVEL_INFO, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  fake_esy_hal_system_get_ctx()->core_id = 0u;
  esy_log("B", ESY_LOG_LEVEL_INFO, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  capture_sender();
  TEST_ASSERT_EQUAL_STRING("[A][I] aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n[B][I] bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n", s_ctx.output);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&s_ctx.backend));
}

void test_printf_deinit_waits_for_fwrite_and_does_not_delete_active_task(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_init(&s_ctx.backend, &s_ctx.printf_config));
  queue_block();
  s_ctx.close_on_write = true;
  capture_sender();
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.close_result);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_delete_count());
  TEST_ASSERT_FALSE(s_ctx.in_critical);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&s_ctx.backend));
}

void test_init_failure_cleanup_retry_and_public_self_argument(void) {
  fake_esy_os_task_set_create_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_logger_backend_printf_init(&s_ctx.backend, &s_ctx.printf_config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&s_ctx.backend));
  s_ctx.rtt_resources.buffer_size = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_backend_rtt_init(&s_ctx.backend, &s_ctx.rtt_config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_deinit(&s_ctx.backend));
  s_ctx.rtt_resources.buffer_size = TEST_BLOCK_SIZE;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_init(&s_ctx.backend, &s_ctx.rtt_config));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.backend.acquire_buffer(s_ctx.backend.ctx, &s_ctx.borrowed, &s_ctx.borrowed_size));
  TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.backend.acquire_buffer(&s_ctx.backend, &s_ctx.borrowed, &s_ctx.borrowed_size));
  TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.backend.submit_buffer(&s_ctx.backend, s_ctx.borrowed, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_deinit(&s_ctx.backend));
  fake_esy_os_task_set_create_result(ESY_OK);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_init(&s_ctx.backend, &s_ctx.printf_config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&s_ctx.backend));
}

static void init_poll_uart(uint32_t threshold) {
  esy_logger_backend_uart_core_resources_t resources = {
    .context_storage = &s_ctx.uart_storage.context_storage,
    .pool_buffers = s_ctx.uart_storage.pool_buffers,
    .pool_buffer_size = sizeof(s_ctx.uart_storage.pool_buffers),
    .pool_index_stacks = s_ctx.uart_storage.pool_index_stacks,
    .pool_index_stack_count = 8u,
    .ready_ring_slots = s_ctx.uart_storage.ready_ring_slots,
    .ready_ring_slot_count = 8u,
  };
  esy_logger_backend_uart_core_config_t cfg;
  esy_logger_backend_uart_core_default_cfg(&cfg);
  cfg.uart = &s_ctx.uart;
  cfg.block_size = TEST_BLOCK_SIZE;
  cfg.pool_block_count = TEST_BLOCK_COUNT;
  cfg.dma_short_threshold = threshold;
  cfg.static_resources = &resources;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_init(&s_ctx.backend, &cfg));
}

static void assert_busy_query_runs_outside_critical(esy_uart_t *uart) {
  (void)uart;
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_critical_nesting());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
}

void test_uart_terminal_source_release_does_not_confirm_physical_quiescence(void) {
  for (unsigned os_mode = 0u; os_mode < 2u; ++os_mode) {
    for (unsigned failed = 0u; failed < 2u; ++failed) {
      if (os_mode != 0u) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_init(
            &s_ctx.backend, &s_ctx.uart_config));
      } else init_poll_uart(0u);
      fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
      slot->send_dma_result = ESY_OK;
      slot->keep_tx_busy_after_dma = true;
      slot->busy_hook = assert_busy_query_runs_outside_critical;
      queue_block();
      TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&s_ctx.backend));
      if (failed != 0u) fake_esy_hal_uart_fail_last_dma(&s_ctx.uart);
      else fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
      TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.backend));
      TEST_ASSERT_FALSE(slot->dma_pending);
      TEST_ASSERT_TRUE(slot->tx_busy);
      /* Source retirement releases every pool block even while the line drains. */
      uint8_t *blocks[TEST_BLOCK_COUNT];
      size_t capacity;
      for (unsigned i = 0u; i < TEST_BLOCK_COUNT; ++i)
        TEST_ASSERT_EQUAL(ESY_OK, s_ctx.backend.acquire_buffer(
            &s_ctx.backend, &blocks[i], &capacity));
      for (unsigned i = 0u; i < TEST_BLOCK_COUNT; ++i)
        TEST_ASSERT_EQUAL(ESY_OK, s_ctx.backend.submit_buffer(
            &s_ctx.backend, blocks[i], 0u));
      void *context = s_ctx.backend.ctx;
      uint32_t deleted = fake_esy_os_task_get_delete_count();
      uint32_t aborted = slot->abort_count;
      TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_quiesce(&s_ctx.backend));
      TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&s_ctx.backend));
      TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&s_ctx.backend));
      TEST_ASSERT_EQUAL_PTR(context, s_ctx.backend.ctx);
      TEST_ASSERT_EQUAL(deleted, fake_esy_os_task_get_delete_count());
      TEST_ASSERT_EQUAL(aborted, slot->abort_count);
      TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.backend.acquire_buffer(
          &s_ctx.backend, &s_ctx.borrowed, &capacity));
      slot->tx_busy = false;
      TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
      TEST_ASSERT_NULL(s_ctx.backend.ctx);
      slot->keep_tx_busy_after_dma = false;
    }
  }
}

static void queue_and_run_during_quiesce_check(esy_uart_t *uart) {
  assert_busy_query_runs_outside_critical(uart);
  fake_esy_hal_uart_find(uart)->busy_hook = NULL;
  queue_block();
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_try_kick_one(&s_ctx.backend));
}

void test_busy_quiesce_reopens_sender_and_renotifies_work_queued_during_check(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_init(
      &s_ctx.backend, &s_ctx.uart_config));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->send_dma_result = ESY_OK;
  slot->tx_busy = true;
  slot->busy_hook = queue_and_run_during_quiesce_check;
  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  uint32_t notified = task->notify_give_count;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_quiesce(&s_ctx.backend));
  TEST_ASSERT_EQUAL(notified + 2u, task->notify_give_count);
  TEST_ASSERT_EQUAL_UINT32(0u, slot->send_dma_count);
  slot->tx_busy = false;
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&s_ctx.backend));
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  slot->busy_hook = assert_busy_query_runs_outside_critical;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_quiesce(&s_ctx.backend));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_logger_backend_uart_quiesce(&s_ctx.backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_resume(&s_ctx.backend));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

static void leave_wire_busy_after_blocking_send(esy_uart_t *uart) {
  fake_esy_hal_uart_find(uart)->tx_busy = true;
}

void test_blocking_source_return_also_waits_for_wire_drain_before_shutdown(void) {
  init_poll_uart(TEST_BLOCK_SIZE);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->send_hook = leave_wire_busy_after_blocking_send;
  slot->busy_hook = assert_busy_query_runs_outside_critical;
  queue_block();
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_quiesce(&s_ctx.backend));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&s_ctx.backend));
  slot->tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  init_poll_uart(TEST_BLOCK_SIZE);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

void test_poll_uart_bounds_work_and_retries_unsent_tail_after_time_interval(void) {
  init_poll_uart(TEST_BLOCK_SIZE);
  queue_block();
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->override_send_result = true;
  slot->send_result = 4;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_EQUAL(1u, slot->send_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_EQUAL(1u, slot->send_count);
  fake_esy_hal_system_advance_tick(10u);
  slot->override_send_result = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_EQUAL(2u, slot->send_count);
  TEST_ASSERT_EQUAL(12u, slot->last_tx_len);
  TEST_ASSERT_EQUAL_MEMORY("456789abcdef", slot->last_tx, 12u);
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

void test_poll_uart_busy_stop_preserves_dma_and_releases_after_completion(void) {
  init_poll_uart(0u);
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
  queue_block();
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&s_ctx.backend));
  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
}

static void complete_log_during_uart_poll(esy_uart_t *uart) {
  TEST_ASSERT_FALSE(s_ctx.in_critical);
  fake_esy_hal_uart_find(uart)->poll_hook = NULL;
  fake_esy_hal_uart_complete_last_dma(uart);
}

void test_poll_uart_shutdown_keeps_dma_storage_until_owner_cleanup_succeeds(void) {
  init_poll_uart(0u);
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
  queue_block();
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  slot->poll_result = ESY_ERR_IO;
  slot->poll_hook = complete_log_during_uart_poll;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_TRUE(slot->tx_busy);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&s_ctx.backend));
  slot->poll_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_FALSE(slot->tx_busy);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

void test_failed_dma_submit_keeps_pool_block_until_poll_or_abort_and_never_resends(void) {
  for (unsigned retire_with_poll = 0u; retire_with_poll < 2u; ++retire_with_poll) {
    init_poll_uart(0u);
    queue_block();
    const uint8_t *held = s_ctx.borrowed;
    fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
    const uint32_t sends_before = slot->send_dma_count;
    const uint32_t aborts_before = slot->abort_count;
    slot->send_dma_result = ESY_ERR_IO;
    slot->retain_failed_submission = true;
    slot->abort_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 32u));
    TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.backend));
    TEST_ASSERT_EQUAL_UINT32(aborts_before, slot->abort_count);
    uint8_t *remaining[TEST_BLOCK_COUNT - 1u];
    size_t capacity;
    for (unsigned i = 0u; i < TEST_BLOCK_COUNT - 1u; ++i) {
      TEST_ASSERT_EQUAL(ESY_OK, s_ctx.backend.acquire_buffer(
          &s_ctx.backend, &remaining[i], &capacity));
      TEST_ASSERT_TRUE(held != remaining[i]);
    }
    uint8_t *unavailable;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.backend.acquire_buffer(
        &s_ctx.backend, &unavailable, &capacity));
    for (unsigned i = 0u; i < TEST_BLOCK_COUNT - 1u; ++i) {
      TEST_ASSERT_EQUAL(ESY_OK, s_ctx.backend.submit_buffer(
          &s_ctx.backend, remaining[i], 0u));
    }
    TEST_ASSERT_EQUAL_MEMORY("0123456789abcdef", held, 16u);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&s_ctx.backend));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_logger_backend_uart_core_process(&s_ctx.backend, 32u));
    TEST_ASSERT_EQUAL_UINT32(aborts_before + 1u, slot->abort_count);
    TEST_ASSERT_EQUAL_UINT32(sends_before + 1u, slot->send_dma_count);
    TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.backend));
    slot->abort_result = ESY_ERR_STATE;
    TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 32u));
    TEST_ASSERT_TRUE(slot->tx_busy);
    TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.backend));
    TEST_ASSERT_EQUAL_UINT32(aborts_before + 2u, slot->abort_count);
    TEST_ASSERT_EQUAL_UINT32(sends_before + 1u, slot->send_dma_count);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&s_ctx.backend));
    if (retire_with_poll != 0u) slot->poll_hook = complete_log_during_uart_poll;
    else slot->abort_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_core_process(&s_ctx.backend, 32u));
    TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&s_ctx.backend));
    TEST_ASSERT_EQUAL_UINT32(sends_before + 1u, slot->send_dma_count);
    TEST_ASSERT_FALSE(slot->tx_busy);
    TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  }
}

void test_poll_printf_drains_only_the_requested_number_of_blocks(void) {
  esy_logger_backend_printf_core_resources_t resources = {
    .pool_buffers = s_ctx.printf_storage.pool_buffers,
    .pool_buffer_size = sizeof(s_ctx.printf_storage.pool_buffers),
    .pool_index_stacks = s_ctx.printf_storage.pool_index_stacks,
    .pool_index_stack_count = 8u,
    .ready_ring_slots = s_ctx.printf_storage.ready_ring_slots,
    .ready_ring_slot_count = 8u,
  };
  esy_logger_backend_printf_core_config_t cfg;
  esy_logger_backend_printf_core_default_cfg(&cfg);
  cfg.block_size = TEST_BLOCK_SIZE;
  cfg.pool_block_count = TEST_BLOCK_COUNT;
  cfg.static_resources = &resources;
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_printf_core_init(&s_ctx.backend, &cfg));
  queue_block();
  queue_block();
  begin_capture();
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_printf_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_EQUAL(16u, s_ctx.output_size);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_printf_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_EQUAL(32u, s_ctx.output_size);
  TEST_ASSERT_FALSE(s_ctx.in_critical);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_printf_deinit(&s_ctx.backend));
}

void test_poll_rtt_preserves_borrowed_buffer_on_busy_stop(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_rtt_core_init(&s_ctx.backend, &s_ctx.rtt_config));
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.backend.acquire_buffer(&s_ctx.backend, &s_ctx.borrowed, &s_ctx.borrowed_size));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_rtt_deinit(&s_ctx.backend));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.backend.submit_buffer(&s_ctx.backend, s_ctx.borrowed, 0u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_rtt_deinit(&s_ctx.backend));
}

void test_core_logger_process_rejects_os_workers_and_zero_budget_without_io(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  queue_block();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_logger_backend_uart_core_process(&s_ctx.backend, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_logger_backend_uart_core_process(&s_ctx.backend, 1u));
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_uart_find(&s_ctx.uart)->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

static void park_sender(void) {
  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  fake_esy_os_task_set_current(task->task_handle);
  fake_esy_os_task_set_park_hook(exit_on_wait);
  if (setjmp(s_ctx.task_exit) == 0) task->entry(task->arg);
  fake_esy_os_task_set_park_hook(NULL);
  fake_esy_os_task_set_current(NULL);
}

void test_os_uart_stop_waits_for_park_before_releasing_context_and_storage(void) {
  fake_esy_os_set_started(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  queue_block();
  void *context = s_ctx.backend.ctx;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&s_ctx.backend));
  TEST_ASSERT_EQUAL_PTR(context, s_ctx.backend.ctx);
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_delete_count());
  park_sender();
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_uart_find(&s_ctx.uart)->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
  TEST_ASSERT_NULL(s_ctx.backend.ctx);
  TEST_ASSERT_EQUAL(1u, fake_esy_os_task_get_delete_count());
  fake_esy_os_set_started(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_uart_deinit(&s_ctx.backend));
}

void test_os_printf_stop_waits_for_park_before_releasing_context_and_storage(void) {
  fake_esy_os_set_started(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_printf_init(&s_ctx.backend, &s_ctx.printf_config));
  queue_block();
  begin_capture();
  void *context = s_ctx.backend.ctx;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_logger_backend_printf_deinit(&s_ctx.backend));
  TEST_ASSERT_EQUAL_PTR(context, s_ctx.backend.ctx);
  park_sender();
  TEST_ASSERT_EQUAL(0u, s_ctx.output_size);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_printf_deinit(&s_ctx.backend));
  TEST_ASSERT_NULL(s_ctx.backend.ctx);
  TEST_ASSERT_EQUAL(1u, fake_esy_os_task_get_delete_count());
}

void test_os_logger_rejects_ports_without_cooperative_parking_before_creating_workers(void) {
  fake_esy_os_task_set_park_supported(false);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_logger_backend_uart_init(&s_ctx.backend, &s_ctx.uart_config));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_logger_backend_printf_init(&s_ctx.backend, &s_ctx.printf_config));
  TEST_ASSERT_NULL(s_ctx.backend.ctx);
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_rtt_init(&s_ctx.backend, &s_ctx.rtt_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_backend_rtt_deinit(&s_ctx.backend));
}
