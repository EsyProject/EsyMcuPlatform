/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/power_manager/esy_power_wakeup_timer_adapter.h"

#include "esy_hal/esy_hal_wakeup_timer.h"

static esy_err_t get_state_mask(void *ctx, uint32_t *out_state_mask) {
  return esy_hal_wakeup_timer_get_supported_state_mask(ctx,
                                                            out_state_mask);
}

static esy_err_t prepare(void *ctx, esy_power_state_t state) {
  return esy_hal_wakeup_timer_prepare(ctx, state);
}

static esy_err_t arm_absolute_atomic(void *ctx, esy_power_state_t state,
                                     uint64_t wake_time_us) {
  return esy_hal_wakeup_timer_arm_absolute_atomic(ctx, state,
                                                       wake_time_us);
}

static esy_err_t capture_and_cancel_atomic(void *ctx, bool *out_expired) {
  return esy_hal_wakeup_timer_capture_and_cancel_atomic(ctx, out_expired);
}

static esy_err_t finish(void *ctx) {
  return esy_hal_wakeup_timer_finish(ctx);
}

static esy_err_t abort_timer(void *ctx) {
  return esy_hal_wakeup_timer_abort(ctx);
}

static esy_err_t read_atomic(void *ctx, uint64_t *out_time_us) {
  return esy_hal_wakeup_timer_now_atomic(ctx, out_time_us);
}

static const esy_power_deadline_timer_ops_t k_deadline_ops = {
    .get_supported_state_mask = get_state_mask,
    .prepare = prepare,
    .arm_absolute_atomic = arm_absolute_atomic,
    .capture_and_cancel_atomic = capture_and_cancel_atomic,
    .finish = finish,
    .abort = abort_timer,
};

static const esy_power_clocksource_ops_t k_clocksource_ops = {
    .get_supported_state_mask = get_state_mask,
};

static const esy_monotonic_clock_ops_t k_clock_ops = {
    .read_atomic = read_atomic,
};

const esy_power_deadline_timer_ops_t *esy_power_wakeup_deadline_timer_ops(
    void) {
  return &k_deadline_ops;
}

const esy_power_clocksource_ops_t *esy_power_wakeup_clocksource_ops(void) {
  return &k_clocksource_ops;
}

const esy_monotonic_clock_ops_t *esy_power_wakeup_monotonic_clock_ops(void) {
  return &k_clock_ops;
}
