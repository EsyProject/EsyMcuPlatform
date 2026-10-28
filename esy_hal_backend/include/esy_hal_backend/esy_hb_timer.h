/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_timer.h
 * @brief HAL backend timer task interface (software / hardware timers).
 *
 * This module provides timer task management based on millisecond or microsecond
 * time bases, including task start, cancellation, status query, and interrupt
 * dispatch entry points.
 *
 * Conventions:
 * - Return type is either @ref esy_err_t (0 on success, negative on failure)
 *   or a timer_id (>=0 valid, <0 failure).
 * - The time base (ms/us) is selected inside @ref esy_timer_task_config_t.
 * - IRQ handler functions are called from low-level interrupt vectors to drive
 *   task scheduling and dispatch.
 */

#ifndef ESY_HB_TIMER_H
#define ESY_HB_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_timer_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the timer module.
 *
 * Resets internal timer task management data structures.
 * This function should be called once during system startup.
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_timer_init(void);

/**
 * @brief Deinitialize the timer module.
 *
 * The current implementation may be empty; this function is reserved for
 * platforms that need to release timer-related resources.
 */
extern void esy_hb_timer_deinit(void);

/**
 * @brief Start a timer task.
 *
 * Starts a one-shot or periodic timer task according to @p task_config.
 * Both millisecond and microsecond modes are supported, depending on the
 * configuration.
 *
 * @param[in] task_config Pointer to the timer task configuration structure
 *
 * @return
 *  - >=0  Success (allocated timer_id)
 *  -  <0  Failure (e.g. no free slot or internal error)
 *
 * @note The callback may be executed in interrupt context. Avoid blocking
 *       operations and long-running computations inside the callback.
 *       Calls are serialized on one owner core; callbacks may start/cancel
 *       timers, but not init/deinit. Callback arguments remain caller-owned.
 *       Start/cancel belong to the task/startup owner or timer callbacks;
 *       unrelated ISRs must not manage these lifecycles.
 *       STM32F4 owns TIM2/TIM5 with full 32-bit 1 MHz counters and CC1 deadline
 *       interrupts, with eight shared logical slots. Keep the clock fixed and
 *       running, service IRQs within 2^31 microseconds, and use positive intervals
 *       of at most INT32_MAX microseconds. Periodic late deliveries are coalesced
 *       while preserving phase. Each IRQ dispatches at most one callback per
 *       slot, outside local IRQ masking. Both timer IRQs share a priority while
 *       any slot is active or either dispatcher is running, preventing nested
 *       timer callbacks from reusing HAL callback state before delivery. A
 *       mismatch returns ESY_ERR_BUSY. Non-default clock sources are unsupported.
 *       Failed starts do not alter existing timers. IDs are reused after release.
 */
extern int32_t esy_hb_timer_start(
    const esy_timer_task_config_t *task_config);

/**
 * @brief Cancel a specific timer task.
 *
 * Invalid timer IDs are ignored.
 *
 * @param[in] timer_id Timer ID returned by @ref esy_hb_timer_start
 */
extern void esy_hb_timer_cancel(int32_t timer_id);

/**
 * @brief Cancel all active timer tasks.
 */
extern void esy_hb_timer_cancel_all(void);

/**
 * @brief Millisecond time-base timer IRQ handler entry.
 *
 * Called from the low-level millisecond timer interrupt to dispatch
 * millisecond-based timer tasks.
 */
extern void esy_hb_timer_handle_ms_irq(void);

/**
 * @brief Microsecond time-base timer IRQ handler entry.
 *
 * Called from the low-level microsecond timer interrupt to dispatch
 * microsecond-based timer tasks.
 */
extern void esy_hb_timer_handle_us_irq(void);

/**
 * @brief Get the maximum number of timer tasks supported.
 *
 * Can be used by the HAL wrapper to validate timer_id ranges or
 * perform capacity planning.
 *
 * @return Maximum number of timer task slots
 */
extern uint8_t esy_hb_timer_get_max_timer_task(void);

/**
 * @brief Check whether a timer task is currently active.
 *
 * Invalid timer IDs return false.
 *
 * @param[in] timer_id Timer ID
 * @retval true  The timer task is active
 * @retval false The timer task is inactive or invalid
 */
extern bool esy_hb_timer_is_active(int32_t timer_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_TIMER_H */
