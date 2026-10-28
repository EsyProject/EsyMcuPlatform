/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esy_middleware/logger/esy_logger.h"
#include "esy_middleware/logger/esy_logger_backend_printf.h"
#include "esy_middleware/logger/esy_logger_backend_rtt.h"
#include "esy_middleware/logger/esy_logger_backend_uart.h"
#include "esy_logger_backend_uart_internal.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_dma.h"
#include "fake_esy_hal_system.h"
#include "fake_esy_hal_uart.h"
#include "fake_segger_rtt.h"

#define TEST_LOGGER_BLOCK_SIZE 32u
#define TEST_LOGGER_POOL_BLOCK_COUNT_MAX 4u
#define TEST_LOGGER_TASK_STACK_SIZE 256u

typedef struct {
  esy_logger_backend_binding_t binding;
  esy_uart_t uart;
  ESY_LOGGER_BACKEND_UART_STATIC_STORAGE(
      uart_storage, TEST_LOGGER_POOL_BLOCK_COUNT_MAX,
      TEST_LOGGER_BLOCK_SIZE, TEST_LOGGER_TASK_STACK_SIZE);
  esy_logger_backend_uart_static_resources_t uart_static_resources;
  ESY_LOGGER_BACKEND_PRINTF_STATIC_STORAGE(
      printf_storage, ESY_MAX_CORE_COUNT, TEST_LOGGER_POOL_BLOCK_COUNT_MAX,
      TEST_LOGGER_BLOCK_SIZE, TEST_LOGGER_TASK_STACK_SIZE);
  esy_logger_backend_printf_static_resources_t printf_static_resources;
  ESY_LOGGER_BACKEND_RTT_STATIC_STORAGE(rtt_storage, TEST_LOGGER_BLOCK_SIZE);
  esy_logger_backend_rtt_static_resources_t rtt_static_resources;
} logger_test_ctx_t;

static logger_test_ctx_t s_ctx;

static esy_err_t bind_test_backend(const esy_logger_backend_t *backend) {
  s_ctx.binding = (esy_logger_backend_binding_t){
      .backend = backend, .filter_level = ESY_LOG_LEVEL_VERBOSE};
  const esy_logger_static_resources_t resources = {
      .bindings = &s_ctx.binding, .binding_count = 1u};
  const esy_logger_config_t config = {
      .static_resources = &resources, .filter_level = ESY_LOG_LEVEL_INFO};
  return esy_logger_init(&config);
}

typedef struct {
  uint8_t storage[256];
  uint8_t capture[1200];
  size_t capture_len;
  size_t offered_len;
  size_t acquire_count;
  size_t submit_count;
  size_t last_submit_len;
  esy_err_t acquire_result;
  esy_err_t submit_result;
} fake_logger_backend_ctx_t;

