/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_core_lockout_internal.h"

#include <stdbool.h>
#include <stdint.h>

#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/os/esy_os_task.h"
#include "esy_hal/esy_hal_system.h"

#ifndef ESY_CORE_LOCKOUT_TASK_PRIORITY
#define ESY_CORE_LOCKOUT_TASK_PRIORITY 31u
#endif

static esy_core_lockout_ctx_t s_ctx;

static bool timeout_reached(uint32_t start_us, uint32_t timeout_us) {
  uint32_t now_us = esy_hal_system_time_32_us();
  return (uint32_t)(now_us - start_us) >= timeout_us;
}

static uint8_t core_lockout_schedulable_core_count(void) {
  uint8_t platform_core_count = esy_hal_system_get_core_count();
  uint8_t os_core_count = esy_os_core_count();

  return (platform_core_count < os_core_count) ? platform_core_count
                                               : os_core_count;
}

static bool wait_for_worker_sequence(const _Atomic uint32_t *sequence,
                                     uint32_t expected) {
  uint32_t start_us = esy_hal_system_time_32_us();

  while (atomic_load_explicit(sequence, memory_order_acquire) != expected) {
    if (timeout_reached(start_us, CORE_LOCKOUT_SAFE_ZONE_TIMEOUT_US)) {
      return false;
    }
    esy_hal_system_wait_for_event();
  }
  return true;
}

ESY_INTERNAL ESY_HAL_RAM_FUNC void esy_core_lockout_worker_wait(
    core_lockout_worker_t *worker, uint32_t request) {
  /* Only the worker publishes acknowledgements. The caller's release remains
   * visible even when cancellation precedes this IRQ-masked RAM safe zone.
   * Masking is required to prevent interrupt handlers from executing during the safe window.
   */
  uint32_t irq_status = esy_hal_system_irq_save();
  atomic_store_explicit(&worker->locked, request, memory_order_release);
  esy_hal_system_memory_barrier();
  esy_hal_system_signal_event();

  while (atomic_load_explicit(&worker->release, memory_order_acquire) != request) {
    esy_hal_system_wait_for_event();
  }

  esy_hal_system_irq_restore(irq_status);
  atomic_store_explicit(&worker->completed, request, memory_order_release);
  esy_hal_system_memory_barrier();
  esy_hal_system_signal_event();
}

static void core_lockout_worker_task(void *arg) {
  core_lockout_worker_t *worker = (core_lockout_worker_t *)arg;

  for (;;) {
    (void)esy_os_task_notify_take(true, UINT32_MAX);
    uint32_t request = atomic_load_explicit(&worker->request,
                                             memory_order_acquire);
    if (request != atomic_load_explicit(&worker->completed,
                                         memory_order_acquire)) {
      esy_core_lockout_worker_wait(worker, request);
    }
  }
}

static esy_err_t core_lockout_worker_init(esy_core_lockout_ctx_t *ctx, uint32_t core_id) {
  if (core_id >= ESY_CORE_LOCKOUT_MAX_CORES) {
    return ESY_ERR_INVALID_ARG;
  }

  core_lockout_worker_t *worker = &ctx->workers[core_id];
  if (worker->task != NULL) {
    return ESY_OK;
  }

  atomic_init(&worker->request, 0u);
  atomic_init(&worker->release, 0u);
  atomic_init(&worker->locked, 0u);
  atomic_init(&worker->completed, 0u);

  esy_err_t ret = esy_os_task_create_static(
      "core_lockout", core_lockout_worker_task, worker,
      CORE_LOCKOUT_STACK_BYTES, ESY_CORE_LOCKOUT_TASK_PRIORITY,
      worker->task_stack_buffer, &worker->task_control_storage, &worker->task);
  if (ret != ESY_OK) {
    return ret;
  }

  esy_os_task_set_core_affinity(worker->task, (uint8_t)(1u << core_id));
  return ESY_OK;
}

static esy_err_t core_lockout_workers_init(void *arg) {
  esy_core_lockout_ctx_t *ctx = arg;
  if (ctx->workers_initialized) {
    return ESY_OK;
  }

  uint8_t core_count = core_lockout_schedulable_core_count();
  if (core_count <= 1u) {
    ctx->workers_initialized = true;
    return ESY_OK;
  }

  if (core_count > ESY_CORE_LOCKOUT_MAX_CORES) {
    return ESY_ERR_NOT_SUPPORTED;
  }

  for (uint32_t i = 0u; i < (uint32_t)core_count; ++i) {
    esy_err_t ret = core_lockout_worker_init(ctx, i);
    if (ret != ESY_OK) {
      return ret;
    }
  }

  ctx->workers_initialized = true;
  return ESY_OK;
}

