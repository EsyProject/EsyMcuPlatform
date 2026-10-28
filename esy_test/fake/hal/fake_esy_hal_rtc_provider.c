/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_rtc_provider.h"

static esy_err_t fake_get_datetime(void *ctx, esy_datetime_t *out_datetime) {
  fake_esy_hal_rtc_provider_t *provider = ctx;
  ++provider->calls;
  /* Deliberately write on failure to exercise facade output isolation. */
  *out_datetime = provider->datetime;
  return provider->status;
}

static esy_err_t fake_set_datetime(void *ctx, const esy_datetime_t *datetime) {
  fake_esy_hal_rtc_provider_t *provider = ctx;
  ++provider->calls;
  if (provider->status == ESY_OK) provider->datetime = *datetime;
  return provider->status;
}

static esy_err_t fake_alarm_set(void *ctx, const esy_datetime_t *datetime) {
  fake_esy_hal_rtc_provider_t *provider = ctx;
  ++provider->calls;
  if (provider->status == ESY_OK) {
    provider->alarm = *datetime;
    provider->pending = false;
  }
  return provider->status;
}

static esy_err_t fake_alarm_cancel(void *ctx) {
  fake_esy_hal_rtc_provider_t *provider = ctx;
  ++provider->calls;
  if (provider->status == ESY_OK) provider->pending = false;
  return provider->status;
}

static esy_err_t fake_alarm_get_pending(void *ctx, bool clear, bool *out_pending) {
  fake_esy_hal_rtc_provider_t *provider = ctx;
  ++provider->calls;
  *out_pending = provider->pending;
  if ((provider->status == ESY_OK) && clear) provider->pending = false;
  return provider->status;
}

static esy_err_t fake_set_calibration(void *ctx, int32_t correction_ppb) {
  fake_esy_hal_rtc_provider_t *provider = ctx;
  ++provider->calls;
  if (provider->status == ESY_OK) provider->calibration_ppb = correction_ppb;
  return provider->status;
}

static esy_err_t fake_get_calibration(void *ctx, int32_t *out_correction_ppb) {
  fake_esy_hal_rtc_provider_t *provider = ctx;
  ++provider->calls;
  *out_correction_ppb = provider->calibration_ppb;
  return provider->status;
}

static const esy_hal_rtc_ops_t k_fake_ops = {
    .capabilities = ESY_RTC_CAP_ALARM | ESY_RTC_CAP_CALIBRATION,
    .get_datetime = fake_get_datetime,
    .set_datetime = fake_set_datetime,
    .alarm_set = fake_alarm_set,
    .alarm_cancel = fake_alarm_cancel,
    .alarm_get_pending = fake_alarm_get_pending,
    .set_calibration = fake_set_calibration,
    .get_calibration = fake_get_calibration,
};

void fake_esy_hal_rtc_provider_reset(fake_esy_hal_rtc_provider_t *provider) {
  if (provider == NULL) return;
  *provider = (fake_esy_hal_rtc_provider_t){
      .datetime = {.year = 1970u, .month = 1u, .day = 1u},
  };
}

const esy_hal_rtc_ops_t *fake_esy_hal_rtc_provider_get_ops(void) {
  return &k_fake_ops;
}

esy_err_t fake_esy_hal_rtc_clock_sample(void *ctx, uint64_t *out_time_us) {
  fake_esy_hal_rtc_clock_t *clock = ctx;
  if ((clock == NULL) || (out_time_us == NULL)) return ESY_ERR_INVALID_ARG;
  ++clock->calls;
  if (clock->status == ESY_OK) *out_time_us = clock->now_us;
  return clock->status;
}
