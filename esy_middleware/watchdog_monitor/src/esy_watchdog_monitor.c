/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor.h"
#include "esy_watchdog_monitor_internal.h"

static esy_err_t start_worker(esy_watchdog_monitor_ctx_t *ctx);
static esy_err_t stop_worker(esy_watchdog_monitor_ctx_t *ctx) {
  (void)esy_os_task_notify_give(ctx->monitor_task);
  return esy_os_task_delete_parked(ctx->monitor_task);
}
static bool is_worker(const void *worker) { return esy_os_task_is_current(worker); }

ESY_INTERNAL bool esy_watchdog_monitor_iteration(esy_watchdog_monitor_ctx_t *ctx) {
  if (ctx == NULL) return false;
  esy_watchdog_monitor_state_lock(ctx);
  bool stop = ctx->stop_requested;
  esy_watchdog_monitor_state_unlock(ctx);
  if (stop) return false;
  (void)esy_os_task_notify_take(true, esy_watchdog_monitor_wait_ms(ctx));
  esy_watchdog_monitor_state_lock(ctx);
  stop = ctx->stop_requested;
  esy_watchdog_monitor_state_unlock(ctx);
  if (stop) return false;
  (void)esy_watchdog_monitor_process_context(ctx);
  return true;
}

static void monitor_task(void *param) {
  esy_watchdog_monitor_ctx_t *ctx = param;
  while (esy_watchdog_monitor_iteration(ctx)) {}
  /* Never access ctx or resume processing after parking, even on native resume. */
  for (;;) (void)esy_os_task_park();
}

static esy_err_t start_worker(esy_watchdog_monitor_ctx_t *ctx) {
  esy_os_task_t *task = NULL;
  esy_err_t ret = esy_os_task_create_static(
      "watchdog_monitor", monitor_task, ctx, ctx->task_stack_buffer_size,
      ctx->task_priority, ctx->task_stack_buffer, ctx->task_control_storage, &task);
  if (ret == ESY_OK) {
    ctx->monitor_task = task;
    esy_os_task_set_core_affinity(task, ctx->task_core_affinity);
  }
  return ret;
}

void esy_watchdog_monitor_default_cfg(esy_watchdog_monitor_config_t *cfg) {
  if (cfg == NULL) return;
  esy_watchdog_monitor_core_config_t core;
  esy_watchdog_monitor_core_default_cfg(&core);
  *cfg = (esy_watchdog_monitor_config_t){
      .check_period_ms = core.check_period_ms, .hw_timeout_ms = core.hw_timeout_ms,
      .task_priority = 5u, .task_core_affinity = ESY_CORE_ALL};
}

esy_err_t esy_watchdog_monitor_init(const esy_watchdog_monitor_config_t *cfg) {
  if (esy_watchdog_monitor_initialized()) return ESY_ERR_STATE;
  if (!cfg || !cfg->static_resources) return ESY_ERR_INVALID_ARG;
  const esy_watchdog_monitor_static_resources_t *r = cfg->static_resources;
  if (!r->task_stack_buffer || !r->task_stack_buffer_size || !r->task_control_storage ||
      (uintptr_t)r->task_stack_buffer % ESY_OS_TASK_STACK_ALIGN != 0u ||
      (uintptr_t)r->task_control_storage % ESY_OS_TASK_STATIC_STORAGE_ALIGN != 0u)
    return ESY_ERR_INVALID_ARG;
  if (!esy_os_task_park_supported()) return ESY_ERR_NOT_SUPPORTED;
  esy_watchdog_monitor_core_resources_t resources = {
      .task_slots = r->task_slots, .task_slot_count = r->task_slot_count};
  esy_watchdog_monitor_core_config_t core = {
      .check_period_ms = cfg->check_period_ms, .hw_timeout_ms = cfg->hw_timeout_ms,
      .clock = cfg->clock, .static_resources = &resources};
  esy_watchdog_monitor_worker_resources_t worker = {
      .task_stack_buffer = r->task_stack_buffer,
      .task_stack_buffer_size = r->task_stack_buffer_size,
      .task_control_storage = r->task_control_storage,
      .task_priority = cfg->task_priority, .task_core_affinity = cfg->task_core_affinity};
  esy_watchdog_monitor_runtime_t runtime = {
      .lock = esy_os_task_enter_critical, .unlock = esy_os_task_exit_critical,
      .start = start_worker, .stop = stop_worker, .is_worker = is_worker};
  return esy_watchdog_monitor_init_with_runtime(&core, &runtime, &worker);
}

esy_err_t esy_watchdog_monitor_start(void) { return esy_watchdog_monitor_start_worker(); }
