/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_timer.h
 * @brief OS abstraction layer - timer interface.
 *
 * This module provides an OS-agnostic timer API using caller-provided static
 * storage. Timers can be one-shot or periodic (auto-reload), and can be
 * controlled via start/stop/delete operations.
 *
 * Contract:
 * - Timer callbacks execute in the OS timer context. Keep callbacks short and
 *   non-blocking; offload long work to a task/queue if needed.
 * - For functions returning esy_err_t: ESY_OK indicates success; negative values
 *   indicate failure.
 *
 * Static allocation:
 * - Static timer creation uses caller-provided opaque storage, allowing the
 *   implementation to remain fully opaque while avoiding heap allocation.
 */

#ifndef ESY_OS_TIMER_H
#define ESY_OS_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/os/esy_os_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Compile-time constants                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Static timer storage size in bytes.
 *
 * This is an ABI contract for the OS port layer.
 * Increase this value if the internal implementation grows.
 *
 * The storage is used by @ref esy_os_timer_create_static and must persist for the
 * lifetime of the timer.
 */
#define ESY_OS_TIMER_STATIC_STORAGE_BYTES 256u

/**
 * @brief Static timer storage alignment (bytes).
 *
 * Choose 8 for ESP32 / common 32-bit MCUs, generally safe.
 * If your platform needs stronger alignment, bump this value.
 */
#define ESY_OS_TIMER_STATIC_STORAGE_ALIGN 8u

/* -------------------------------------------------------------------------- */
/* Opaque types                                                               */
/* -------------------------------------------------------------------------- */

/** @brief Timer handle (opaque). */
typedef struct esy_os_timer_s esy_os_timer_t;

/**
 * @brief Static timer storage object.
 *
 * The caller owns this object and must keep it alive for the lifetime of the timer.
 * The internal layout is implementation-defined.
 *
 * This enables fully opaque static allocation without exposing internal structs.
 */
typedef struct esy_os_timer_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_TIMER_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_TIMER_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_TIMER_STATIC_STORAGE_BYTES];
} esy_os_timer_static_storage_t;

/* -------------------------------------------------------------------------- */
/* Callback type                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Timer callback function type.
 *
 * The callback runs in the OS timer context.
 *
 * @param[in] arg User-defined argument passed at timer creation time.
 */
typedef void (*esy_os_timer_callback_t)(void *arg);

/* -------------------------------------------------------------------------- */
/* Creation / deletion                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create a statically allocated timer (no dynamic allocation).
 *
 * Uses caller-provided opaque storage.
 *
 * @param[in]  name         Timer name (may be NULL).
 * @param[in]  period_ms    Nonzero period in milliseconds, rounded up to a tick.
 * @param[in]  auto_reload  true for periodic timers; false for one-shot timers.
 * @param[in]  arg          User argument passed back to @p callback.
 * @param[in]  callback     Timer callback function (must not be NULL).
 * @param[in]  storage      Caller-provided opaque static storage object.
 *                          Must persist for the lifetime of the timer.
 * @param[out] out_timer    Output: created timer handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, unsupported, OS error, etc.).
 */
extern esy_err_t esy_os_timer_create_static(const char *name,
                                           uint32_t period_ms,
                                           bool auto_reload,
                                           void *arg,
                                           esy_os_timer_callback_t callback,
                                           esy_os_timer_static_storage_t *storage,
                                           esy_os_timer_t **out_timer);

/**
 * @brief Delete a timer.
 *
 * Non-blocking, pollable cleanup. ESY_ERR_BUSY means a command or completion
 * fence is outstanding: retain storage and callback context and retry delete.
 * Only ESY_OK permits reclaiming/recreating the object. Calls are task-context
 * only, including timer callbacks, and must have one externally serialized owner.
 * Do not spin waiting inside a callback; let the daemon process the fence.
 * Finish an outstanding stop before requesting delete (otherwise ESY_ERR_STATE).
 * Requires INCLUDE_xTimerPendFunctionCall=1. Before scheduler start, completion
 * remains BUSY until the daemon has run. No callback may rearm during cleanup.
 * @retval ESY_OK Cleanup complete; storage remains caller-owned.
 * @retval ESY_ERR_BUSY Retry without reclaiming storage.
 * @retval ESY_ERR_STATE A different cleanup operation is pending.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 *
 * @param[in] timer Timer handle.
 */
extern esy_err_t esy_os_timer_delete(esy_os_timer_t *timer);

/* -------------------------------------------------------------------------- */
/* Control                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Start a timer.
 *
 * Non-blocking command submission, safe in a serialized timer callback.
 * ESY_ERR_BUSY means the queue is full or cleanup is pending; retry later.
 * No command was accepted in that case.
 * Timing starts from "now". For a periodic timer that is already running, the
 * period is reloaded/restarted (implementation-defined but typically restarts).
 *
 * @param[in] timer Timer handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, invalid state, OS error, etc.).
 */
extern esy_err_t esy_os_timer_start(esy_os_timer_t *timer);

/**
 * @brief Stop a timer.
 *
 * Poll until ESY_OK using the same ownership and fence rules as delete.
 * ESY_ERR_BUSY retains the timer and context; callbacks may still execute.
 * After ESY_OK no prior callback remains, and start may rearm the timer.
 * @retval ESY_OK Stop and preceding callbacks completed.
 * @retval ESY_ERR_BUSY Retry later; cleanup is not complete.
 * @retval ESY_ERR_STATE Delete is already pending.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 *
 * @param[in] timer Timer handle.
 */
extern esy_err_t esy_os_timer_stop(esy_os_timer_t *timer);

#ifdef __cplusplus
}
#endif

#endif /* ESY_OS_TIMER_H */
