/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_POWER_WATCHDOG_CONSTRAINT_H
#define ESY_POWER_WATCHDOG_CONSTRAINT_H

#include "esy_middleware/power_manager/esy_power_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Caller-owned, externally serialized adapter. Initialize with {0}.
 * Fields are private to the implementation after init. The shared clock must
 * match both the manager and watchdog monitor and outlive registration. */
typedef struct esy_power_watchdog_constraint_s {
  esy_monotonic_clock_t clock;
  bool prepared;
  bool initialized;
} esy_power_watchdog_constraint_t;

/** @brief Initialize zero-initialized storage in task context.
 * @param adapter Exclusive caller-owned storage, initially {0} or deinitialized.
 * @param clock Borrowed clock with a non-NULL read_atomic operation.
 * @retval ESY_OK Initialized without allocation.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer or clock.
 * @retval ESY_ERR_STATE Already initialized; no state is changed.
 * All failures leave the object unchanged. Do not re-zero a live adapter. */
esy_err_t esy_power_watchdog_constraint_init(
    esy_power_watchdog_constraint_t *adapter,
    const esy_monotonic_clock_t *clock);
/** @brief Clear an unused adapter after its constraint is unregistered or its
 * manager is deinitialized. Call only after all callbacks stop, in task context.
 * @param adapter Initialized exclusive storage.
 * @retval ESY_OK Storage is zeroed and may be initialized again.
 * @retval ESY_ERR_INVALID_ARG NULL adapter.
 * @retval ESY_ERR_STATE Not initialized (including repeated deinit).
 * @retval ESY_ERR_BUSY Prepared constraint; unchanged, finish recovery first.
 * A manager recovery fault requires system recovery, not forced deinit. */
esy_err_t esy_power_watchdog_constraint_deinit(
    esy_power_watchdog_constraint_t *adapter);
/** @brief Return static immutable operations. Pass the initialized adapter as
 * ctx. Calls follow esy_power_constraint_ops_t contexts and must be serialized
 * against init/deinit. Deadline errors propagate from the watchdog monitor. */
const esy_power_constraint_ops_t *esy_power_watchdog_constraint_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_POWER_WATCHDOG_CONSTRAINT_H */
