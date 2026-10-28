/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_WAKEUP_TIMER_H
#define ESY_HB_WAKEUP_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_power_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Caller-owned hardware wakeup alarm. */
typedef struct esy_hb_wakeup_timer_s {
  uint64_t armed_time_us;
  uint8_t alarm_index;
  bool initialized;
  bool prepared;
  bool armed;
  bool irq_masked;
} esy_hb_wakeup_timer_t;

typedef struct esy_hb_wakeup_timer_config_s {
  uint8_t alarm_index;
} esy_hb_wakeup_timer_config_t;

/** @brief Claim an alarm. Storage must start at {0}; repeated init is rejected. */
esy_err_t esy_hb_wakeup_timer_init(
    esy_hb_wakeup_timer_t *timer,
    const esy_hb_wakeup_timer_config_t *config);
esy_err_t esy_hb_wakeup_timer_deinit(esy_hb_wakeup_timer_t *timer);
esy_err_t esy_hb_wakeup_timer_get_supported_state_mask(
    esy_hb_wakeup_timer_t *timer, uint32_t *out_state_mask);
esy_err_t esy_hb_wakeup_timer_now_atomic(esy_hb_wakeup_timer_t *timer,
                                             uint64_t *out_time_us);
esy_err_t esy_hb_wakeup_timer_prepare(esy_hb_wakeup_timer_t *timer,
                                          esy_power_state_t state);
esy_err_t esy_hb_wakeup_timer_arm_absolute_atomic(
    esy_hb_wakeup_timer_t *timer, esy_power_state_t state,
    uint64_t wake_time_us);
esy_err_t esy_hb_wakeup_timer_capture_and_cancel_atomic(
    esy_hb_wakeup_timer_t *timer, bool *out_expired);
esy_err_t esy_hb_wakeup_timer_finish(esy_hb_wakeup_timer_t *timer);
esy_err_t esy_hb_wakeup_timer_abort(esy_hb_wakeup_timer_t *timer);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_WAKEUP_TIMER_H */
