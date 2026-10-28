/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_watchdog_monitor_internal.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include "esy_hal/esy_hal_watchdog.h"
#include "esy_hal/esy_hal_system.h"

#define ESY_WATCHDOG_MONITOR_REPORT_MAGIC 0x57444D52u
#define ESY_WATCHDOG_DEADLINE_READ_ATTEMPTS 3u

typedef struct {
  uint32_t generation;
  bool heartbeat;
  bool last_alive;
  bool check_enabled;
  char name[ESY_WATCHDOG_MONITOR_TASK_NAME_BYTES];
} esy_watchdog_task_info_t;

_Static_assert(sizeof(esy_watchdog_task_info_t) <= ESY_WATCHDOG_MONITOR_TASK_SLOT_BYTES,
               "Watchdog slot storage is too small");
_Static_assert(_Alignof(esy_watchdog_task_info_t) <= ESY_WATCHDOG_MONITOR_TASK_SLOT_ALIGN,
               "Watchdog slot alignment is too small");

static esy_watchdog_monitor_ctx_t s_ctx;

/* MPMC task callers and the monitor share slot ownership, heartbeat consumption,
 * callback admission and lifecycle handoff. Per-field atomics cannot commit
 * these compound transitions. The OS critical section excludes every core;
 * each section handles at most one fixed-size slot. Caller-driven execution has
 * one non-ISR owner and needs no lock. No callbacks, hardware/clock
 * calls, task waits or allocation are performed while it is held. */
void esy_watchdog_monitor_state_lock(esy_watchdog_monitor_ctx_t *ctx) {
  if (ctx->runtime.lock) ctx->runtime.lock();
}
void esy_watchdog_monitor_state_unlock(esy_watchdog_monitor_ctx_t *ctx) {
  if (ctx->runtime.unlock) ctx->runtime.unlock();
}

static esy_watchdog_task_info_t *slot_info(esy_watchdog_monitor_ctx_t *ctx,
                                          uint32_t index) {
  return (esy_watchdog_task_info_t *)(void *)ctx->task_slots[index].bytes;
}

/* Caller holds the state lock. Report storage itself must remain immutable
 * while another caller reads it; independent value copies are supported. */
static esy_watchdog_task_info_t *report_info(esy_watchdog_monitor_ctx_t *ctx,
                                            const esy_watchdog_report_t *report) {
  if (report == NULL || report->magic != ESY_WATCHDOG_MONITOR_REPORT_MAGIC ||
      report->generation == 0u || report->slot_index >= ctx->task_slot_count)
    return NULL;
  esy_watchdog_task_info_t *info = slot_info(ctx, report->slot_index);
  return info->generation == report->generation ? info : NULL;
}

static void publish_deadline(esy_watchdog_monitor_ctx_t *ctx, bool valid,
                              uint64_t before_feed_us) {
  uint64_t bound = ctx->timeout_lower_bound_us;
  uint64_t deadline = before_feed_us > UINT64_MAX - bound
                          ? UINT64_MAX : before_feed_us + bound;
  /* All fields participate in one SC order. The bounded reader never waits
   * for a writer preempted by an interrupt-masked power-manager query. */
  atomic_fetch_add(&ctx->deadline_sequence, 1u);
  atomic_store(&ctx->deadline_low, (uint32_t)deadline);
  atomic_store(&ctx->deadline_high, (uint32_t)(deadline >> 32u));
  atomic_store(&ctx->deadline_valid, valid);
  atomic_fetch_add(&ctx->deadline_sequence, 1u);
}

static bool read_feed_time(esy_watchdog_monitor_ctx_t *ctx, uint64_t *now_us) {
  return ctx->timeout_lower_bound_us != 0u && ctx->clock.ops != NULL &&
         ctx->clock.ops->read_atomic(ctx->clock.ctx, now_us) == ESY_OK;
}

