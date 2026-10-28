/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/utils/time/esy_datetime.h"

#include <stdbool.h>

#define MONTHS_PER_YEAR 12u
#define SECONDS_PER_MINUTE INT64_C(60)
#define SECONDS_PER_HOUR INT64_C(3600)
#define SECONDS_PER_DAY INT64_C(86400)
#define DAYS_PER_400_YEARS UINT32_C(146097)
#define UNIX_EPOCH_ABSOLUTE_DAY UINT32_C(719162)

static const uint8_t k_month_days[MONTHS_PER_YEAR] = {
    31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};

static bool is_leap_year(uint32_t year) {
  return (year % 4u == 0u) && ((year % 100u != 0u) || (year % 400u == 0u));
}

static uint32_t month_days(uint32_t year, uint32_t month) {
  return k_month_days[month - 1u] +
         ((month == 2u && is_leap_year(year)) ? 1u : 0u);
}

static uint32_t days_before_year(uint32_t year) {
  const uint32_t previous = year - 1u;
  return 365u * previous + previous / 4u - previous / 100u + previous / 400u;
}

esy_err_t esy_datetime_validate(const esy_datetime_t *datetime) {
  if (datetime == NULL || datetime->year < ESY_TIME_MIN_YEAR ||
      datetime->year > ESY_TIME_MAX_YEAR || datetime->month == 0u ||
      datetime->month > MONTHS_PER_YEAR || datetime->day == 0u ||
      datetime->hour > 23u || datetime->minute > 59u || datetime->second > 59u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (datetime->day > month_days(datetime->year, datetime->month)) {
    return ESY_ERR_INVALID_ARG;
  }
  return ESY_OK;
}

esy_err_t esy_datetime_to_timestamp(const esy_datetime_t *datetime,
                                  esy_timestamp_t *out_timestamp) {
  uint32_t days;
  esy_timestamp_t result;
  if (out_timestamp == NULL || esy_datetime_validate(datetime) != ESY_OK) {
    return ESY_ERR_INVALID_ARG;
  }
  days = days_before_year(datetime->year) - UNIX_EPOCH_ABSOLUTE_DAY;
  for (uint32_t month = 1u; month < datetime->month; ++month) {
    days += month_days(datetime->year, month);
  }
  days += datetime->day - 1u;
  result.seconds = (int64_t)days * SECONDS_PER_DAY +
                   datetime->hour * SECONDS_PER_HOUR +
                   datetime->minute * SECONDS_PER_MINUTE + datetime->second;
  result.nanoseconds = 0u;
  *out_timestamp = result;
  return ESY_OK;
}

esy_err_t esy_datetime_from_timestamp(const esy_timestamp_t *timestamp,
                                    esy_datetime_t *out_datetime) {
  uint32_t days;
  uint32_t year;
  uint32_t month = 1u;
  uint32_t day_seconds;
  esy_datetime_t result;
  if (timestamp == NULL || out_datetime == NULL || timestamp->seconds < 0 ||
      timestamp->seconds > ESY_TIME_MAX_SECONDS ||
      timestamp->nanoseconds >= ESY_TIME_NANOSECONDS_PER_SECOND) {
    return ESY_ERR_INVALID_ARG;
  }
  days = (uint32_t)(timestamp->seconds / SECONDS_PER_DAY) +
         UNIX_EPOCH_ABSOLUTE_DAY;
  /* Skip complete Gregorian cycles; at most 399 year iterations remain. */
  year = 1u + (days / DAYS_PER_400_YEARS) * 400u;
  days %= DAYS_PER_400_YEARS;
  while (days >= (is_leap_year(year) ? 366u : 365u)) {
    days -= is_leap_year(year) ? 366u : 365u;
    ++year;
  }
  while (days >= month_days(year, month)) {
    days -= month_days(year, month);
    ++month;
  }
  day_seconds = (uint32_t)(timestamp->seconds % SECONDS_PER_DAY);
  result.year = (uint16_t)year;
  result.month = (uint8_t)month;
  result.day = (uint8_t)(days + 1u);
  result.hour = (uint8_t)(day_seconds / (uint32_t)SECONDS_PER_HOUR);
  result.minute = (uint8_t)((day_seconds % (uint32_t)SECONDS_PER_HOUR) /
                          (uint32_t)SECONDS_PER_MINUTE);
  result.second = (uint8_t)(day_seconds % (uint32_t)SECONDS_PER_MINUTE);
  *out_datetime = result;
  return ESY_OK;
}
