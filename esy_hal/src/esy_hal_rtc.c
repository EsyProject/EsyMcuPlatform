/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_rtc.h"

#include <string.h>

#include "esy_common/utils/time/esy_datetime.h"

#define RTC_MICROSECONDS_PER_SECOND UINT64_C(1000000)
#define RTC_KNOWN_CAPS (ESY_RTC_CAP_ALARM | ESY_RTC_CAP_CALIBRATION)

/* Caller-owned implementation; no singleton state or native resources. */
typedef struct {
  esy_hal_rtc_monotonic_us_fn monotonic_us;
  void *clock_ctx;
  esy_timestamp_t now;
  int64_t alarm_seconds;
  uint64_t last_us;
  uint32_t fractional_remainder;
  int32_t calibration_ppb;
  bool initialized;
  bool alarm_armed;
  bool alarm_pending;
} rtc_software_ctx_t;

_Static_assert(sizeof(rtc_software_ctx_t) <=
                   ESY_HAL_RTC_SOFTWARE_STORAGE_BYTES,
               "software RTC storage is too small");
_Static_assert(_Alignof(rtc_software_ctx_t) <= _Alignof(esy_hal_rtc_software_t),
               "software RTC storage alignment is too small");

static esy_err_t check_rtc(const esy_hal_rtc_t *rtc) {
  if (rtc == NULL) return ESY_ERR_INVALID_ARG;
  return rtc->ops == NULL ? ESY_ERR_STATE : ESY_OK;
}

esy_err_t esy_hal_rtc_init(esy_hal_rtc_t *rtc,
                          const esy_hal_rtc_config_t *config) {
  const esy_hal_rtc_ops_t *ops;
  if ((rtc == NULL) || (config == NULL)) return ESY_ERR_INVALID_ARG;
  if (rtc->ops != NULL) return ESY_ERR_STATE;
  ops = config->ops;
  if ((ops == NULL) || (ops->get_datetime == NULL) ||
      (ops->set_datetime == NULL)) return ESY_ERR_INVALID_ARG;
  if (((ops->capabilities & ESY_RTC_CAP_ALARM) != 0u) &&
      ((ops->alarm_set == NULL) || (ops->alarm_cancel == NULL) ||
       (ops->alarm_get_pending == NULL))) return ESY_ERR_INVALID_ARG;
  if (((ops->capabilities & ESY_RTC_CAP_CALIBRATION) != 0u) &&
      ((ops->set_calibration == NULL) || (ops->get_calibration == NULL)))
    return ESY_ERR_INVALID_ARG;
  rtc->provider_ctx = config->provider_ctx;
  rtc->ops = ops;
  return ESY_OK;
}

esy_err_t esy_hal_rtc_deinit(esy_hal_rtc_t *rtc) {
  esy_err_t status = check_rtc(rtc);
  if (status != ESY_OK) return status;
  *rtc = (esy_hal_rtc_t){0};
  return ESY_OK;
}

esy_err_t esy_hal_rtc_get_caps(esy_hal_rtc_t *rtc, uint32_t *out_caps) {
  esy_err_t status = check_rtc(rtc);
  if (out_caps == NULL) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  *out_caps = rtc->ops->capabilities & RTC_KNOWN_CAPS;
  return ESY_OK;
}

esy_err_t esy_hal_rtc_get_datetime(esy_hal_rtc_t *rtc,
                                  esy_datetime_t *out_datetime) {
  esy_datetime_t datetime = {0};
  esy_err_t status = check_rtc(rtc);
  if (out_datetime == NULL) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  status = rtc->ops->get_datetime(rtc->provider_ctx, &datetime);
  if (status != ESY_OK) return status;
  if (esy_datetime_validate(&datetime) != ESY_OK) return ESY_ERR_IO;
  *out_datetime = datetime;
  return ESY_OK;
}