ESY_INTERNAL void esy_watchdog_monitor_check_once(esy_watchdog_monitor_ctx_t *ctx) {
  if (ctx == NULL) return;
  esy_watchdog_alert_event_t event = {0};
  char alert_name[ESY_WATCHDOG_MONITOR_TASK_NAME_BYTES] = {0};
  bool all_alive = true;
  for (uint32_t i = 0u; i < ctx->task_slot_count; ++i) {
    esy_watchdog_monitor_state_lock(ctx);
    esy_watchdog_task_info_t *info = slot_info(ctx, i);
    if (info->generation != 0u && info->check_enabled) {
      info->last_alive = info->heartbeat;
      info->heartbeat = false;
      if (!info->last_alive) {
        all_alive = false;
        if (event.task_name == NULL) {
          memcpy(alert_name, info->name, sizeof(alert_name));
          event.task_name = alert_name;
          event.reason = ESY_WATCHDOG_ALERT_REASON_MISSED_HEARTBEAT;
          event.report = (esy_watchdog_report_t){i, info->generation,
                                               ESY_WATCHDOG_MONITOR_REPORT_MAGIC};
          event.registered_task_count = ctx->registered_count;
          event.check_period_ms = ctx->check_period_ms;
        }
      }
    }
    esy_watchdog_monitor_state_unlock(ctx);
  }

  esy_watchdog_monitor_state_lock(ctx);
  esy_watchdog_alert_cb_t cb = event.task_name != NULL ? ctx->alert_cb : NULL;
  void *user_ctx = ctx->alert_user_ctx;
  if (cb != NULL) ctx->callback_active = true;
  bool feed = all_alive && ctx->hw_feed_enabled && ctx->hardware_active &&
              !ctx->hardware_paused && !ctx->stop_requested;
  if (feed) ctx->feed_inflight = true;
  esy_watchdog_monitor_state_unlock(ctx);

  if (cb != NULL) {
    cb(&event, user_ctx);
    esy_watchdog_monitor_state_lock(ctx);
    ctx->callback_active = false;
    esy_watchdog_monitor_state_unlock(ctx);
  }
  if (feed) {
    uint64_t before_feed_us = 0u;
    bool valid = read_feed_time(ctx, &before_feed_us);
    esy_hal_watchdog_feed();
    publish_deadline(ctx, valid, before_feed_us);
    esy_watchdog_monitor_state_lock(ctx);
    ctx->feed_inflight = false;
    esy_watchdog_monitor_state_unlock(ctx);
  }
}

void esy_watchdog_monitor_core_default_cfg(esy_watchdog_monitor_core_config_t *cfg) {
  if (cfg == NULL) return;
  *cfg = (esy_watchdog_monitor_core_config_t){
      .check_period_ms = 1000u, .hw_timeout_ms = 5000u};
}

static esy_err_t validate_config(const esy_watchdog_monitor_core_config_t *cfg) {
  if (cfg == NULL || cfg->check_period_ms == 0u || cfg->hw_timeout_ms == 0u ||
      cfg->check_period_ms >= cfg->hw_timeout_ms ||
      cfg->check_period_ms > UINT32_MAX / 2u || cfg->static_resources == NULL)
    return ESY_ERR_INVALID_ARG;
  const esy_watchdog_monitor_core_resources_t *r = cfg->static_resources;
  if (r->task_slots == NULL || r->task_slot_count == 0u ||
      r->task_slot_count > INT32_MAX ||
      (uintptr_t)r->task_slots % ESY_WATCHDOG_MONITOR_TASK_SLOT_ALIGN != 0u ||
      (cfg->clock.ops != NULL && cfg->clock.ops->read_atomic == NULL))
    return ESY_ERR_INVALID_ARG;
  if (cfg->clock.ops != NULL &&
      (!atomic_is_lock_free(&s_ctx.deadline_sequence) ||
       !atomic_is_lock_free(&s_ctx.deadline_low) ||
       !atomic_is_lock_free(&s_ctx.deadline_high) ||
       !atomic_is_lock_free(&s_ctx.deadline_valid))) return ESY_ERR_NOT_SUPPORTED;
  return ESY_OK;
}

