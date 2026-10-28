/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_timer.h
 * @brief HAL timer task interface.
 *
 * Provides a task-oriented timer abstraction based on millisecond and
 * microsecond time bases.
 *
 * Design notes:
 * - This module manages logical timer tasks, not raw hardware timers.
 * - The HAL dispatches timer events and delegates concrete
 *   hardware operations to the HAL backend.
 * - Both one-shot and periodic timers are supported.
 */

#ifndef ESY_HAL_TIMER_H
#define ESY_HAL_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_timer_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the HAL timer module.
 *
 * Allocates and initializes hardware and/or software resources required
 * by the HAL timer subsystem. Task/startup context only, with all callers
 * stopped. Repeated initialization returns ESY_ERR_STATE; failure retains no
 * timer callbacks and permits retry. STM32F4 reserves the complete 32-bit TIM2
 * and TIM5 peripherals, CC1 channels and IRQs; do not share them with capture,
 * PWM or another owner. Its APB timer clock must divide exactly to 1 MHz.
 * ESP32 returns ESY_ERR_BUSY if an earlier native cleanup still owns a slot;
 * retry cancellation/deinitialization before initializing again.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_timer_init(void);

/**
 * @brief Deinitialize the HAL timer module.
 *
 * Releases all hardware and/or software resources associated with
 * HAL timer tasks. Task/startup context only, after all callers stop; never
 * deinitialize from a callback. Repeated deinitialization is harmless. Callback
 * arguments must remain valid until their one-shot delivery completes, or
 * cancellation/deinitialization has retired the corresponding timer.
 * ESP32 suppresses delivery before native stop/delete. A cleanup failure keeps
 * the native handle and its slot reserved for another cancel/deinit attempt;
 * init returns ESY_ERR_BUSY until that cleanup succeeds. No user callback is
 * retained after failed deinitialization; an already executing callback still
 * has to finish before its argument can be released.
 */
extern void esy_hal_timer_deinit(void);

/**
 * @brief Fill a timer task configuration structure with default values.
 *
 * Writes platform-defined default settings into @p config.
 * The caller may override individual fields before starting the task.
 *
 * @param[out] config Configuration structure to initialize; must not be NULL.
 */
extern void esy_hal_timer_default_task_cfg(
    esy_timer_task_config_t *config);

/**
 * @brief Start a timer task.
 *
 * Configures and starts a timer task according to @p task_config.
 * Both one-shot and periodic timers are supported. All calls and timer IRQs
 * must run on one owner core; serialize calls from different tasks. A callback
 * may start/cancel timers, including when delivered before start returns.
 * Start/cancel operations belong to the serialized task/startup owner or its
 * timer callbacks; unrelated ISRs must not manage these lifecycles.
 * Do not call module init/deinit from a callback. Slot metadata is protected
 * against the same-core timer ISR; this is not a cross-core lock.
 * Intervals must be positive and represent at most INT32_MAX microseconds;
 * millisecond intervals therefore stop at 2147483. The counter/clock source
 * must remain running, and interrupt service must not be delayed by 2^31
 * microseconds. Wrapped 32-bit deadlines are compared within that half range.
 * STM32F4 callbacks execute in its timer ISR, at most once per slot per IRQ;
 * overdue periodic deliveries are coalesced, preserving the period phase.
 * Callback work must be bounded; execution latency is additional to counter
 * quantization. Both STM32F4 timer IRQs share one priority while any logical
 * timer is active or a callback is dispatching, including across MS/US modes.
 * A conflicting priority returns ESY_ERR_BUSY without altering active timers.
 * A failed start, including exhausted capacity, leaves existing timers intact.
 * ESP32 callbacks execute in the SDK timer task. Serialize timer management
 * with that task on the owner core, including callback dispatch. One-shot native
 * retirement occurs before user delivery; periodic callbacks may cancel their
 * own timer and start a replacement without the old dispatcher changing it.
 * The time base (milliseconds or microseconds) is selected via fields
 * inside the configuration structure.
 *
 * @param[in] task_config Timer task configuration.
 *
 * @return
 *  - >=0 Timer identifier on success.
 *  - <0  Error code on failure.
 */
extern int32_t esy_hal_timer_start(
    esy_timer_task_config_t *task_config);

/**
 * @brief Cancel a timer task.
 *
 * Stops and removes the timer task associated with the given identifier.
 * On the owner core this may be called from a timer callback. It does not
 * terminate a callback already executing; retire that callback before releasing
 * its argument. Numeric identifiers may be reused after cancellation/delivery.
 * ESP32 native cleanup failure suppresses further user delivery but retains
 * the handle/slot: is_active remains true and another cancel/deinit retries.
 *
 * @param[in] timer_id Timer identifier returned by @ref esy_hal_timer_start.
 */
extern void esy_hal_timer_cancel(int32_t timer_id);

/**
 * @brief Check whether a timer task is active.
 *
 * @param[in] timer_id Timer identifier.
 *
 * @retval true  The timer task is active, or ESP32 native cleanup is pending.
 * @retval false The timer is invalid or not active.
 */
extern bool esy_hal_timer_is_active(int32_t timer_id);

/**
 * @brief Return the interval until the earliest active logical timer deadline.
 *
 * @param[out] out_interval_us Relative interval in microseconds.
 * @retval ESY_OK At least one active timer was found.
 * @retval ESY_ERR_STATE No logical timer is active.
 */
extern esy_err_t esy_hal_timer_get_next_wake_interval_us(
    uint32_t *out_interval_us);

/**
 * @brief Millisecond time-base interrupt handler.
 *
 * Invoked from the platform millisecond timer ISR.
 * Dispatches and advances millisecond-based timer tasks.
 */
extern void esy_hal_timer_handle_ms_irq(void);

/**
 * @brief Microsecond time-base interrupt handler.
 *
 * Invoked from the platform microsecond timer ISR.
 * Dispatches and advances microsecond-based timer tasks.
 */
extern void esy_hal_timer_handle_us_irq(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_TIMER_H */
