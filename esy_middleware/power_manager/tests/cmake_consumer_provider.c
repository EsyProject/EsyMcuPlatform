/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Link-only native boundary for the public C/C++ consumer regression. */
#include "esy_hal/esy_hal_low_power.h"
#include "esy_hal/esy_hal_wakeup_timer.h"

#include <string.h>

void esy_hal_power_transaction_init(esy_hal_power_transaction_t *transaction) {
  memset(transaction, 0, sizeof(*transaction));
}

esy_err_t esy_hal_power_get_capabilities(esy_power_capabilities_t *out) {
  (void)out;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_power_transaction_begin(esy_hal_power_transaction_t *transaction,
                                          esy_power_state_t state) {
  (void)transaction;
  (void)state;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_power_transaction_enter_atomic(
    esy_hal_power_transaction_t *transaction) {
  (void)transaction;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_power_transaction_complete(
    esy_hal_power_transaction_t *transaction, esy_hal_power_result_t *out) {
  (void)transaction;
  (void)out;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_power_transaction_abort(esy_hal_power_transaction_t *transaction) {
  (void)transaction;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_wakeup_timer_get_supported_state_mask(
    esy_hal_wakeup_timer_t *timer, uint32_t *out) {
  (void)timer;
  (void)out;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_wakeup_timer_prepare(esy_hal_wakeup_timer_t *timer,
                                       esy_power_state_t state) {
  (void)timer;
  (void)state;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_wakeup_timer_arm_absolute_atomic(
    esy_hal_wakeup_timer_t *timer, esy_power_state_t state, uint64_t deadline) {
  (void)timer;
  (void)state;
  (void)deadline;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_wakeup_timer_capture_and_cancel_atomic(
    esy_hal_wakeup_timer_t *timer, bool *out) {
  (void)timer;
  (void)out;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_wakeup_timer_finish(esy_hal_wakeup_timer_t *timer) {
  (void)timer;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_wakeup_timer_abort(esy_hal_wakeup_timer_t *timer) {
  (void)timer;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_wakeup_timer_now_atomic(esy_hal_wakeup_timer_t *timer,
                                          uint64_t *out) {
  (void)timer;
  (void)out;
  return ESY_ERR_NOT_SUPPORTED;
}