esy_err_t esy_watchdog_monitor_init_with_runtime(
    const esy_watchdog_monitor_core_config_t *cfg,
    const esy_watchdog_monitor_runtime_t *runtime,
    const esy_watchdog_monitor_worker_resources_t *worker) {
  if (s_ctx.initialized) return ESY_ERR_STATE;
  esy_err_t ret = validate_config(cfg);
  if (ret != ESY_OK) return ret;
  uint64_t before_init_us = 0u;
  bool clock_valid = cfg->clock.ops != NULL &&
      cfg->clock.ops->read_atomic(cfg->clock.ctx, &before_init_us) == ESY_OK;
  ret = esy_hal_watchdog_init(cfg->hw_timeout_ms, true);
  if (ret != ESY_OK) return ret;

  const esy_watchdog_monitor_core_resources_t *r = cfg->static_resources;
  for (uint32_t i = 0u; i < r->task_slot_count; ++i)
    memset(r->task_slots[i].bytes, 0, sizeof(r->task_slots[i].bytes));
  s_ctx.check_period_ms = cfg->check_period_ms;
  s_ctx.hw_timeout_ms = cfg->hw_timeout_ms;
  s_ctx.clock = cfg->clock;
  if (runtime != NULL) s_ctx.runtime = *runtime;
  if (worker != NULL) {
    s_ctx.task_stack_buffer = worker->task_stack_buffer;
    s_ctx.task_stack_buffer_size = worker->task_stack_buffer_size;
    s_ctx.task_control_storage = worker->task_control_storage;
    s_ctx.task_priority = worker->task_priority;
    s_ctx.task_core_affinity = worker->task_core_affinity;
  }
  s_ctx.task_slots = r->task_slots;
  s_ctx.task_slot_count = r->task_slot_count;
  s_ctx.initialized = true;
  s_ctx.hardware_active = true;
  s_ctx.hw_feed_enabled = true;
  uint64_t bound = 0u;
  if (esy_hal_watchdog_get_timeout_lower_bound_us(&bound) == ESY_OK)
    s_ctx.timeout_lower_bound_us = bound;
  publish_deadline(&s_ctx, clock_valid && s_ctx.timeout_lower_bound_us != 0u,
                   before_init_us);
  return ESY_OK;
}

bool esy_watchdog_monitor_initialized(void) { return s_ctx.initialized; }

esy_err_t esy_watchdog_monitor_core_init(const esy_watchdog_monitor_core_config_t *cfg) {
  return esy_watchdog_monitor_init_with_runtime(cfg, NULL, NULL);
}

static esy_err_t start_execution(bool worker) {
  if (!s_ctx.initialized || s_ctx.running || s_ctx.monitor_task != NULL ||
      !s_ctx.hardware_active || (s_ctx.runtime.start != NULL) != worker)
    return ESY_ERR_STATE;
  s_ctx.last_check_ms = esy_hal_system_get_tick();
  s_ctx.stop_requested = false;
  s_ctx.running = true;
  if (worker) {
    esy_err_t ret = s_ctx.runtime.start(&s_ctx);
    if (ret != ESY_OK) {
      s_ctx.running = false;
      return ret;
    }
  }
  return ESY_OK;
}

esy_err_t esy_watchdog_monitor_core_start(void) { return start_execution(false); }
esy_err_t esy_watchdog_monitor_start_worker(void) { return start_execution(true); }

uint32_t esy_watchdog_monitor_wait_ms(esy_watchdog_monitor_ctx_t *ctx) {
  uint32_t elapsed = (uint32_t)(esy_hal_system_get_tick() - ctx->last_check_ms);
  return elapsed < ctx->check_period_ms ? ctx->check_period_ms - elapsed : 0u;
}

esy_err_t esy_watchdog_monitor_process_context(esy_watchdog_monitor_ctx_t *ctx) {
  esy_watchdog_monitor_state_lock(ctx);
  bool allowed = ctx->initialized && ctx->running && !ctx->stop_requested &&
                 !ctx->processing;
  esy_watchdog_monitor_state_unlock(ctx);
  if (!allowed) return ESY_ERR_STATE;
  uint32_t now_ms = esy_hal_system_get_tick();
  if ((uint32_t)(now_ms - ctx->last_check_ms) < ctx->check_period_ms) return ESY_OK;
  ctx->last_check_ms = now_ms;
  esy_watchdog_monitor_state_lock(ctx);
  ctx->processing = true;
  esy_watchdog_monitor_state_unlock(ctx);
  esy_watchdog_monitor_check_once(ctx);
  esy_watchdog_monitor_state_lock(ctx);
  ctx->processing = false;
  esy_watchdog_monitor_state_unlock(ctx);
  return ESY_OK;
}

esy_err_t esy_watchdog_monitor_core_process(void) {
  if (s_ctx.runtime.start != NULL) return ESY_ERR_STATE;
  return esy_watchdog_monitor_process_context(&s_ctx);
}