static esy_err_t fake_backend_acquire(void *self, uint8_t **buf,
                                      size_t *buf_len) {
  esy_logger_backend_t *backend = (esy_logger_backend_t *)self;
  fake_logger_backend_ctx_t *ctx =
      backend != NULL ? (fake_logger_backend_ctx_t *)backend->ctx : NULL;
  if (ctx == NULL || buf == NULL || buf_len == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  ctx->acquire_count++;
  if (ctx->acquire_result != ESY_OK) {
    return ctx->acquire_result;
  }

  *buf = ctx->storage;
  *buf_len = ctx->offered_len != 0u ? ctx->offered_len : sizeof(ctx->storage);
  if (*buf_len > sizeof(ctx->storage)) {
    *buf_len = sizeof(ctx->storage);
  }
  return ESY_OK;
}

static esy_err_t fake_backend_submit(void *self, const uint8_t *buf,
                                     size_t len) {
  esy_logger_backend_t *backend = (esy_logger_backend_t *)self;
  fake_logger_backend_ctx_t *ctx =
      backend != NULL ? (fake_logger_backend_ctx_t *)backend->ctx : NULL;
  if (ctx == NULL || buf != ctx->storage) {
    return ESY_ERR_INVALID_ARG;
  }

  ctx->submit_count++;
  ctx->last_submit_len = len;
  size_t free_len = sizeof(ctx->capture) - ctx->capture_len;
  size_t copy_len = len < free_len ? len : free_len;
  if (copy_len > 0u) {
    memcpy(&ctx->capture[ctx->capture_len], buf, copy_len);
    ctx->capture_len += copy_len;
  }
  return ctx->submit_result;
}

static void fill_uart_cfg(esy_logger_backend_uart_config_t *cfg,
                          uint32_t pool_block_count,
                          uint32_t dma_short_threshold) {
  esy_logger_backend_uart_default_cfg(cfg);
  memset(&s_ctx.uart_static_resources, 0, sizeof(s_ctx.uart_static_resources));
  ESY_LOGGER_BACKEND_UART_BIND_STATIC_RESOURCES(&s_ctx.uart_static_resources,
                                                s_ctx.uart_storage);

  cfg->uart = &s_ctx.uart;
  cfg->block_size = TEST_LOGGER_BLOCK_SIZE;
  cfg->pool_block_count = pool_block_count;
  cfg->dma_short_threshold = dma_short_threshold;
  cfg->static_resources = &s_ctx.uart_static_resources;
}

static void fill_printf_cfg(esy_logger_backend_printf_config_t *cfg,
                            uint32_t pool_block_count) {
  esy_logger_backend_printf_default_cfg(cfg);
  memset(&s_ctx.printf_static_resources, 0, sizeof(s_ctx.printf_static_resources));
  ESY_LOGGER_BACKEND_PRINTF_BIND_STATIC_RESOURCES(&s_ctx.printf_static_resources,
                                                  s_ctx.printf_storage);

  cfg->block_size = TEST_LOGGER_BLOCK_SIZE;
  cfg->pool_block_count = pool_block_count;
  cfg->static_resources = &s_ctx.printf_static_resources;
}

static void fill_rtt_cfg(esy_logger_backend_rtt_config_t *cfg) {
  esy_logger_backend_rtt_default_cfg(cfg);
  memset(&s_ctx.rtt_static_resources, 0, sizeof(s_ctx.rtt_static_resources));
  ESY_LOGGER_BACKEND_RTT_BIND_STATIC_RESOURCES(&s_ctx.rtt_static_resources,
                                               s_ctx.rtt_storage);

  cfg->up_buffer_index = 1u;
  cfg->static_resources = &s_ctx.rtt_static_resources;
}

void setUp(void) {
  memset(&s_ctx.uart, 0, sizeof(s_ctx.uart));
  memset(&s_ctx.uart_storage.context_storage, 0,
         sizeof(s_ctx.uart_storage.context_storage));
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_dma_reset();
  fake_esy_hal_system_reset();
  fake_esy_hal_uart_reset();
  fake_segger_rtt_reset();
  esy_uart_config_t uart_cfg;
  esy_hal_uart_default_cfg(&uart_cfg);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_uart_init(&s_ctx.uart, &uart_cfg));
  fake_esy_hal_system_get_ctx()->core_count = 1u;
  fake_esy_hal_system_get_ctx()->core_id = 0u;
}

void tearDown(void) {}

void test_logger_init_rejects_invalid_backend_parameters(void) {
  fake_logger_backend_ctx_t ctx = {0};
  esy_logger_backend_t backend = {
      .acquire_buffer = fake_backend_acquire,
      .submit_buffer = fake_backend_submit,
      .ctx = &ctx,
  };

  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_logger_init(NULL));

  backend.acquire_buffer = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, bind_test_backend(&backend));

  backend.acquire_buffer = fake_backend_acquire;
  backend.submit_buffer = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, bind_test_backend(&backend));
}

