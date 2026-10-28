/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_wakeup_timer.h"

#include "esy_hal_backend/esy_hb_wakeup_timer.h"

_Static_assert(sizeof(esy_hb_wakeup_timer_t) <=
                   sizeof(esy_hal_wakeup_timer_t),
               "platform wakeup timer storage is too small");

static esy_hb_wakeup_timer_t *driver_timer(
    esy_hal_wakeup_timer_t *timer) {
  return (esy_hb_wakeup_timer_t *)(void *)timer;
}

esy_err_t esy_hal_wakeup_timer_init(
    esy_hal_wakeup_timer_t *timer,
    const esy_hal_wakeup_timer_config_t *config) {
  esy_hb_wakeup_timer_config_t driver_config;
  if ((timer == NULL) || (config == NULL)) return ESY_ERR_INVALID_ARG;
  if (driver_timer(timer)->initialized) return ESY_ERR_STATE;
  driver_config.alarm_index = config->alarm_index;
  return esy_hb_wakeup_timer_init(driver_timer(timer), &driver_config);
}

esy_err_t esy_hal_wakeup_timer_deinit(esy_hal_wakeup_timer_t *timer) {
  return timer == NULL ? ESY_ERR_INVALID_ARG
                       : esy_hb_wakeup_timer_deinit(driver_timer(timer));
}

esy_err_t esy_hal_wakeup_timer_get_supported_state_mask(
    esy_hal_wakeup_timer_t *timer, uint32_t *out_state_mask) {
  return timer == NULL ? ESY_ERR_INVALID_ARG
                       : esy_hb_wakeup_timer_get_supported_state_mask(
                             driver_timer(timer), out_state_mask);
}

esy_err_t esy_hal_wakeup_timer_now_atomic(
    esy_hal_wakeup_timer_t *timer, uint64_t *out_time_us) {
  return timer == NULL ? ESY_ERR_INVALID_ARG
                       : esy_hb_wakeup_timer_now_atomic(driver_timer(timer),
                                                            out_time_us);
}

esy_err_t esy_hal_wakeup_timer_prepare(esy_hal_wakeup_timer_t *timer,
                                            esy_power_state_t state) {
  return timer == NULL
             ? ESY_ERR_INVALID_ARG
             : esy_hb_wakeup_timer_prepare(driver_timer(timer), state);
}

esy_err_t esy_hal_wakeup_timer_arm_absolute_atomic(
    esy_hal_wakeup_timer_t *timer, esy_power_state_t state,
    uint64_t wake_time_us) {
  return timer == NULL ? ESY_ERR_INVALID_ARG
                       : esy_hb_wakeup_timer_arm_absolute_atomic(
                             driver_timer(timer), state, wake_time_us);
}

esy_err_t esy_hal_wakeup_timer_capture_and_cancel_atomic(
    esy_hal_wakeup_timer_t *timer, bool *out_expired) {
  return timer == NULL ? ESY_ERR_INVALID_ARG
                       : esy_hb_wakeup_timer_capture_and_cancel_atomic(
                             driver_timer(timer), out_expired);
}

esy_err_t esy_hal_wakeup_timer_finish(esy_hal_wakeup_timer_t *timer) {
  return timer == NULL ? ESY_ERR_INVALID_ARG
                       : esy_hb_wakeup_timer_finish(driver_timer(timer));
}

esy_err_t esy_hal_wakeup_timer_abort(esy_hal_wakeup_timer_t *timer) {
  return timer == NULL ? ESY_ERR_INVALID_ARG
                       : esy_hb_wakeup_timer_abort(driver_timer(timer));
}
