/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_POWER_PLATFORM_BACKEND_H
#define ESY_POWER_PLATFORM_BACKEND_H

#include "esy_middleware/power_manager/esy_power_manager.h"
#include "esy_hal/esy_hal_low_power.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Caller-owned context, initially {0}. One manager exclusively borrows
 * this backend until its deinit. Operations and lifecycle are serialized by
 * that manager's owner; fields are implementation-owned after init. */
typedef struct esy_power_hal_backend_s {
  esy_hal_power_transaction_t transaction;
  esy_hal_power_result_t result;
  bool initialized;
  bool active;
} esy_power_hal_backend_t;

/** @brief Initialize unused {0} storage in task context.
 * @param backend Exclusive caller-owned storage, kept alive until deinit.
 * @retval ESY_OK Initialized; no allocation or hardware claim.
 * @retval ESY_ERR_INVALID_ARG NULL backend.
 * @retval ESY_ERR_STATE Already initialized. Failures leave storage unchanged.
 */
esy_err_t esy_power_hal_backend_init(esy_power_hal_backend_t *backend);

/** @brief Deinitialize after the borrowing manager is deinitialized.
 * @param backend Initialized storage; all callbacks must have stopped.
 * @retval ESY_OK Cleared; may be initialized again.
 * @retval ESY_ERR_INVALID_ARG NULL backend.
 * @retval ESY_ERR_STATE Not initialized, including repeated deinit.
 * @retval ESY_ERR_BUSY Active or failed transaction; storage is retained.
 * Call in task context. Recovery failure requires system recovery. */
esy_err_t esy_power_hal_backend_deinit(esy_power_hal_backend_t *backend);

/** @brief Return immutable operations for manager backend injection. */
const esy_power_backend_ops_t *esy_power_hal_backend_ops(void);

/** @brief Return the standard HAL always-on clocksource operations. */
const esy_power_clocksource_ops_t *esy_power_platform_clocksource_ops(void);

/** @brief Return reads for the standard HAL always-on clock. */
const esy_monotonic_clock_ops_t *esy_power_platform_monotonic_clock_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_POWER_PLATFORM_BACKEND_H */