void test_logger_submit_uses_backend_and_honors_filter(void) {
  fake_logger_backend_ctx_t ctx = {
      .acquire_result = ESY_OK,
      .submit_result = ESY_OK,
  };
  esy_logger_backend_t backend = {
      .acquire_buffer = fake_backend_acquire,
      .submit_buffer = fake_backend_submit,
      .ctx = &ctx,
  };

  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&backend));

  esy_log("LOG", ESY_LOG_LEVEL_INFO, "value=%d", 7);
  TEST_ASSERT_EQUAL_UINT(1u, ctx.acquire_count);
  TEST_ASSERT_EQUAL_UINT(1u, ctx.submit_count);
  TEST_ASSERT_TRUE(ctx.last_submit_len > 0u);
  TEST_ASSERT_NOT_NULL(strstr((const char *)ctx.storage, "[LOG][I] value=7"));

  esy_logger_set_filter_level(ESY_LOG_LEVEL_ERROR);
  esy_log("LOG", ESY_LOG_LEVEL_INFO, "filtered");
  TEST_ASSERT_EQUAL_UINT(1u, ctx.acquire_count);
  TEST_ASSERT_EQUAL_UINT(1u, ctx.submit_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_detach(&backend));
}

void test_logger_drops_invalid_level_without_backend_access(void) {
  fake_logger_backend_ctx_t ctx = {
      .acquire_result = ESY_OK,
      .submit_result = ESY_OK,
  };
  esy_logger_backend_t backend = {
      .acquire_buffer = fake_backend_acquire,
      .submit_buffer = fake_backend_submit,
      .ctx = &ctx,
  };

  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&backend));

  esy_log("LOG", (esy_log_level_t)99, "invalid");
  TEST_ASSERT_EQUAL_UINT(0u, ctx.acquire_count);
  TEST_ASSERT_EQUAL_UINT(0u, ctx.submit_count);

  esy_logger_set_filter_level((esy_log_level_t)99);
  esy_log("LOG", ESY_LOG_LEVEL_INFO, "still-info");
  TEST_ASSERT_EQUAL_UINT(1u, ctx.acquire_count);
  TEST_ASSERT_EQUAL_UINT(1u, ctx.submit_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_detach(&backend));
}

void test_logger_stops_when_acquire_or_submit_fails(void) {
  fake_logger_backend_ctx_t ctx = {
      .offered_len = 8u,
      .acquire_result = ESY_ERR_NO_MEM,
      .submit_result = ESY_OK,
  };
  esy_logger_backend_t backend = {
      .acquire_buffer = fake_backend_acquire,
      .submit_buffer = fake_backend_submit,
      .ctx = &ctx,
  };

  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&backend));
  esy_log("LOG", ESY_LOG_LEVEL_INFO, "message requiring a buffer");
  TEST_ASSERT_EQUAL_UINT(1u, ctx.acquire_count);
  TEST_ASSERT_EQUAL_UINT(0u, ctx.submit_count);

  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_detach(&backend));
  memset(&ctx, 0, sizeof(ctx));
  ctx.offered_len = 8u;
  ctx.acquire_result = ESY_OK;
  ctx.submit_result = ESY_ERR_FAIL;
  backend.ctx = &ctx;

  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&backend));
  esy_log("LOG", ESY_LOG_LEVEL_INFO, "message split into chunks");
  TEST_ASSERT_EQUAL_UINT(1u, ctx.acquire_count);
  TEST_ASSERT_EQUAL_UINT(1u, ctx.submit_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_detach(&backend));
}

void test_logger_chunks_long_lines_across_backend_buffers(void) {
  fake_logger_backend_ctx_t ctx = {
      .offered_len = 8u,
      .acquire_result = ESY_OK,
      .submit_result = ESY_OK,
  };
  esy_logger_backend_t backend = {
      .acquire_buffer = fake_backend_acquire,
      .submit_buffer = fake_backend_submit,
      .ctx = &ctx,
  };

  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&backend));
  esy_log("LOG", ESY_LOG_LEVEL_INFO, "abcdefghijklmnopqrstuvwxyz");

  TEST_ASSERT_TRUE(ctx.acquire_count > 1u);
  TEST_ASSERT_EQUAL_UINT(ctx.acquire_count, ctx.submit_count);
  TEST_ASSERT_NOT_NULL(strstr((const char *)ctx.capture,
                              "[LOG][I] abcdefghijklmnopqrstuvwxyz"));
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_detach(&backend));
}