static esy_err_t core_lockout_other_core(void *arg, uint32_t *other_core) {
  esy_core_lockout_ctx_t *ctx = arg;
  if (other_core == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  if (!esy_os_started()) {
    *other_core = UINT32_MAX;
    return ESY_OK;
  }

  uint8_t core_count = core_lockout_schedulable_core_count();
  if (core_count <= 1u) {
    *other_core = UINT32_MAX;
    return ESY_OK;
  }

  if (core_count > ESY_CORE_LOCKOUT_MAX_CORES) return ESY_ERR_NOT_SUPPORTED;

  /* A timed-out request still belongs to its worker until completion. Nudge
   * outstanding work again so a lost notification can drain on a later retry.
   * Never reset or reuse live storage, including after a caller changes cores.
   */
  for (uint32_t i = 0u; i < (uint32_t)core_count; ++i) {
    core_lockout_worker_t *pending = &ctx->workers[i];
    if (atomic_load_explicit(&pending->request, memory_order_acquire) !=
        atomic_load_explicit(&pending->completed, memory_order_acquire)) {
      (void)esy_os_task_notify_give(pending->task);
      esy_os_task_yield_core((uint8_t)i);
      esy_hal_system_signal_event();
      return ESY_ERR_BUSY;
    }
  }

  uint32_t self = esy_hal_system_get_core_id();
  if (self >= (uint32_t)core_count) {
    return ESY_ERR_STATE;
  }

  uint32_t other = self ^ 1u;
  if (other >= (uint32_t)core_count) {
    return ESY_ERR_NOT_SUPPORTED;
  }

  core_lockout_worker_t *worker = &ctx->workers[other];
  if (worker->task == NULL) {
    return ESY_ERR_STATE;
  }

  uint32_t request = atomic_load_explicit(&worker->request,
                                          memory_order_relaxed) + 1u;
  atomic_store_explicit(&worker->request, request, memory_order_release);
  esy_err_t ret = esy_os_task_notify_give(worker->task);
  esy_os_task_yield_core((uint8_t)other);
  esy_hal_system_signal_event();

  if ((ret != ESY_OK) || !wait_for_worker_sequence(&worker->locked, request)) {
    atomic_store_explicit(&worker->release, request, memory_order_release);
    esy_hal_system_memory_barrier();
    esy_hal_system_signal_event();
    /* A late worker observes this release even if it has not locked yet.
     * Failure to acknowledge leaves the request outstanding and blocks reuse.
     */
    (void)wait_for_worker_sequence(&worker->completed, request);
    return (ret != ESY_OK) ? ret : ESY_ERR_TIMEOUT;
  }

  *other_core = other;
  return ESY_OK;
}

static esy_err_t core_release_other_core(void *arg, uint32_t other_core) {
  esy_core_lockout_ctx_t *ctx = arg;
  if (other_core == UINT32_MAX) {
    return ESY_OK;
  }

  if (other_core >= ESY_CORE_LOCKOUT_MAX_CORES) {
    return ESY_ERR_INVALID_ARG;
  }

  core_lockout_worker_t *worker = &ctx->workers[other_core];
  uint32_t request = atomic_load_explicit(&worker->request,
                                          memory_order_relaxed);
  atomic_store_explicit(&worker->release, request, memory_order_release);
  esy_hal_system_memory_barrier();
  esy_hal_system_signal_event();

  return wait_for_worker_sequence(&worker->completed, request)
             ? ESY_OK
             : ESY_ERR_TIMEOUT;
}

static const esy_core_lockout_peer_ops_t k_peer_ops = {
  .prepare = core_lockout_workers_init,
  .lock = core_lockout_other_core,
  .release = core_release_other_core,
};

esy_err_t esy_core_lockout_init(void) {
  return esy_core_lockout_initialize(&k_peer_ops, &s_ctx);
}

/* Explicit-context execution seam exercises the real peer provider with caller-
 * constructed state; production execution uses the same engine and k_peer_ops. */
ESY_INTERNAL esy_err_t esy_core_lockout_execute_with_ctx(esy_core_lockout_core_ctx_t *core,
    esy_core_lockout_ctx_t *ctx, esy_core_lockout_operation_t operation, void *arg) {
  if (!core || !ctx) return ESY_ERR_INVALID_ARG;
  core->initialized = ctx->workers_initialized;
  core->peer_ops = &k_peer_ops;
  core->peer_ctx = ctx;
  return esy_core_lockout_core_execute(core, operation, arg);
}
