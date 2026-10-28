/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_LOW_POWER_H
#define ESY_HAL_LOW_POWER_H

#include <stdbool.h>

#include "esy_common/defs/system/esy_power_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Result captured by one resumable HAL power transaction. */
typedef struct esy_hal_power_result_s {
  esy_power_state_t state;
  bool entered;
  esy_err_t entry_status;
  esy_err_t fatal_error;
} esy_hal_power_result_t;

/** @brief Caller-owned single-owner HAL power transaction. */
typedef struct esy_hal_power_transaction_s {
  esy_hal_power_result_t result;
  uint32_t phase;
  bool driver_prepared;
} esy_hal_power_transaction_t;

/** @brief Reset a transaction before its first use or after completion. */
void esy_hal_power_transaction_init(
    esy_hal_power_transaction_t *transaction);

/** @brief Return resumable state support and conservative timing limits. */
esy_err_t esy_hal_power_get_capabilities(
    esy_power_capabilities_t *out_capabilities);

/** @brief Prepare one resumable transaction with interrupts enabled. */
esy_err_t esy_hal_power_transaction_begin(
    esy_hal_power_transaction_t *transaction, esy_power_state_t state);

/**
 * @brief Enter hardware with interrupts masked and perform early restoration.
 *
 * A non-OK fatal_error means clocks could not be restored. The caller must
 * keep interrupts masked and enter its fail-stop/reset path immediately.
 */
esy_err_t esy_hal_power_transaction_enter_atomic(
    esy_hal_power_transaction_t *transaction);

/** @brief Complete a returning transaction with interrupts still masked. */
esy_err_t esy_hal_power_transaction_complete(
    esy_hal_power_transaction_t *transaction,
    esy_hal_power_result_t *out_result);

/** @brief Abort a prepared transaction before hardware entry. */
esy_err_t esy_hal_power_transaction_abort(
    esy_hal_power_transaction_t *transaction);

/** @brief Reset-class shutdown modes excluded from automatic idle policy. */
typedef enum esy_hal_shutdown_mode_e {
  ESY_HAL_SHUTDOWN_MODE_STANDBY = 0,
  ESY_HAL_SHUTDOWN_MODE_COUNT
} esy_hal_shutdown_mode_t;

/** @brief Enter an explicit reset-class state. Success does not return. */
esy_err_t esy_hal_shutdown(esy_hal_shutdown_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_LOW_POWER_H */
