/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_WATCHDOG_MONITOR_INTERNAL_H
#define ESY_WATCHDOG_MONITOR_INTERNAL_H

#include <stdatomic.h>
#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor_core.h"

typedef struct esy_watchdog_monitor_ctx_s esy_watchdog_monitor_ctx_t;

/* Module-private scheduling and MPMC synchronization, installed only by the OS
 * adapter. Caller-driven instances have NULL hooks and one non-ISR owner. */
typedef struct {
  void (*lock)(void);
  void (*unlock)(void);
  esy_err_t (*start)(esy_watchdog_monitor_ctx_t *ctx);
  esy_err_t (*stop)(esy_watchdog_monitor_ctx_t *ctx);
  bool (*is_worker)(const void *worker);
} esy_watchdog_monitor_runtime_t;

typedef struct {
  void *task_stack_buffer;
  uint32_t task_stack_buffer_size;
  void *task_control_storage;
  uint32_t task_priority;
  uint8_t task_core_affinity;
} esy_watchdog_monitor_worker_resources_t;

struct esy_watchdog_monitor_ctx_s {
  void *monitor_task;
  uint32_t check_period_ms;
  uint32_t hw_timeout_ms;
  uint32_t task_priority;
  uint8_t task_core_affinity;
  void *task_stack_buffer;
  uint32_t task_stack_buffer_size;
  void *task_control_storage;
  esy_watchdog_monitor_task_slot_t *task_slots;
  uint32_t task_slot_count;
  uint32_t registered_count;
  uint32_t generation_seed;
  esy_watchdog_monitor_runtime_t runtime;
  uint32_t last_check_ms;
  bool processing;
  bool initialized;
  bool running;
  bool stop_requested;
  bool hardware_active;
  bool hardware_paused;
  bool feed_inflight;
  bool hw_feed_enabled;
  bool callback_active;
  esy_watchdog_alert_cb_t alert_cb;
  void *alert_user_ctx;
  esy_monotonic_clock_t clock;
  uint64_t timeout_lower_bound_us;
  atomic_uint deadline_sequence;
  atomic_uint deadline_low;
  atomic_uint deadline_high;
  atomic_bool deadline_valid;
};

/* Lifecycle owner excludes concurrent init/deinit while checking this state. */
bool esy_watchdog_monitor_initialized(void);
esy_err_t esy_watchdog_monitor_init_with_runtime(
    const esy_watchdog_monitor_core_config_t *cfg,
    const esy_watchdog_monitor_runtime_t *runtime,
    const esy_watchdog_monitor_worker_resources_t *worker);
esy_err_t esy_watchdog_monitor_start_worker(void);
esy_err_t esy_watchdog_monitor_process_context(esy_watchdog_monitor_ctx_t *ctx);
uint32_t esy_watchdog_monitor_wait_ms(esy_watchdog_monitor_ctx_t *ctx);
void esy_watchdog_monitor_state_lock(esy_watchdog_monitor_ctx_t *ctx);
void esy_watchdog_monitor_state_unlock(esy_watchdog_monitor_ctx_t *ctx);

/* Single monitor endpoint; lifecycle owner excludes reclamation during calls. */
ESY_INTERNAL void esy_watchdog_monitor_check_once(esy_watchdog_monitor_ctx_t *ctx);
ESY_INTERNAL bool esy_watchdog_monitor_iteration(esy_watchdog_monitor_ctx_t *ctx);
#endif
