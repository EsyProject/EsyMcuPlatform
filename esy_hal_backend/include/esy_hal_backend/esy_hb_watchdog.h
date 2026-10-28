/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_watchdog.h
 * @brief HAL backend watchdog timer interface.
 *
 * This header wraps the low-level hardware watchdog operations, including
 * initialization, feeding (reloading), enabling/disabling, and reset-cause
 * inspection.
 *
 * Conventions:
 * - APIs that can fail use @ref esy_err_t: 0 means success, negative values
 *   are error codes.
 * - Platform decides the maximum timeout range and whether "pause while
 *   debugging" is supported.
 */

#ifndef ESY_HB_WATCHDOG_H
#define ESY_HB_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the watchdog timer.
 *
 * Configures the watchdog timeout and whether the counter should pause while
 * the CPU is halted by a debugger. After this call, the watchdog starts running
 * immediately and begins counting down.
 *
 * @param[in] timeout_ms     Timeout in milliseconds. The maximum value is
 *                           hardware/platform dependent.
 * @param[in] pause_on_debug true to pause while debugging; false to keep
 *                           counting even under debugger halt.
 *
 * @retval ESY_OK Success
 * @retval ESY_ERR_INVALID_ARG @p timeout_ms is 0 or outside the port range.
 * @retval <0     Other platform-specific failure
 *
 * @note Supported nominal ranges are 1..8000 ms (Pico), 1..8190 ms
 *       (STM32F4), and 1..UINT32_MAX/150 ms (ESP32). Out-of-range values
 *       are rejected rather than silently clamped. These are programming
 *       ranges, not certified timing bounds. Reinitializing a live ESP32
 *       watchdog returns ESY_ERR_STATE; serialize ownership on every port.
 * @note If the platform does not support "pause on debug", @p pause_on_debug
 *       may be ignored.
 * @warning On some platforms, once the watchdog is enabled it cannot be fully
 *          disabled; it can only be reloaded or the system must be reset.
 */
extern esy_err_t esy_hb_watchdog_init(uint32_t timeout_ms, bool pause_on_debug);

/**
 * @brief Deinitialize (disable) the watchdog timer.
 *
 * Task-context, single-owner operation. ESY_OK means the counter is stopped.
 * ESY_ERR_NOT_SUPPORTED leaves an irreversible watchdog running; retain
 * ownership and keep feeding or perform system recovery. Other errors retain
 * ownership too. Repeated successful cleanup is harmless.
 *
 * @note Whether disabling is allowed after enabling is platform dependent.
 *       An unsupported operation returns ESY_ERR_NOT_SUPPORTED explicitly.
 */
extern esy_err_t esy_hb_watchdog_deinit(void);

/**
 * @brief Query a guaranteed minimum timeout in microseconds after init/feed.
 * @param[out] out_timeout_us Written only on success; must not be NULL.
 * @retval ESY_OK A strictly positive lower bound, including clock tolerance.
 * @retval ESY_ERR_INVALID_ARG NULL output.
 * @retval ESY_ERR_NOT_SUPPORTED No certified bound for the active clock/port.
 * @retval ESY_ERR_STATE Pico watchdog is not enabled (before init or after deinit).
 * STM32F4 and ESP32 return ESY_ERR_NOT_SUPPORTED for every non-NULL output,
 * including before initialization; no certified bound is available there.
 * Task context, serialized with lifecycle operations. A nominal requested
 * timeout is not a bound. Callers must fail closed on any error.
 */
extern esy_err_t esy_hb_watchdog_get_timeout_lower_bound_us(
    uint64_t *out_timeout_us);

/**
 * @brief Feed (kick) the watchdog.
 *
 * Reloads the watchdog counter to prevent a timeout reset. Call this
 * periodically within the configured timeout window.
 */
extern void esy_hb_watchdog_feed(void);

/**
 * @brief Enable the watchdog timer.
 *
 * Starts the watchdog using the active initialized timeout/debug policy. Pico
 * also retains its last successful configuration after deinit. Without init,
 * the implementation may fall back to default settings (e.g. 1000 ms timeout,
 * no pause-on-debug).
 *
 * @note If the platform requires configuration/unlock before enabling, call
 *       @ref esy_hb_watchdog_init first.
 */
extern void esy_hb_watchdog_enable(void);

/**
 * @brief Check whether the last reset was caused by the watchdog.
 *
 * @retval true  The previous reset was triggered by the watchdog
 * @retval false The previous reset was caused by something else
 *
 * @note This is typically derived from a reset-cause register latched at boot.
 */
extern bool esy_hb_watchdog_was_reset(void);

/**
 * @brief Clear the watchdog reset flag.
 *
 * Acknowledges the public reset status without starting, stopping, feeding or
 * reconfiguring the live watchdog. Pico keeps this acknowledgement in software
 * for the current boot because its native reset reason is read-only. It is
 * valid before init. Serialize status and lifecycle operations on one owner.
 */
extern void esy_hb_watchdog_clear_reset(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_WATCHDOG_H */