void test_logger_truncates_to_bounded_line_and_keeps_newline(void) {
  char payload[1100];
  memset(payload, 'A', sizeof(payload));
  payload[sizeof(payload) - 1u] = '\0';

  fake_logger_backend_ctx_t ctx = {
      .acquire_result = ESY_OK,
      .submit_result = ESY_OK,
  };
  esy_logger_backend_t backend = {
      .acquire_buffer = fake_backend_acquire,
      .submit_buffer = fake_backend_submit,
      .ctx = &ctx,
  };

  TEST_ASSERT_EQUAL_INT(ESY_OK, bind_test_backend(&backend));
  esy_log("LOG", ESY_LOG_LEVEL_INFO, "%s", payload);

  TEST_ASSERT_EQUAL_UINT(1023u, ctx.capture_len);
  TEST_ASSERT_EQUAL_UINT8('\n', ctx.capture[1022u]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_logger_detach(&backend));
}

void test_printf_backend_init_rejects_invalid_parameters_and_repeat_init(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_printf_config_t cfg;
  fill_printf_cfg(&cfg, 4u);

  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(NULL, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(&backend, NULL));

  cfg.static_resources = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(&backend, &cfg));

  fill_printf_cfg(&cfg, 4u);
  cfg.static_resources->ready_ring_slot_count = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(&backend, &cfg));

  cfg.block_size = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(&backend, &cfg));

  fill_printf_cfg(&cfg, 1u);
  s_ctx.printf_storage.pool_index_stacks[0] = 0xA5A5A5A5u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5u,
                          s_ctx.printf_storage.pool_index_stacks[0]);
  TEST_ASSERT_NULL(backend.ctx);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_create_static_count());

  fill_printf_cfg(&cfg, 3u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(&backend, &cfg));

  fill_printf_cfg(&cfg, 4u);
  cfg.block_size = 129u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(&backend, &cfg));

  fill_printf_cfg(&cfg, 4u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_printf_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_os_task_get_create_static_count());
  cfg.pool_block_count = 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
                        esy_logger_backend_printf_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&backend));
  TEST_ASSERT_NULL(backend.ctx);
}

void test_printf_backend_pool_empty_ring_full_and_isr_submit(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_printf_config_t cfg;
  uint8_t *first = NULL;
  uint8_t *second = NULL;
  uint8_t *third = NULL;
  size_t len = 0u;

  fill_printf_cfg(&cfg, 2u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_printf_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &first, &len));
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &second, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
                        backend.acquire_buffer(&backend, &third, &len));

  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, first, 5u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
                        backend.submit_buffer(&backend, second, 6u));

  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  TEST_ASSERT_NOT_NULL(task);
  TEST_ASSERT_EQUAL_UINT(1u, task->notify_give_count);

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &third, &len));
  TEST_ASSERT_EQUAL_PTR(second, third);
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, third, 0u));
  fake_esy_hal_system_get_ctx()->is_in_isr = false;

  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&backend));
}

void test_printf_backend_rejects_runtime_topology_and_task_failures(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_printf_config_t cfg;

  fill_printf_cfg(&cfg, 4u);
  fake_esy_hal_system_get_ctx()->core_count = ESY_MAX_CORE_COUNT + 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_init(&backend, &cfg));

  fake_esy_hal_system_get_ctx()->core_count = 1u;
  fake_esy_os_task_set_create_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL,
                        esy_logger_backend_printf_init(&backend, &cfg));
  TEST_ASSERT_NULL(backend.ctx);
}

