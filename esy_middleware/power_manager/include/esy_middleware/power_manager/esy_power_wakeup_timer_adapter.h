/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_POWER_WAKEUP_TIMER_ADAPTER_H
#define ESY_POWER_WAKEUP_TIMER_ADAPTER_H

#include "esy_middleware/power_manager/esy_power_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return immutable deadline operations backed by a HAL timer.
 * Pass an initialized esy_hal_wakeup_timer_t as the operations context.
 * The caller owns the timer and selected hardware alarm; retain both until
 * manager deinit. Only the serialized power transaction may operate the timer
 * while borrowed. Errors are forwarded from the HAL timer unchanged.
 */
const esy_power_deadline_timer_ops_t *esy_power_wakeup_deadline_timer_ops(void);

/** @brief Return immutable capabilities for the same borrowed HAL timer.
 * Use the same timer context for deadline, capability and monotonic operations;
 * all timestamps must belong to one clock domain.
 */
const esy_power_clocksource_ops_t *esy_power_wakeup_clocksource_ops(void);

/** @brief Return immutable monotonic operations for the same HAL timer.
 * The timer must remain readable through entry and early recovery. The manager
 * invokes reads with interrupts masked; HAL read errors are propagated.
 */
const esy_monotonic_clock_ops_t *esy_power_wakeup_monotonic_clock_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_POWER_WAKEUP_TIMER_ADAPTER_H */