esy_err_t esy_hal_rtc_set_datetime(esy_hal_rtc_t *rtc,
                                  const esy_datetime_t *datetime) {
  esy_err_t status = check_rtc(rtc);
  if (esy_datetime_validate(datetime) != ESY_OK) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  return rtc->ops->set_datetime(rtc->provider_ctx, datetime);
}

esy_err_t esy_hal_rtc_alarm_set(esy_hal_rtc_t *rtc,
                               const esy_datetime_t *datetime) {
  esy_err_t status = check_rtc(rtc);
  if (esy_datetime_validate(datetime) != ESY_OK) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  if ((rtc->ops->capabilities & ESY_RTC_CAP_ALARM) == 0u)
    return ESY_ERR_NOT_SUPPORTED;
  return rtc->ops->alarm_set(rtc->provider_ctx, datetime);
}

esy_err_t esy_hal_rtc_alarm_cancel(esy_hal_rtc_t *rtc) {
  esy_err_t status = check_rtc(rtc);
  if (status != ESY_OK) return status;
  if ((rtc->ops->capabilities & ESY_RTC_CAP_ALARM) == 0u)
    return ESY_ERR_NOT_SUPPORTED;
  return rtc->ops->alarm_cancel(rtc->provider_ctx);
}

esy_err_t esy_hal_rtc_alarm_get_pending(esy_hal_rtc_t *rtc, bool clear,
                                       bool *out_pending) {
  bool pending = false;
  esy_err_t status = check_rtc(rtc);
  if (out_pending == NULL) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  if ((rtc->ops->capabilities & ESY_RTC_CAP_ALARM) == 0u)
    return ESY_ERR_NOT_SUPPORTED;
  status = rtc->ops->alarm_get_pending(rtc->provider_ctx, clear, &pending);
  if (status == ESY_OK) *out_pending = pending;
  return status;
}

esy_err_t esy_hal_rtc_set_calibration(esy_hal_rtc_t *rtc,
                                     int32_t correction_ppb) {
  esy_err_t status = check_rtc(rtc);
  if (status != ESY_OK) return status;
  if ((rtc->ops->capabilities & ESY_RTC_CAP_CALIBRATION) == 0u)
    return ESY_ERR_NOT_SUPPORTED;
  return rtc->ops->set_calibration(rtc->provider_ctx, correction_ppb);
}

esy_err_t esy_hal_rtc_get_calibration(esy_hal_rtc_t *rtc,
                                     int32_t *out_correction_ppb) {
  int32_t correction = 0;
  esy_err_t status = check_rtc(rtc);
  if (out_correction_ppb == NULL) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  if ((rtc->ops->capabilities & ESY_RTC_CAP_CALIBRATION) == 0u)
    return ESY_ERR_NOT_SUPPORTED;
  status = rtc->ops->get_calibration(rtc->provider_ctx, &correction);
  if (status == ESY_OK) *out_correction_ppb = correction;
  return status;
}

static esy_err_t software_load(const void *storage, rtc_software_ctx_t *ctx) {
  if (storage == NULL) return ESY_ERR_INVALID_ARG;
  memcpy(ctx, storage, sizeof(*ctx));
  return ctx->initialized ? ESY_OK : ESY_ERR_STATE;
}

static void software_store(void *storage, const rtc_software_ctx_t *ctx) {
  memcpy(storage, ctx, sizeof(*ctx));
}

static void software_evaluate_alarm(rtc_software_ctx_t *ctx) {
  if (ctx->alarm_armed && (ctx->now.seconds >= ctx->alarm_seconds)) {
    ctx->alarm_armed = false;
    ctx->alarm_pending = true;
  }
}

