/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_DATETIME_H
#define ESY_DATETIME_H

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/base/esy_time_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Validate a UTC Gregorian calendar without changing it.
 * @param datetime Borrowed value, valid only for the duration of the call.
 * @return ESY_OK for a valid value, ESY_ERR_INVALID_ARG for NULL or a range error.
 * @note Pure, bounded, reentrant and ISR-safe; no hardware or OS is accessed.
 */
esy_err_t esy_datetime_validate(const esy_datetime_t *datetime);

/**
 * @brief Convert UTC calendar time to Unix seconds with zero nanoseconds.
 * @param datetime Borrowed UTC value in the supported 1970..9999 interval.
 * @param[out] out_timestamp Result, unchanged on failure; must not alias input.
 * @return ESY_OK or ESY_ERR_INVALID_ARG for NULL or an invalid calendar value.
 * @note Pure, bounded, reentrant and ISR-safe. No timezone or libc state is used.
 */
esy_err_t esy_datetime_to_timestamp(const esy_datetime_t *datetime,
                                  esy_timestamp_t *out_timestamp);

/**
 * @brief Convert a normalized Unix timestamp to a UTC Gregorian calendar.
 * @param timestamp Borrowed timestamp in the esy_timestamp_t supported range.
 * Nanoseconds are validated and then discarded, without rounding.
 * @param[out] out_datetime Result, unchanged on failure; must not alias input.
 * @return ESY_OK or ESY_ERR_INVALID_ARG for NULL or an invalid timestamp.
 * @note Pure, bounded, reentrant and ISR-safe. No resource lifecycle is required.
 */
esy_err_t esy_datetime_from_timestamp(const esy_timestamp_t *timestamp,
                                    esy_datetime_t *out_datetime);

#ifdef __cplusplus
}
#endif

#endif /* ESY_DATETIME_H */
