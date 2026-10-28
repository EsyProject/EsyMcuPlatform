/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_watchdog.h
 * @brief HAL watchdog timer interface
 *
 * Provides a unified hardware abstraction for watchdog initialization,
 * feeding (refresh), enable/disable control, and reset status management.
 *
 * This interface wraps the underlying HAL backend watchdog implementation
 * and exposes a consistent API to upper layers.
 */

#ifndef ESY_HAL_WATCHDOG_H
#define ESY_HAL_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the hardware watchdog
 *
 * Configures the watchdog timeout and requests whether the watchdog counter
 * should pause while the system is halted by a debugger. Pico honors this
 * request. STM32F4 honors it when the device exposes the IWDG debug-freeze
 * control. ESP32 ignores it; its watchdog debug behavior is platform-defined.
 *
 * Successful initialization starts the watchdog immediately on every port.
 * A failed hardware initialization may require system recovery; do not assume
 * an irreversible peripheral can be disabled after a hardware failure.
 *
 * @param[in] timeout_ms     Watchdog timeout in milliseconds
 *                           (maximum value is hardware-dependent).
 * @param[in] pause_on_debug Requested debug-halt policy: true to pause counting,
 *                           false to continue, where supported as described above.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_watchdog_init(uint32_t timeout_ms,
                                            bool pause_on_debug);

/**
 * @brief Deinitialize the hardware watchdog
 *
 * Task-context, single-owner operation. ESY_OK means the counter is stopped.
 * ESY_ERR_NOT_SUPPORTED means the watchdog cannot stop: it remains owned and
 * must continue to be fed or the system must reset. Other errors likewise
 * retain ownership. Repeated successful cleanup is harmless.
 */
extern esy_err_t esy_hal_watchdog_deinit(void);

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
extern esy_err_t esy_hal_watchdog_get_timeout_lower_bound_us(
    uint64_t *out_timeout_us);

/**
 * @brief Feed (refresh) the watchdog
 *
 * Resets the watchdog counter to prevent a system reset.
 * This function must be called periodically within the configured
 * timeout interval.
 *
 * It is recommended to call this function from the main loop
 * or from critical system tasks.
 */
extern void esy_hal_watchdog_feed(void);

/**
 * @brief Enable the hardware watchdog
 *
 * Starts the watchdog timer.
 * Uses the active initialized timeout/debug policy. Pico also retains the last
 * successful configuration after deinit. Lifecycle/status operations use one
 * serialized owner.
 *
 * If no successful @ref esy_hal_watchdog_init has supplied a configuration,
 * a default configuration is applied:
 * - timeout: 1000 ms
 * - watchdog does not pause in debug mode
 */
extern void esy_hal_watchdog_enable(void);

/**
 * @brief Check whether the last system reset was caused by the watchdog
 *
 * @retval true  The last reset was triggered by the watchdog.
 * @retval false The last reset was caused by another source.
 */
extern bool esy_hal_watchdog_was_reset(void);

/**
 * @brief Clear the watchdog reset flag
 *
 * Clears the internal flag indicating a watchdog-triggered reset.
 * Does not start, stop, feed or reconfigure the live watchdog. On Pico the
 * acknowledgement is software-local for this boot because the hardware reset
 * reason is read-only. This operation may be used before watchdog init.
 */
extern void esy_hal_watchdog_clear_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_WATCHDOG_H */
