/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/logger/esy_logger_backend_uart.h"
#include <string.h>
#include "esy_logger_backend_core_internal.h"
#include "esy_logger_backend_os_internal.h"

static void logger_task(void *ctx) {
  for (;;) {
    while (esy_logger_backend_uart_run_one(ctx)) {}
    if (esy_logger_backend_uart_stop_requested(ctx)) {
      /* No context access after this point, including after a native resume. */
      for (;;) (void)esy_os_task_park();
    }
    if (esy_logger_backend_uart_retry_pending(ctx)) esy_os_task_delay_ms(10u);
    else (void)esy_os_task_notify_take(true, UINT32_MAX);
  }
}

static esy_err_t start_worker(void *ctx, const void *config, void **worker) {
  const esy_logger_backend_uart_config_t *cfg = config;
  if (!esy_os_task_park_supported()) return ESY_ERR_NOT_SUPPORTED;
  esy_os_task_t *task = NULL;
  esy_err_t err = esy_os_task_create_static("esy_logger_backend_uart", logger_task, ctx,
      cfg->static_resources->task_stack_buffer_size, cfg->task_priority,
      cfg->static_resources->task_stack_buffer, cfg->static_resources->task_control_storage, &task);
  if (err == ESY_OK) esy_os_task_set_core_affinity(task, cfg->task_core_affinity);
  *worker = task;
  return err;
}

void esy_logger_backend_uart_default_cfg(esy_logger_backend_uart_config_t *cfg) {
  if (!cfg) return;
  esy_logger_backend_uart_core_config_t core;
  esy_logger_backend_uart_core_default_cfg(&core);
  memset(cfg, 0, sizeof(*cfg));
  cfg->task_priority = 6u;
  cfg->task_core_affinity = ESY_CORE_ALL;
  cfg->uart = core.uart;
  cfg->block_size = core.block_size;
  cfg->pool_block_count = core.pool_block_count;
  cfg->dma_short_threshold = core.dma_short_threshold;
}

esy_err_t esy_logger_backend_uart_init(esy_logger_backend_t *backend,
    const esy_logger_backend_uart_config_t *cfg) {
  if (!backend || !cfg || !cfg->static_resources || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (backend->ctx || backend->acquire_buffer || backend->submit_buffer) return ESY_ERR_STATE;
  if (!esy_logger_task_resources_valid(cfg->static_resources->task_stack_buffer,
      cfg->static_resources->task_stack_buffer_size, cfg->static_resources->task_control_storage)) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_uart_core_resources_t resources = {
    .context_storage = cfg->static_resources->context_storage,
    .pool_buffers = cfg->static_resources->pool_buffers,
    .pool_buffer_size = cfg->static_resources->pool_buffer_size,
    .pool_index_stacks = cfg->static_resources->pool_index_stacks,
    .pool_index_stack_count = cfg->static_resources->pool_index_stack_count,
    .ready_ring_slots = cfg->static_resources->ready_ring_slots,
    .ready_ring_slot_count = cfg->static_resources->ready_ring_slot_count,
  };
  esy_logger_backend_uart_core_config_t core = {
    .uart = cfg->uart,
    .block_size = cfg->block_size,
    .pool_block_count = cfg->pool_block_count,
    .dma_short_threshold = cfg->dma_short_threshold,
    .static_resources = &resources,
  };
  esy_logger_backend_runtime_t runtime = esy_logger_os_runtime();
  runtime.start = start_worker;
  return esy_logger_backend_uart_init_with_runtime(backend, &core, &runtime, cfg);
}
