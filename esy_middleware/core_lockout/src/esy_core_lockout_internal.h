/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_CORE_LOCKOUT_INTERNAL_H
#define ESY_CORE_LOCKOUT_INTERNAL_H

#include <stdatomic.h>
#include "esy_middleware/core_lockout/esy_core_lockout.h"
#include "esy_core_lockout_core_internal.h"
#include "esy_middleware/os/esy_os_task.h"
#include "esy_hal/esy_hal_system.h"

#define CORE_LOCKOUT_SAFE_ZONE_TIMEOUT_US 1000000u
#define CORE_LOCKOUT_STACK_BYTES 1024u

/** @brief Internal SPSC handshake and permanent worker storage.
 * Externally serialized operation callers own request/release; this worker owns
 * locked/completed. A request number is not reused until completed matches it.
 * Atomic loads/stores, rather than read-modify-write operations, keep the RAM
 * wait path suitable for ports without lock-free compare-and-exchange.
 */
typedef struct {
  esy_os_task_t *task;
  esy_os_task_static_storage_t task_control_storage;
  _Alignas(ESY_OS_TASK_STACK_ALIGN)
      uint32_t task_stack_buffer[CORE_LOCKOUT_STACK_BYTES / sizeof(uint32_t)];
  _Atomic uint32_t request;
  _Atomic uint32_t release;
  _Atomic uint32_t locked;
  _Atomic uint32_t completed;
} core_lockout_worker_t;

/** @brief Internal context; lifecycle changes require quiescent callers. */
typedef struct {
  core_lockout_worker_t workers[ESY_CORE_LOCKOUT_MAX_CORES];
  bool workers_initialized;
} esy_core_lockout_ctx_t;

/** @brief Internal worker endpoint; acknowledge and drain one request in RAM.
 * @param worker Worker-owned storage, valid for the task lifetime.
 * @param request Request captured by this worker after notification.
 */
ESY_INTERNAL ESY_HAL_RAM_FUNC void esy_core_lockout_worker_wait(
    core_lockout_worker_t *worker, uint32_t request);

/** @brief Internal execution seam using explicit context and public primitives.
 * @param core Explicit guard/fault state; caller initializes it before use.
 * @param ctx Ready worker state, or an unready context to reject execution.
 * @param operation Synchronous operation callback; NULL is invalid.
 * @param arg Borrowed callback argument.
 * @return ESY_OK or the operation admission/cleanup error.
 */
ESY_INTERNAL esy_err_t esy_core_lockout_execute_with_ctx(
    esy_core_lockout_core_ctx_t *core, esy_core_lockout_ctx_t *ctx,
    esy_core_lockout_operation_t operation,
    void *arg);

#endif /* ESY_CORE_LOCKOUT_INTERNAL_H */