static esy_err_t software_advance(rtc_software_ctx_t *ctx) {
  uint64_t sample_us;
  uint64_t elapsed_us;
  uint64_t whole_seconds;
  uint64_t fraction;
  int64_t nanoseconds;
  int64_t seconds;
  int64_t remainder;
  esy_err_t status = ctx->monotonic_us(ctx->clock_ctx, &sample_us);
  if (status != ESY_OK) return status;
  /* Unsigned subtraction preserves a single wrap, subject to the clock contract. */
  elapsed_us = sample_us - ctx->last_us;
  if (elapsed_us > (uint64_t)ESY_TIME_MAX_SECONDS *
                       RTC_MICROSECONDS_PER_SECOND +
                       (RTC_MICROSECONDS_PER_SECOND - 1u))
    return ESY_ERR_STATE;
  whole_seconds = elapsed_us / RTC_MICROSECONDS_PER_SECOND;
  /* Decompose before multiplying to keep the complete UTC span in int64_t. */
  fraction = (elapsed_us % RTC_MICROSECONDS_PER_SECOND) *
                 (uint64_t)((int64_t)ESY_TIME_NANOSECONDS_PER_SECOND +
                            ctx->calibration_ppb) +
             ctx->fractional_remainder;
  nanoseconds = (int64_t)ctx->now.nanoseconds +
                (int64_t)whole_seconds * ctx->calibration_ppb +
                (int64_t)(fraction / RTC_MICROSECONDS_PER_SECOND);
  seconds = ctx->now.seconds + (int64_t)whole_seconds +
            nanoseconds / ESY_TIME_NANOSECONDS_PER_SECOND;
  remainder = nanoseconds % ESY_TIME_NANOSECONDS_PER_SECOND;
  if (remainder < 0) {
    remainder += ESY_TIME_NANOSECONDS_PER_SECOND;
    --seconds;
  }
  if ((seconds < 0) || (seconds > ESY_TIME_MAX_SECONDS)) return ESY_ERR_STATE;
  ctx->now.seconds = seconds;
  ctx->now.nanoseconds = (uint32_t)remainder;
  ctx->fractional_remainder =
      (uint32_t)(fraction % RTC_MICROSECONDS_PER_SECOND);
  ctx->last_us = sample_us;
  software_evaluate_alarm(ctx);
  return ESY_OK;
}

static esy_err_t software_get_datetime(void *storage,
                                      esy_datetime_t *out_datetime) {
  rtc_software_ctx_t state;
  rtc_software_ctx_t *ctx = &state;
  esy_err_t status = software_load(storage, ctx);
  if (out_datetime == NULL) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  status = software_advance(ctx);
  if (status != ESY_OK) return status;
  status = esy_datetime_from_timestamp(&ctx->now, out_datetime);
  if (status == ESY_OK) software_store(storage, ctx);
  return status;
}

static esy_err_t software_set_datetime(void *storage,
                                      const esy_datetime_t *datetime) {
  rtc_software_ctx_t state;
  rtc_software_ctx_t *ctx = &state;
  esy_timestamp_t timestamp;
  uint64_t sample_us;
  esy_err_t status = software_load(storage, ctx);
  if (status != ESY_OK) return status;
  status = esy_datetime_to_timestamp(datetime, &timestamp);
  if (status != ESY_OK) return status;
  status = ctx->monotonic_us(ctx->clock_ctx, &sample_us);
  if (status != ESY_OK) return status;
  ctx->now = timestamp;
  ctx->last_us = sample_us;
  ctx->fractional_remainder = 0u;
  software_evaluate_alarm(ctx);
  software_store(storage, ctx);
  return ESY_OK;
}

static esy_err_t software_alarm_set(void *storage,
                                   const esy_datetime_t *datetime) {
  rtc_software_ctx_t state;
  rtc_software_ctx_t *ctx = &state;
  esy_timestamp_t timestamp;
  esy_err_t status = software_load(storage, ctx);
  if (status != ESY_OK) return status;
  status = esy_datetime_to_timestamp(datetime, &timestamp);
  if (status != ESY_OK) return status;
  status = software_advance(ctx);
  if (status != ESY_OK) return status;
  if (timestamp.seconds <= ctx->now.seconds) return ESY_ERR_STATE;
  ctx->alarm_seconds = timestamp.seconds;
  ctx->alarm_armed = true;
  ctx->alarm_pending = false;
  software_store(storage, ctx);
  return ESY_OK;
}