void test_printf_backend_rejects_invalid_acquire_and_submit_arguments(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_printf_config_t cfg;
  uint8_t *buf = NULL;
  size_t len = 0u;

  fill_printf_cfg(&cfg, 2u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_printf_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(NULL, &buf, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(&backend, NULL, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(&backend, &buf, NULL));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(NULL, buf, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(&backend, NULL, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(&backend, buf + 1u, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, backend.submit_buffer(&backend, buf, len + 1u));

  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&backend));
}

void test_rtt_backend_init_rejects_invalid_parameters_and_repeat_init(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_rtt_config_t cfg;

  fill_rtt_cfg(&cfg);

  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_rtt_init(NULL, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_rtt_init(&backend, NULL));

  fill_rtt_cfg(&cfg);
  cfg.static_resources = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_rtt_init(&backend, &cfg));

  fill_rtt_cfg(&cfg);
  cfg.static_resources->buffer_size = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_rtt_init(&backend, &cfg));

  fill_rtt_cfg(&cfg);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
                        esy_logger_backend_rtt_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_deinit(&backend));
  TEST_ASSERT_NULL(backend.ctx);
}

void test_rtt_backend_acquire_submit_and_busy_paths(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_rtt_config_t cfg;
  uint8_t *buf = NULL;
  uint8_t *busy_buf = NULL;
  size_t len = 0u;
  size_t busy_len = 0u;

  fill_rtt_cfg(&cfg);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(NULL, &buf, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(&backend, NULL, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(&backend, &buf, NULL));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  TEST_ASSERT_EQUAL_UINT(TEST_LOGGER_BLOCK_SIZE, len);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY,
                        backend.acquire_buffer(&backend, &busy_buf,
                                               &busy_len));

  memcpy(buf, "rtt", 3u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(NULL, buf, 3u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(&backend, NULL, 3u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(&backend, buf + 1u, 3u));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, buf, 3u));
  fake_segger_rtt_state_t *rtt = fake_segger_rtt_get_state();
  TEST_ASSERT_EQUAL_UINT(1u, rtt->write_count);
  TEST_ASSERT_EQUAL_UINT(1u, rtt->last_buffer_index);
  TEST_ASSERT_EQUAL_MEMORY("rtt", rtt->capture, 3u);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_critical_nesting());

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  fake_segger_rtt_set_forced_write_len(1u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, backend.submit_buffer(&backend, buf, 3u));

  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_rtt_deinit(&backend));
}

void test_uart_backend_init_rejects_invalid_parameters_and_repeat_init(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;
  fill_uart_cfg(&cfg, 4u, 4u);

  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(NULL, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(&backend, NULL));

  cfg.uart = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(&backend, &cfg));

  fill_uart_cfg(&cfg, 4u, 4u);
  cfg.static_resources = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(&backend, &cfg));

  fill_uart_cfg(&cfg, 4u, 4u);
  cfg.static_resources->pool_buffer_size = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(&backend, &cfg));

  fill_uart_cfg(&cfg, 4u, 4u);
  cfg.block_size = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(&backend, &cfg));

  fill_uart_cfg(&cfg, 1u, 4u);
  s_ctx.uart_storage.pool_index_stacks[0] = 0xA5A5A5A5u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5u,
                          s_ctx.uart_storage.pool_index_stacks[0]);
  TEST_ASSERT_NULL(backend.ctx);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_create_static_count());

  fill_uart_cfg(&cfg, 3u, 4u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(&backend, &cfg));

  fill_uart_cfg(&cfg, 4u, 33u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_init(&backend, &cfg));

  fill_uart_cfg(&cfg, 4u, 4u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_init(&backend, &cfg));
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_is_initialized(&backend));
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_os_task_get_create_static_count());

  cfg.pool_block_count = 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
                        esy_logger_backend_uart_init(&backend, &cfg));

  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
  TEST_ASSERT_NULL(backend.ctx);
}

void test_uart_backend_pool_empty_and_ring_full_return_blocks(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;
  uint8_t *first = NULL;
  uint8_t *second = NULL;
  uint8_t *third = NULL;
  size_t len = 0u;

  fill_uart_cfg(&cfg, 2u, 32u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &first, &len));
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &second, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
                        backend.acquire_buffer(&backend, &third, &len));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, first, 5u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
                        backend.submit_buffer(&backend, second, 6u));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &third, &len));
  TEST_ASSERT_EQUAL_PTR(second, third);
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, third, 0u));
  fake_esy_hal_system_get_ctx()->is_in_isr = false;

  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
}

