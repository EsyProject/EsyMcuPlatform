/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_low_power.h"

#include <string.h>

#include "esy_hal_backend/esy_hb_low_power.h"

#define ESY_HAL_POWER_PHASE_EMPTY    0u
#define ESY_HAL_POWER_PHASE_BEGUN    1u
#define ESY_HAL_POWER_PHASE_ENTERED  2u
#define ESY_HAL_POWER_PHASE_COMPLETE 3u

static esy_hb_low_power_mode_t state_to_driver_mode(
    esy_power_state_t state) {
  return state == ESY_POWER_STATE_CPU_IDLE ? ESY_HB_LOW_POWER_MODE_SLEEP
                                           : ESY_HB_LOW_POWER_MODE_STOP;
}

void esy_hal_power_transaction_init(
    esy_hal_power_transaction_t *transaction) {
  if (transaction != NULL) memset(transaction, 0, sizeof(*transaction));
}

esy_err_t esy_hal_power_get_capabilities(
    esy_power_capabilities_t *out_capabilities) {
  esy_hb_low_power_capabilities_t driver_capabilities;
  esy_err_t ret;
  if (out_capabilities == NULL) return ESY_ERR_INVALID_ARG;
  ret = esy_hb_low_power_get_capabilities(&driver_capabilities);
  if (ret != ESY_OK) return ret;
  memset(out_capabilities, 0, sizeof(*out_capabilities));
  out_capabilities->supported_state_mask =
      ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE);
  for (uint32_t state = (uint32_t)ESY_POWER_STATE_CPU_IDLE;
       state < (uint32_t)ESY_POWER_STATE_COUNT; state++) {
    esy_hb_low_power_mode_t mode =
        state_to_driver_mode((esy_power_state_t)state);
    if ((driver_capabilities.supported_mode_mask &
         ESY_HB_LOW_POWER_MODE_MASK(mode)) == 0u) {
      continue;
    }
    out_capabilities->supported_state_mask |= ESY_POWER_STATE_MASK(state);
    out_capabilities->states[state].entry_latency_us =
        driver_capabilities.entry_latency_us[mode];
    out_capabilities->states[state].exit_latency_us =
        driver_capabilities.exit_latency_us[mode];
    out_capabilities->states[state].min_residency_us =
        driver_capabilities.min_residency_us[mode];
  }
  return ESY_OK;
}

esy_err_t esy_hal_power_transaction_begin(
    esy_hal_power_transaction_t *transaction, esy_power_state_t state) {
  esy_power_capabilities_t capabilities;
  esy_err_t ret;
  if ((transaction == NULL) || (state <= ESY_POWER_STATE_ACTIVE) ||
      (state >= ESY_POWER_STATE_COUNT)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((transaction->phase != ESY_HAL_POWER_PHASE_EMPTY) &&
      (transaction->phase != ESY_HAL_POWER_PHASE_COMPLETE)) {
    return ESY_ERR_STATE;
  }
  ret = esy_hal_power_get_capabilities(&capabilities);
  if (ret != ESY_OK) return ret;
  if ((capabilities.supported_state_mask & ESY_POWER_STATE_MASK(state)) == 0u) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  esy_hal_power_transaction_init(transaction);
  transaction->result.state = state;
  transaction->result.entry_status = ESY_ERR_STATE;
  ret = esy_hb_low_power_prepare(state_to_driver_mode(state));
  if (ret != ESY_OK) return ret;
  transaction->driver_prepared = true;
  transaction->phase = ESY_HAL_POWER_PHASE_BEGUN;
  return ESY_OK;
}

esy_err_t esy_hal_power_transaction_enter_atomic(
    esy_hal_power_transaction_t *transaction) {
  esy_hb_low_power_result_t driver_result = {0};
  esy_err_t ret;
  if (transaction == NULL) return ESY_ERR_INVALID_ARG;
  if (transaction->phase != ESY_HAL_POWER_PHASE_BEGUN) {
    return ESY_ERR_STATE;
  }
  ret = esy_hb_low_power_enter(
      state_to_driver_mode(transaction->result.state), &driver_result);
  transaction->result.entry_status = ret;
  transaction->result.entered = driver_result.entered;
  transaction->result.fatal_error = driver_result.fatal_error;
  if (driver_result.entered || (driver_result.fatal_error != ESY_OK)) {
    transaction->driver_prepared = false;
  } else if (transaction->driver_prepared) {
    esy_err_t cancel_ret = esy_hb_low_power_cancel(
        state_to_driver_mode(transaction->result.state));
    transaction->driver_prepared = false;
    if (cancel_ret != ESY_OK) {
      transaction->result.fatal_error = cancel_ret;
    }
  }
  transaction->phase = ESY_HAL_POWER_PHASE_ENTERED;
  return transaction->result.fatal_error == ESY_OK
             ? ret
             : transaction->result.fatal_error;
}

esy_err_t esy_hal_power_transaction_complete(
    esy_hal_power_transaction_t *transaction,
    esy_hal_power_result_t *out_result) {
  if ((transaction == NULL) || (out_result == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (transaction->phase != ESY_HAL_POWER_PHASE_ENTERED) {
    return ESY_ERR_STATE;
  }
  if (transaction->result.fatal_error != ESY_OK) return ESY_ERR_STATE;
  *out_result = transaction->result;
  transaction->phase = ESY_HAL_POWER_PHASE_COMPLETE;
  return ESY_OK;
}

esy_err_t esy_hal_power_transaction_abort(
    esy_hal_power_transaction_t *transaction) {
  if (transaction == NULL) return ESY_ERR_INVALID_ARG;
  if (transaction->phase != ESY_HAL_POWER_PHASE_BEGUN) {
    return ESY_ERR_STATE;
  }
  if (transaction->driver_prepared) {
    esy_err_t ret = esy_hb_low_power_cancel(
        state_to_driver_mode(transaction->result.state));
    if (ret != ESY_OK) return ret;
  }
  transaction->driver_prepared = false;
  transaction->phase = ESY_HAL_POWER_PHASE_COMPLETE;
  return ESY_OK;
}

esy_err_t esy_hal_shutdown(esy_hal_shutdown_mode_t mode) {
  if (mode != ESY_HAL_SHUTDOWN_MODE_STANDBY) {
    return ESY_ERR_INVALID_ARG;
  }
  return esy_hb_shutdown(ESY_HB_SHUTDOWN_MODE_STANDBY);
}
