/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TIME_RTC_H
#define ESY_TIME_RTC_H

#include "esy_hal/esy_hal_rtc.h"
#include "esy_middleware/time/esy_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Seed the UTC time service from an attached calendar RTC.
 * @param[in,out] time Initialized time service; remains unchanged on failure.
 * @param[in] rtc Initialized RTC facade whose calendar is maintained in UTC.
 * @return ESY_OK, ESY_ERR_INVALID_ARG for NULL, or calendar/provider/time error.
 * @note Synchronous, non-ISR, externally serialized with both owners. Borrows
 * both handles only during this call; it does not attach, detach or own either.
 * Calendar precision is one second. RTC read and monotonic-provider latency
 * contribute to synchronization uncertainty; no fractional correction is made.
 */
esy_err_t esy_time_rtc_load(esy_time_t *time, esy_hal_rtc_t *rtc);

/**
 * @brief Store current UTC in an attached RTC, discarding fractional seconds.
 * @param[in] time Initialized service with valid UTC (for example after SNTP).
 * @param[in,out] rtc Initialized UTC calendar RTC; provider owns hardware policy.
 * @return ESY_OK, ESY_ERR_INVALID_ARG for NULL, or calendar/provider/time error.
 * @note Same synchronous ownership/context contract as esy_time_rtc_load. RTC
 * alarms follow their provider's calendar-step semantics. This does not persist
 * software-provider state or make the RTC retain time through power loss.
 */
esy_err_t esy_time_rtc_store(esy_time_t *time, esy_hal_rtc_t *rtc);

#ifdef __cplusplus
}
#endif

#endif /* ESY_TIME_RTC_H */