void test_uart_backend_dma_start_failure_releases_busy_and_buffer(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;
  uint8_t *buf = NULL;
  uint8_t *again = NULL;
  size_t len = 0u;

  fill_uart_cfg(&cfg, 2u, 0u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  memset(buf, 'A', 16u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, buf, 16u));

  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&backend));
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&backend));

  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, slot->send_dma_result);

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &again, &len));
  TEST_ASSERT_EQUAL_PTR(buf, again);
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, again, 0u));

  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
}

void test_uart_backend_deinit_defers_cleanup_while_dma_is_busy(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;
  uint8_t *buf = NULL;
  size_t len = 0u;

  fill_uart_cfg(&cfg, 2u, 0u);
  fake_esy_hal_uart_set_send_dma_result(&s_ctx.uart, ESY_OK);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  memset(buf, 'B', 16u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, buf, 16u));

  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&backend));
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_is_dma_busy(&backend));

  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_logger_backend_uart_deinit(&backend));
  TEST_ASSERT_NOT_NULL(backend.acquire_buffer);
  TEST_ASSERT_NOT_NULL(backend.submit_buffer);
  TEST_ASSERT_NOT_NULL(backend.ctx);
  TEST_ASSERT_TRUE(
      esy_logger_backend_uart_internal_is_deinit_requested(&backend));
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_delete_count());

  fake_esy_hal_uart_complete_last_dma(&s_ctx.uart);
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&backend));

  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
  TEST_ASSERT_NULL(backend.ctx);
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_os_task_get_delete_count());
}

void test_uart_backend_submit_from_isr_notifies_task_with_isr_path(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;
  uint8_t *buf = NULL;
  size_t len = 0u;

  fill_uart_cfg(&cfg, 2u, 32u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_init(&backend, &cfg));

  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, buf, 4u));

  fake_esy_os_task_entry_t *task = fake_esy_os_task_get_last_created();
  TEST_ASSERT_NOT_NULL(task);
  TEST_ASSERT_EQUAL_UINT(1u, task->notify_give_count);
  TEST_ASSERT_FALSE(fake_esy_os_get_state()->last_isr_yield_requested);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_critical_nesting());

  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
}

void test_uart_backend_rejects_task_create_failure(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;

  fill_uart_cfg(&cfg, 4u, 4u);
  fake_esy_os_task_set_create_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL,
                        esy_logger_backend_uart_init(&backend, &cfg));
  TEST_ASSERT_NULL(backend.ctx);
}

void test_uart_backend_rejects_invalid_acquire_submit_and_internal_arguments(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;
  uint8_t *buf = NULL;
  size_t len = 0u;

  fill_uart_cfg(&cfg, 2u, 32u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_init(&backend, &cfg));

  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_try_kick_one(NULL));
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_initialized(NULL));
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_deinit_requested(NULL));
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(NULL));

  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(NULL, &buf, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(&backend, NULL, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.acquire_buffer(&backend, &buf, NULL));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(NULL, buf, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(&backend, NULL, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        backend.submit_buffer(&backend, buf + 1u, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, buf, 0u));

  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
}

void test_uart_backend_short_message_uses_blocking_send_path(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;
  uint8_t *buf = NULL;
  size_t len = 0u;

  fill_uart_cfg(&cfg, 2u, 32u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_init(&backend, &cfg));

  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  memset(buf, 'S', 4u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, buf, 4u));

  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&backend));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT(1u, slot->send_count);
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_is_dma_busy(&backend));

  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
}

