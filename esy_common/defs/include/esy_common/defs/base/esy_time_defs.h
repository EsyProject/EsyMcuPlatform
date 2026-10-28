/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TIME_DEFS_H
#define ESY_TIME_DEFS_H

#include <stdint.h>

/** @brief Earliest supported Gregorian calendar year (the Unix epoch). */
#define ESY_TIME_MIN_YEAR 1970u
/** @brief Latest supported Gregorian calendar year. */
#define ESY_TIME_MAX_YEAR 9999u
/** @brief Last supported Unix second, 9999-12-31 23:59:59 UTC. */
#define ESY_TIME_MAX_SECONDS INT64_C(253402300799)
/** @brief Number of nanoseconds in a second; fractions must be smaller. */
#define ESY_TIME_NANOSECONDS_PER_SECOND UINT32_C(1000000000)

/**
 * @brief UTC Gregorian calendar value, with one-second resolution.
 *
 * Years are 1970..9999; month/day are one-based. Leap seconds, local time,
 * daylight saving and timezone policy are outside this value contract.
 */
typedef struct esy_datetime_s {
  uint16_t year;  /**< Gregorian year, ESY_TIME_MIN_YEAR..ESY_TIME_MAX_YEAR. */
  uint8_t month;  /**< Month, 1..12. */
  uint8_t day;    /**< Day, 1..the number of days in the selected month. */
  uint8_t hour;   /**< Hour, 0..23. */
  uint8_t minute; /**< Minute, 0..59. */
  uint8_t second; /**< Second, 0..59; leap seconds are not represented. */
} esy_datetime_t;

/**
 * @brief Normalized Unix UTC timestamp independent of libc time_t width.
 *
 * This is POSIX time: leap seconds are not counted. The supported interval is
 * 1970-01-01 through 9999-12-31, inclusive. Values are copied, own no resources
 * and carry no synchronization guarantees; their owner serializes mutation.
 */
typedef struct esy_timestamp_s {
  int64_t seconds;       /**< Seconds since 1970-01-01; 0..ESY_TIME_MAX_SECONDS. */
  uint32_t nanoseconds;  /**< Fraction, 0..ESY_TIME_NANOSECONDS_PER_SECOND-1. */
} esy_timestamp_t;

#endif /* ESY_TIME_DEFS_H */