esy_err_t esy_watchdog_monitor_stop(void) {
  if (!s_ctx.initialized) return ESY_OK;
  if ((s_ctx.runtime.is_worker && s_ctx.runtime.is_worker(s_ctx.monitor_task)) ||
      (!s_ctx.runtime.start && s_ctx.processing)) return ESY_ERR_STATE;
  esy_watchdog_monitor_state_lock(&s_ctx);
  s_ctx.stop_requested = true;
  esy_watchdog_monitor_state_unlock(&s_ctx);
  if (s_ctx.runtime.stop && s_ctx.monitor_task != NULL) {
    esy_err_t ret = s_ctx.runtime.stop(&s_ctx);
    if (ret != ESY_OK) return ret;
  }
  s_ctx.monitor_task = NULL;
  s_ctx.running = false;
  return ESY_OK;
}

esy_err_t esy_watchdog_monitor_deinit(void) {
  if (!s_ctx.initialized) return ESY_OK;
  if ((s_ctx.runtime.is_worker && s_ctx.runtime.is_worker(s_ctx.monitor_task)) ||
      (!s_ctx.runtime.start && s_ctx.processing)) return ESY_ERR_STATE;
  esy_watchdog_monitor_state_lock(&s_ctx);
  if (s_ctx.feed_inflight || s_ctx.callback_active) {
    esy_watchdog_monitor_state_unlock(&s_ctx);
    return ESY_ERR_BUSY;
  }
  s_ctx.hardware_paused = true;
  esy_watchdog_monitor_state_unlock(&s_ctx);
  if (s_ctx.hardware_active) {
    esy_err_t ret = esy_hal_watchdog_deinit();
    if (ret != ESY_OK) {
      esy_watchdog_monitor_state_lock(&s_ctx);
      s_ctx.hardware_paused = false;
      esy_watchdog_monitor_state_unlock(&s_ctx);
      return ret;
    }
    esy_watchdog_monitor_state_lock(&s_ctx);
    s_ctx.hardware_active = false;
    esy_watchdog_monitor_state_unlock(&s_ctx);
  }
  esy_err_t ret = esy_watchdog_monitor_stop();
  if (ret != ESY_OK) return ret;
  /* Tokens never repeat during this boot, including across init/deinit. */
  uint32_t generation_seed = s_ctx.generation_seed;
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.generation_seed = generation_seed;
  return ESY_OK;
}