void test_uart_backend_quiesce_preserves_pending_log_until_resume(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t cfg;
  uint8_t *buf = NULL;
  size_t len = 0u;

  fill_uart_cfg(&cfg, 2u, 32u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.acquire_buffer(&backend, &buf, &len));
  memset(buf, 'Q', 4u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, backend.submit_buffer(&backend, buf, 4u));

  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_quiesce(&backend));
  TEST_ASSERT_FALSE(esy_logger_backend_uart_internal_try_kick_one(&backend));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_resume(&backend));
  TEST_ASSERT_TRUE(esy_logger_backend_uart_internal_try_kick_one(&backend));

  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&s_ctx.uart);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT(1u, slot->send_count);
  fake_esy_hal_system_get_ctx()->is_in_isr = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
}

void test_printf_core_pool_depth_rejects_one_before_storage_changes_and_accepts_two(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_printf_config_t os_cfg;
  fill_printf_cfg(&os_cfg, 2u);
  esy_logger_backend_printf_core_resources_t resources = {
      .pool_buffers = s_ctx.printf_static_resources.pool_buffers,
      .pool_buffer_size = s_ctx.printf_static_resources.pool_buffer_size,
      .pool_index_stacks = s_ctx.printf_static_resources.pool_index_stacks,
      .pool_index_stack_count = s_ctx.printf_static_resources.pool_index_stack_count,
      .ready_ring_slots = s_ctx.printf_static_resources.ready_ring_slots,
      .ready_ring_slot_count = s_ctx.printf_static_resources.ready_ring_slot_count,
  };
  esy_logger_backend_printf_core_config_t cfg;
  esy_logger_backend_printf_core_default_cfg(&cfg);
  cfg.block_size = TEST_LOGGER_BLOCK_SIZE;
  cfg.pool_block_count = 1u;
  cfg.static_resources = &resources;
  resources.pool_index_stacks[0] = 0xA5A5A5A5u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_printf_core_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5u, resources.pool_index_stacks[0]);
  TEST_ASSERT_NULL(backend.ctx);
  cfg.pool_block_count = 2u;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_printf_core_init(&backend, &cfg));
  cfg.pool_block_count = 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
                        esy_logger_backend_printf_core_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&backend));
  cfg.pool_block_count = 2u;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_printf_core_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_printf_deinit(&backend));
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_create_static_count());
}

void test_uart_core_pool_depth_rejects_one_before_storage_changes_and_accepts_two(void) {
  esy_logger_backend_t backend = {0};
  esy_logger_backend_uart_config_t os_cfg;
  fill_uart_cfg(&os_cfg, 2u, TEST_LOGGER_BLOCK_SIZE);
  esy_logger_backend_uart_core_resources_t resources = {
      .context_storage = s_ctx.uart_static_resources.context_storage,
      .pool_buffers = s_ctx.uart_static_resources.pool_buffers,
      .pool_buffer_size = s_ctx.uart_static_resources.pool_buffer_size,
      .pool_index_stacks = s_ctx.uart_static_resources.pool_index_stacks,
      .pool_index_stack_count = s_ctx.uart_static_resources.pool_index_stack_count,
      .ready_ring_slots = s_ctx.uart_static_resources.ready_ring_slots,
      .ready_ring_slot_count = s_ctx.uart_static_resources.ready_ring_slot_count,
  };
  esy_logger_backend_uart_core_config_t cfg;
  esy_logger_backend_uart_core_default_cfg(&cfg);
  cfg.uart = &s_ctx.uart;
  cfg.block_size = TEST_LOGGER_BLOCK_SIZE;
  cfg.pool_block_count = 1u;
  cfg.dma_short_threshold = TEST_LOGGER_BLOCK_SIZE;
  cfg.static_resources = &resources;
  resources.pool_index_stacks[0] = 0xA5A5A5A5u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_logger_backend_uart_core_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5u, resources.pool_index_stacks[0]);
  TEST_ASSERT_NULL(backend.ctx);
  cfg.pool_block_count = 2u;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_core_init(&backend, &cfg));
  cfg.pool_block_count = 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
                        esy_logger_backend_uart_core_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
  cfg.pool_block_count = 2u;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_logger_backend_uart_core_init(&backend, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_logger_backend_uart_deinit(&backend));
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_task_get_create_static_count());
}
