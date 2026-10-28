/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_RTC_PROVIDER_H
#define FAKE_ESY_HAL_RTC_PROVIDER_H

#include "esy_hal/esy_hal_rtc.h"

/** @brief Caller-owned, resettable RTC provider for host contract tests. */
typedef struct {
  esy_datetime_t datetime; /**< Calendar returned by read; may be malformed. */
  esy_datetime_t alarm; /**< Most recently submitted calendar alarm. */
  esy_err_t status; /**< Status returned by all hooks. */
  uint32_t calls; /**< Number of hook calls. */
  int32_t calibration_ppb; /**< Effective fake calibration. */
  bool pending; /**< Latched fake alarm flag. */
} fake_esy_hal_rtc_provider_t;

/** @brief Controlled unsigned 64-bit microsecond clock, without wall-time waits. */
typedef struct {
  uint64_t now_us; /**< Arbitrary modulo-2^64 sample. */
  esy_err_t status; /**< Failure injected into the next and later samples. */
  uint32_t calls; /**< Number of sample attempts. */
} fake_esy_hal_rtc_clock_t;

/** @brief Reset provider to Unix epoch, successful hooks and no pending alarm. */
void fake_esy_hal_rtc_provider_reset(fake_esy_hal_rtc_provider_t *provider);
/** @brief Immutable complete provider; state and injected errors are caller-owned. */
const esy_hal_rtc_ops_t *fake_esy_hal_rtc_provider_get_ops(void);
/** @brief Sample a fake_esy_hal_rtc_clock_t; output unchanged on injected errors. */
esy_err_t fake_esy_hal_rtc_clock_sample(void *ctx, uint64_t *out_time_us);

#endif /* FAKE_ESY_HAL_RTC_PROVIDER_H */
