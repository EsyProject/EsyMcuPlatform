/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_common/utils/time/esy_datetime.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_datetime_matches_independent_unix_calendar_vectors(void) {
  static const struct {
    esy_datetime_t datetime;
    int64_t seconds;
  } k_vectors[] = {
      {{1970u, 1u, 1u, 0u, 0u, 0u}, INT64_C(0)},
      {{1970u, 1u, 2u, 0u, 0u, 0u}, INT64_C(86400)},
      {{2000u, 2u, 29u, 12u, 34u, 56u}, INT64_C(951827696)},
      {{2036u, 2u, 7u, 6u, 28u, 16u}, INT64_C(2085978496)},
      {{2038u, 1u, 19u, 3u, 14u, 8u}, INT64_C(2147483648)},
      {{2100u, 3u, 1u, 0u, 0u, 0u}, INT64_C(4107542400)},
      {{2400u, 2u, 29u, 0u, 0u, 0u}, INT64_C(13574563200)},
      {{9999u, 12u, 31u, 23u, 59u, 59u}, ESY_TIME_MAX_SECONDS}};

  for (size_t i = 0u; i < sizeof(k_vectors) / sizeof(k_vectors[0]); ++i) {
    esy_timestamp_t timestamp = {0};
    esy_datetime_t result = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_datetime_validate(&k_vectors[i].datetime));
    TEST_ASSERT_EQUAL(ESY_OK, esy_datetime_to_timestamp(&k_vectors[i].datetime,
                                                    &timestamp));
    TEST_ASSERT_EQUAL_INT64(k_vectors[i].seconds, timestamp.seconds);
    TEST_ASSERT_EQUAL_UINT32(0u, timestamp.nanoseconds);
    timestamp.nanoseconds = ESY_TIME_NANOSECONDS_PER_SECOND - 1u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_datetime_from_timestamp(&timestamp, &result));
    TEST_ASSERT_EQUAL_UINT16(k_vectors[i].datetime.year, result.year);
    TEST_ASSERT_EQUAL_UINT8(k_vectors[i].datetime.month, result.month);
    TEST_ASSERT_EQUAL_UINT8(k_vectors[i].datetime.day, result.day);
    TEST_ASSERT_EQUAL_UINT8(k_vectors[i].datetime.hour, result.hour);
    TEST_ASSERT_EQUAL_UINT8(k_vectors[i].datetime.minute, result.minute);
    TEST_ASSERT_EQUAL_UINT8(k_vectors[i].datetime.second, result.second);
  }
}

void test_datetime_rejects_invalid_fields_and_non_gregorian_leap_days(void) {
  static const esy_datetime_t k_invalid[] = {
      {1969u, 12u, 31u, 23u, 59u, 59u}, {10000u, 1u, 1u, 0u, 0u, 0u},
      {2024u, 0u, 1u, 0u, 0u, 0u}, {2024u, 13u, 1u, 0u, 0u, 0u},
      {2024u, 1u, 0u, 0u, 0u, 0u}, {2024u, 1u, 32u, 0u, 0u, 0u},
      {2023u, 2u, 29u, 0u, 0u, 0u}, {2100u, 2u, 29u, 0u, 0u, 0u},
      {2024u, 2u, 30u, 0u, 0u, 0u}, {2024u, 4u, 31u, 0u, 0u, 0u},
      {2024u, 1u, 1u, 24u, 0u, 0u}, {2024u, 1u, 1u, 0u, 60u, 0u},
      {2024u, 1u, 1u, 0u, 0u, 60u}};
  esy_timestamp_t timestamp = {INT64_C(123), 456u};

  for (size_t i = 0u; i < sizeof(k_invalid) / sizeof(k_invalid[0]); ++i) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_datetime_validate(&k_invalid[i]));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_datetime_to_timestamp(&k_invalid[i], &timestamp));
    TEST_ASSERT_EQUAL_INT64(123, timestamp.seconds);
    TEST_ASSERT_EQUAL_UINT32(456u, timestamp.nanoseconds);
  }
}

void test_datetime_conversion_rejects_null_and_out_of_range_timestamps(void) {
  const esy_datetime_t valid = {2024u, 1u, 1u, 0u, 0u, 0u};
  esy_datetime_t result = valid;
  const esy_timestamp_t invalid[] = {
      {-1, 0u}, {ESY_TIME_MAX_SECONDS + 1, 0u},
      {0, ESY_TIME_NANOSECONDS_PER_SECOND}};
  esy_timestamp_t timestamp = {0};

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_datetime_validate(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_datetime_to_timestamp(NULL, &timestamp));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_datetime_to_timestamp(&valid, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_datetime_from_timestamp(NULL, &result));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_datetime_from_timestamp(&timestamp, NULL));
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_datetime_from_timestamp(&invalid[i], &result));
    TEST_ASSERT_EQUAL_MEMORY(&valid, &result, sizeof(result));
  }
}

void test_datetime_round_trips_every_calendar_day_in_supported_interval(void) {
  /* Day-by-day coverage catches century boundaries and every month length. */
  esy_timestamp_t timestamp = {0, 0u};
  for (; timestamp.seconds <= ESY_TIME_MAX_SECONDS;
       timestamp.seconds += INT64_C(86400)) {
    esy_datetime_t datetime;
    esy_timestamp_t result;
    TEST_ASSERT_EQUAL(ESY_OK, esy_datetime_from_timestamp(&timestamp, &datetime));
    TEST_ASSERT_EQUAL(ESY_OK, esy_datetime_to_timestamp(&datetime, &result));
    TEST_ASSERT_EQUAL_INT64(timestamp.seconds, result.seconds);
  }
}
