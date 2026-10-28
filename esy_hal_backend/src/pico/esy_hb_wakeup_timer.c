/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_wakeup_timer.h"

#include <string.h>

#include "hardware/irq.h"
#include "hardware/timer.h"
#include "pico/time.h"

static void wakeup_alarm_callback(uint alarm_index) { (void)alarm_index; }

static bool timer_is_valid(const esy_hb_wakeup_timer_t *timer) {
  return (timer != NULL) && timer->initialized &&
         (timer->alarm_index < NUM_ALARMS);
}

esy_err_t esy_hb_wakeup_timer_init(
    esy_hb_wakeup_timer_t *timer,
    const esy_hb_wakeup_timer_config_t *config) {
  if ((timer == NULL) || (config == NULL) ||
      (config->alarm_index >= NUM_ALARMS)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (timer->initialized) return ESY_ERR_STATE;
  if (hardware_alarm_is_claimed(config->alarm_index)) return ESY_ERR_BUSY;
  memset(timer, 0, sizeof(*timer));
  hardware_alarm_claim(config->alarm_index);
  hardware_alarm_set_callback(config->alarm_index, wakeup_alarm_callback);
  timer->alarm_index = config->alarm_index;
  timer->initialized = true;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_deinit(esy_hb_wakeup_timer_t *timer) {
  if (!timer_is_valid(timer)) return ESY_ERR_INVALID_ARG;
  if (timer->prepared || timer->armed) return ESY_ERR_BUSY;
  hardware_alarm_set_callback(timer->alarm_index, NULL);
  hardware_alarm_unclaim(timer->alarm_index);
  memset(timer, 0, sizeof(*timer));
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_get_supported_state_mask(
    esy_hb_wakeup_timer_t *timer, uint32_t *out_state_mask) {
  if (!timer_is_valid(timer) || (out_state_mask == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                    ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE);
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_now_atomic(esy_hb_wakeup_timer_t *timer,
                                             uint64_t *out_time_us) {
  if (!timer_is_valid(timer) || (out_time_us == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_time_us = time_us_64();
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_prepare(esy_hb_wakeup_timer_t *timer,
                                          esy_power_state_t state) {
  if (!timer_is_valid(timer)) return ESY_ERR_INVALID_ARG;
  if (state != ESY_POWER_STATE_CPU_IDLE) return ESY_ERR_NOT_SUPPORTED;
  if (timer->prepared || timer->armed) return ESY_ERR_STATE;
  timer->prepared = true;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_arm_absolute_atomic(
    esy_hb_wakeup_timer_t *timer, esy_power_state_t state,
    uint64_t wake_time_us) {
  if (!timer_is_valid(timer)) return ESY_ERR_INVALID_ARG;
  if (state != ESY_POWER_STATE_CPU_IDLE) return ESY_ERR_NOT_SUPPORTED;
  if (!timer->prepared || timer->armed || (wake_time_us <= time_us_64())) {
    return ESY_ERR_STATE;
  }
  if (hardware_alarm_set_target(timer->alarm_index,
                                from_us_since_boot(wake_time_us))) {
    return ESY_ERR_BUSY;
  }
  timer->armed_time_us = wake_time_us;
  timer->armed = true;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_capture_and_cancel_atomic(
    esy_hb_wakeup_timer_t *timer, bool *out_expired) {
  uint irq_number;
  if (!timer_is_valid(timer) || (out_expired == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!timer->prepared || !timer->armed) return ESY_ERR_STATE;
  irq_number = hardware_alarm_get_irq_num(timer->alarm_index);
  *out_expired = time_us_64() >= timer->armed_time_us;
  irq_set_enabled(irq_number, false);
  hardware_alarm_cancel(timer->alarm_index);
  timer->armed = false;
  timer->irq_masked = true;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_finish(esy_hb_wakeup_timer_t *timer) {
  if (!timer_is_valid(timer)) return ESY_ERR_INVALID_ARG;
  if (!timer->prepared || !timer->irq_masked || timer->armed) {
    return ESY_ERR_STATE;
  }
  irq_set_enabled(hardware_alarm_get_irq_num(timer->alarm_index), true);
  timer->irq_masked = false;
  timer->prepared = false;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_abort(esy_hb_wakeup_timer_t *timer) {
  if (!timer_is_valid(timer)) return ESY_ERR_INVALID_ARG;
  if (!timer->prepared || timer->armed || timer->irq_masked) {
    return ESY_ERR_STATE;
  }
  timer->prepared = false;
  return ESY_OK;
}
