/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_wakeup_timer.h"

#include <string.h>

esy_err_t esy_hb_wakeup_timer_init(
    esy_hb_wakeup_timer_t *timer,
    const esy_hb_wakeup_timer_config_t *config) {
  if ((timer == NULL) || (config == NULL)) return ESY_ERR_INVALID_ARG;
  memset(timer, 0, sizeof(*timer));
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_wakeup_timer_deinit(esy_hb_wakeup_timer_t *timer) {
  return timer == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_wakeup_timer_get_supported_state_mask(
    esy_hb_wakeup_timer_t *timer, uint32_t *out_state_mask) {
  if ((timer == NULL) || (out_state_mask == NULL)) return ESY_ERR_INVALID_ARG;
  *out_state_mask = 0u;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_wakeup_timer_now_atomic(esy_hb_wakeup_timer_t *timer,
                                             uint64_t *out_time_us) {
  return (timer == NULL) || (out_time_us == NULL) ? ESY_ERR_INVALID_ARG
                                                  : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_wakeup_timer_prepare(esy_hb_wakeup_timer_t *timer,
                                          esy_power_state_t state) {
  (void)state;
  return timer == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_wakeup_timer_arm_absolute_atomic(
    esy_hb_wakeup_timer_t *timer, esy_power_state_t state,
    uint64_t wake_time_us) {
  (void)state;
  (void)wake_time_us;
  return timer == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_wakeup_timer_capture_and_cancel_atomic(
    esy_hb_wakeup_timer_t *timer, bool *out_expired) {
  return (timer == NULL) || (out_expired == NULL) ? ESY_ERR_INVALID_ARG
                                                  : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_wakeup_timer_finish(esy_hb_wakeup_timer_t *timer) {
  return timer == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_wakeup_timer_abort(esy_hb_wakeup_timer_t *timer) {
  return timer == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}