static esy_err_t software_alarm_cancel(void *storage) {
  rtc_software_ctx_t state;
  rtc_software_ctx_t *ctx = &state;
  esy_err_t status = software_load(storage, ctx);
  if (status != ESY_OK) return status;
  ctx->alarm_armed = false;
  ctx->alarm_pending = false;
  software_store(storage, ctx);
  return ESY_OK;
}

static esy_err_t software_alarm_get_pending(void *storage, bool clear,
                                          bool *out_pending) {
  rtc_software_ctx_t state;
  rtc_software_ctx_t *ctx = &state;
  esy_err_t status = software_load(storage, ctx);
  if (out_pending == NULL) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  status = software_advance(ctx);
  if (status != ESY_OK) return status;
  *out_pending = ctx->alarm_pending;
  if (clear) ctx->alarm_pending = false;
  software_store(storage, ctx);
  return ESY_OK;
}

static esy_err_t software_set_calibration(void *storage,
                                         int32_t correction_ppb) {
  rtc_software_ctx_t state;
  rtc_software_ctx_t *ctx = &state;
  esy_err_t status = software_load(storage, ctx);
  if (status != ESY_OK) return status;
  if ((correction_ppb < -ESY_HAL_RTC_SOFTWARE_MAX_CALIBRATION_PPB) ||
      (correction_ppb > ESY_HAL_RTC_SOFTWARE_MAX_CALIBRATION_PPB))
    return ESY_ERR_INVALID_ARG;
  status = software_advance(ctx);
  if (status != ESY_OK) return status;
  ctx->calibration_ppb = correction_ppb;
  software_store(storage, ctx);
  return ESY_OK;
}

static esy_err_t software_get_calibration(void *storage,
                                         int32_t *out_correction_ppb) {
  rtc_software_ctx_t state;
  rtc_software_ctx_t *ctx = &state;
  esy_err_t status = software_load(storage, ctx);
  if (out_correction_ppb == NULL) return ESY_ERR_INVALID_ARG;
  if (status != ESY_OK) return status;
  *out_correction_ppb = ctx->calibration_ppb;
  return ESY_OK;
}

static const esy_hal_rtc_ops_t k_software_ops = {
    .capabilities = RTC_KNOWN_CAPS,
    .get_datetime = software_get_datetime,
    .set_datetime = software_set_datetime,
    .alarm_set = software_alarm_set,
    .alarm_cancel = software_alarm_cancel,
    .alarm_get_pending = software_alarm_get_pending,
    .set_calibration = software_set_calibration,
    .get_calibration = software_get_calibration,
};

esy_err_t esy_hal_rtc_software_init(
    esy_hal_rtc_software_t *software,
    const esy_hal_rtc_software_config_t *config) {
  rtc_software_ctx_t ctx = {0};
  rtc_software_ctx_t previous;
  esy_err_t status;
  if ((software == NULL) || (config == NULL) ||
      (config->monotonic_us == NULL)) return ESY_ERR_INVALID_ARG;
  memcpy(&previous, software, sizeof(previous));
  if (previous.initialized) return ESY_ERR_STATE;
  status = esy_datetime_to_timestamp(&config->initial_datetime, &ctx.now);
  if (status != ESY_OK) return status;
  status = config->monotonic_us(config->clock_ctx, &ctx.last_us);
  if (status != ESY_OK) return status;
  ctx.monotonic_us = config->monotonic_us;
  ctx.clock_ctx = config->clock_ctx;
  ctx.initialized = true;
  memcpy(software, &ctx, sizeof(ctx));
  return ESY_OK;
}

esy_err_t esy_hal_rtc_software_deinit(esy_hal_rtc_software_t *software) {
  rtc_software_ctx_t ctx;
  esy_err_t status = software_load(software, &ctx);
  if (status != ESY_OK) return status;
  memset(software, 0, sizeof(*software));
  return ESY_OK;
}

const esy_hal_rtc_ops_t *esy_hal_rtc_software_get_ops(void) {
  return &k_software_ops;
}
