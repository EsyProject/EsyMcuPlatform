/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/time/esy_time.h"
#include "esy_middleware/time/esy_time_rtc.h"

typedef struct {
  esy_time_t time;
  esy_hal_rtc_t rtc;
  esy_hal_rtc_software_t software;
  uint64_t monotonic_us;
  esy_err_t clock_result;
} time_rtc_ctx_t;

static time_rtc_ctx_t s_ctx;

static esy_err_t read_clock(void *ctx, uint64_t *out_time_us) {
  (void)ctx;
  if (s_ctx.clock_result == ESY_OK) {
    *out_time_us = s_ctx.monotonic_us;
  }
  return s_ctx.clock_result;
}

void setUp(void) {
  s_ctx = (time_rtc_ctx_t){0};
  const esy_time_config_t time_config = {read_clock, NULL};
  const esy_hal_rtc_software_config_t rtc_config = {
      read_clock, NULL, {2000u, 1u, 1u, 0u, 0u, 0u}};
  const esy_hal_rtc_config_t binding = {
      esy_hal_rtc_software_get_ops(), &s_ctx.software};
  TEST_ASSERT_EQUAL(ESY_OK, esy_time_init(&s_ctx.time, &time_config));
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_hal_rtc_software_init(&s_ctx.software, &rtc_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_rtc_init(&s_ctx.rtc, &binding));
}

void tearDown(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_rtc_deinit(&s_ctx.rtc));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_rtc_software_deinit(&s_ctx.software));
  TEST_ASSERT_EQUAL(ESY_OK, esy_time_deinit(&s_ctx.time));
}

void test_time_rtc_load_seeds_utc_from_calendar_and_time_continues(void) {
  esy_timestamp_t now;
  TEST_ASSERT_EQUAL(ESY_OK, esy_time_rtc_load(&s_ctx.time, &s_ctx.rtc));
  s_ctx.monotonic_us += UINT64_C(1500000);
  TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &now));
  TEST_ASSERT_EQUAL_INT64(INT64_C(946684801), now.seconds);
  TEST_ASSERT_EQUAL_UINT32(500000000u, now.nanoseconds);
}

void test_time_rtc_store_discards_fraction_and_sets_utc_calendar(void) {
  const esy_timestamp_t synchronized = {INT64_C(951827696), 999999999u};
  esy_datetime_t datetime;
  TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &synchronized));
  TEST_ASSERT_EQUAL(ESY_OK, esy_time_rtc_store(&s_ctx.time, &s_ctx.rtc));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_rtc_get_datetime(&s_ctx.rtc, &datetime));
  TEST_ASSERT_EQUAL_UINT16(2000u, datetime.year);
  TEST_ASSERT_EQUAL_UINT8(2u, datetime.month);
  TEST_ASSERT_EQUAL_UINT8(29u, datetime.day);
  TEST_ASSERT_EQUAL_UINT8(12u, datetime.hour);
  TEST_ASSERT_EQUAL_UINT8(34u, datetime.minute);
  TEST_ASSERT_EQUAL_UINT8(56u, datetime.second);
}

void test_time_rtc_propagates_invalid_state_and_provider_errors(void) {
  esy_hal_rtc_t detached = {0};
  esy_time_t uninitialized = {0};
  esy_timestamp_t now;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_time_rtc_store(&s_ctx.time, &s_ctx.rtc));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_time_rtc_load(&s_ctx.time, &detached));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_time_rtc_load(&uninitialized, &s_ctx.rtc));
  TEST_ASSERT_EQUAL(ESY_OK, esy_time_rtc_load(&s_ctx.time, &s_ctx.rtc));
  s_ctx.clock_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
                    esy_time_rtc_load(&s_ctx.time, &s_ctx.rtc));
  TEST_ASSERT_EQUAL(ESY_ERR_IO,
                    esy_time_rtc_store(&s_ctx.time, &s_ctx.rtc));
  s_ctx.clock_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &now));
  TEST_ASSERT_EQUAL_INT64(INT64_C(946684800), now.seconds);
}

void test_time_rtc_rejects_null_handles(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_rtc_load(NULL, &s_ctx.rtc));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_rtc_load(&s_ctx.time, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_rtc_store(NULL, &s_ctx.rtc));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_rtc_store(&s_ctx.time, NULL));
}