esy_err_t esy_watchdog_monitor_register(const char *task_name,
                                        esy_watchdog_report_t *out_report) {
  if (task_name == NULL || out_report == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  char name[ESY_WATCHDOG_MONITOR_TASK_NAME_BYTES] = {0};
  for (uint32_t i = 0u; i < sizeof(name) - 1u && task_name[i] != '\0'; ++i)
    name[i] = task_name[i];
  for (uint32_t i = 0u; i < s_ctx.task_slot_count; ++i) {
    esy_watchdog_monitor_state_lock(&s_ctx);
    esy_watchdog_task_info_t *info = slot_info(&s_ctx, i);
    if (info->generation == 0u && s_ctx.generation_seed != UINT32_MAX) {
      *info = (esy_watchdog_task_info_t){.generation = ++s_ctx.generation_seed,
                                       .check_enabled = true};
      memcpy(info->name, name, sizeof(name));
      ++s_ctx.registered_count;
      *out_report = (esy_watchdog_report_t){i, info->generation,
                                          ESY_WATCHDOG_MONITOR_REPORT_MAGIC};
      esy_watchdog_monitor_state_unlock(&s_ctx);
      return ESY_OK;
    }
    esy_watchdog_monitor_state_unlock(&s_ctx);
  }
  return ESY_ERR_NO_MEM;
}

void esy_watchdog_monitor_unregister(esy_watchdog_report_t *report) {
  if (!s_ctx.initialized || report == NULL) return;
  esy_watchdog_monitor_state_lock(&s_ctx);
  esy_watchdog_task_info_t *info = report_info(&s_ctx, report);
  if (info != NULL) {
    memset(info, 0, sizeof(*info));
    --s_ctx.registered_count;
    memset(report, 0, sizeof(*report));
  }
  esy_watchdog_monitor_state_unlock(&s_ctx);
}

esy_err_t esy_watchdog_monitor_enable_task_check(const esy_watchdog_report_t *report) {
  if (report == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  esy_watchdog_monitor_state_lock(&s_ctx);
  esy_watchdog_task_info_t *info = report_info(&s_ctx, report);
  if (info != NULL) info->check_enabled = true;
  esy_watchdog_monitor_state_unlock(&s_ctx);
  return info != NULL ? ESY_OK : ESY_ERR_INVALID_ARG;
}

void esy_watchdog_monitor_disable_task_check(const esy_watchdog_report_t *report) {
  if (!s_ctx.initialized || report == NULL) return;
  esy_watchdog_monitor_state_lock(&s_ctx);
  esy_watchdog_task_info_t *info = report_info(&s_ctx, report);
  if (info != NULL) info->check_enabled = false;
  esy_watchdog_monitor_state_unlock(&s_ctx);
}

esy_err_t esy_watchdog_monitor_get_task_status(const esy_watchdog_report_t *report,
                                              bool *out_alive) {
  if (report == NULL || out_alive == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  esy_watchdog_monitor_state_lock(&s_ctx);
  esy_watchdog_task_info_t *info = report_info(&s_ctx, report);
  if (info != NULL) *out_alive = info->last_alive;
  esy_watchdog_monitor_state_unlock(&s_ctx);
  return info != NULL ? ESY_OK : ESY_ERR_INVALID_ARG;
}

void esy_watchdog_monitor_report(const esy_watchdog_report_t *report) {
  if (!s_ctx.initialized || report == NULL) return;
  esy_watchdog_monitor_state_lock(&s_ctx);
  esy_watchdog_task_info_t *info = report_info(&s_ctx, report);
  if (info != NULL) info->heartbeat = true;
  esy_watchdog_monitor_state_unlock(&s_ctx);
}

int32_t esy_watchdog_monitor_get_task_count(void) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  esy_watchdog_monitor_state_lock(&s_ctx);
  int32_t count = (int32_t)s_ctx.registered_count;
  esy_watchdog_monitor_state_unlock(&s_ctx);
  return count;
}

void esy_watchdog_monitor_disable_hw_feed(void) {
  if (!s_ctx.initialized) return;
  esy_watchdog_monitor_state_lock(&s_ctx);
  s_ctx.hw_feed_enabled = false;
  esy_watchdog_monitor_state_unlock(&s_ctx);
}

esy_err_t esy_watchdog_monitor_set_alert_cb(esy_watchdog_alert_cb_t cb, void *user_ctx) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  esy_watchdog_monitor_state_lock(&s_ctx);
  if (s_ctx.callback_active) {
    esy_watchdog_monitor_state_unlock(&s_ctx);
    return ESY_ERR_BUSY;
  }
  s_ctx.alert_cb = cb;
  s_ctx.alert_user_ctx = user_ctx;
  esy_watchdog_monitor_state_unlock(&s_ctx);
  return ESY_OK;
}

esy_err_t esy_watchdog_monitor_get_hard_deadline_atomic(
    const esy_monotonic_clock_t *clock, uint64_t *out_deadline_us) {
  if (clock == NULL || out_deadline_us == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.initialized || clock->ops == NULL || clock->ops != s_ctx.clock.ops ||
      clock->ctx != s_ctx.clock.ctx) return ESY_ERR_STATE;
  if (s_ctx.timeout_lower_bound_us == 0u) return ESY_ERR_NOT_SUPPORTED;
  for (uint32_t attempt = 0u; attempt < ESY_WATCHDOG_DEADLINE_READ_ATTEMPTS; ++attempt) {
    uint32_t before = atomic_load(&s_ctx.deadline_sequence);
    if ((before & 1u) != 0u) continue;
    uint32_t low = atomic_load(&s_ctx.deadline_low);
    uint32_t high = atomic_load(&s_ctx.deadline_high);
    bool valid = atomic_load(&s_ctx.deadline_valid);
    uint32_t after = atomic_load(&s_ctx.deadline_sequence);
    if (before == after) {
      if (!valid) return ESY_ERR_STATE;
      *out_deadline_us = ((uint64_t)high << 32u) | low;
      return ESY_OK;
    }
  }
  return ESY_ERR_BUSY;
}
