/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_rtc.h"
#include "esy_common/utils/time/esy_datetime.h"
#include "fake_esy_hal_rtc_provider.h"
#include "hal_test_fakes.h"
#include "unity.h"

#include <string.h>

typedef struct {
  esy_hal_rtc_t rtc;
  esy_hal_rtc_software_t software;
  fake_esy_hal_rtc_provider_t fake;
  fake_esy_hal_rtc_clock_t clock;
} rtc_test_ctx_t;

static rtc_test_ctx_t s_ctx;
static const esy_datetime_t k_epoch = {.year = 1970u, .month = 1u, .day = 1u};

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_rtc_provider_reset(&s_ctx.fake);
}

void tearDown(void) {}

static void start_software(uint64_t time_us, const esy_datetime_t *datetime) {
  esy_hal_rtc_software_config_t software_config = {
      .monotonic_us = fake_esy_hal_rtc_clock_sample,
      .clock_ctx = &s_ctx.clock,
      .initial_datetime = *datetime,
  };
  esy_hal_rtc_config_t facade_config = {
      .ops = esy_hal_rtc_software_get_ops(),
      .provider_ctx = &s_ctx.software,
  };
  s_ctx.clock.now_us = time_us;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_software_init(
                                    &s_ctx.software, &software_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_init(&s_ctx.rtc, &facade_config));
}

static void start_fake(void) {
  const esy_hal_rtc_config_t config = {
      .ops = fake_esy_hal_rtc_provider_get_ops(),
      .provider_ctx = &s_ctx.fake,
  };
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_init(&s_ctx.rtc, &config));
}

static int64_t read_seconds(void) {
  esy_datetime_t datetime;
  esy_timestamp_t timestamp;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                        esy_hal_rtc_get_datetime(&s_ctx.rtc, &datetime));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                        esy_datetime_to_timestamp(&datetime, &timestamp));
  return timestamp.seconds;
}

void test_rtc_facade_validates_binding_and_preserves_live_binding(void) {
  esy_hal_rtc_ops_t ops = *fake_esy_hal_rtc_provider_get_ops();
  esy_hal_rtc_config_t config = {.ops = &ops, .provider_ctx = &s_ctx.fake};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_init(NULL, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_init(&s_ctx.rtc, NULL));
  ops.get_datetime = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_init(&s_ctx.rtc, &config));
  ops = *fake_esy_hal_rtc_provider_get_ops();
  ops.alarm_cancel = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_init(&s_ctx.rtc, &config));
  ops = *fake_esy_hal_rtc_provider_get_ops();
  ops.get_calibration = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_init(&s_ctx.rtc, &config));
  config.ops = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_init(&s_ctx.rtc, &config));
  config.ops = fake_esy_hal_rtc_provider_get_ops();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_init(&s_ctx.rtc, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_init(&s_ctx.rtc, &config));
  TEST_ASSERT_EQUAL_INT64(0, read_seconds());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_deinit(&s_ctx.rtc));
  TEST_ASSERT_NULL(s_ctx.rtc.ops);
  TEST_ASSERT_NULL(s_ctx.rtc.provider_ctx);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_deinit(&s_ctx.rtc));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_deinit(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_init(&s_ctx.rtc, &config));
}

void test_rtc_facade_rejects_invalid_arguments_and_detached_operations(void) {
  esy_datetime_t datetime = k_epoch;
  uint32_t caps = 99u;
  bool pending = true;
  int32_t calibration = 99;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_get_caps(&s_ctx.rtc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_get_caps(&s_ctx.rtc, &caps));
  TEST_ASSERT_EQUAL_UINT32(99u, caps);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_get_datetime(&s_ctx.rtc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_get_datetime(&s_ctx.rtc, &datetime));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_set_datetime(&s_ctx.rtc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_set_datetime(&s_ctx.rtc, &datetime));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_alarm_set(&s_ctx.rtc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_set(&s_ctx.rtc, &datetime));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_cancel(&s_ctx.rtc));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_set_calibration(&s_ctx.rtc, 0));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_get_calibration(&s_ctx.rtc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_get_calibration(&s_ctx.rtc, &calibration));
  TEST_ASSERT_TRUE(pending);
  TEST_ASSERT_EQUAL_INT32(99, calibration);
}

void test_rtc_facade_masks_capabilities_and_optional_hooks_return_unsupported(void) {
  esy_hal_rtc_ops_t ops = *fake_esy_hal_rtc_provider_get_ops();
  esy_hal_rtc_config_t config = {.ops = &ops, .provider_ctx = &s_ctx.fake};
  uint32_t caps = 99u;
  int32_t calibration = 99;
  bool pending = true;
  ops.capabilities = UINT32_C(1) << 31;
  ops.alarm_set = NULL;
  ops.alarm_cancel = NULL;
  ops.alarm_get_pending = NULL;
  ops.set_calibration = NULL;
  ops.get_calibration = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_init(&s_ctx.rtc, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_get_caps(&s_ctx.rtc, &caps));
  TEST_ASSERT_EQUAL_UINT32(0u, caps);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_rtc_alarm_set(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_rtc_alarm_cancel(&s_ctx.rtc));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, true, &pending));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_rtc_set_calibration(&s_ctx.rtc, 1));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_rtc_get_calibration(&s_ctx.rtc, &calibration));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.fake.calls);
  TEST_ASSERT_TRUE(pending);
  TEST_ASSERT_EQUAL_INT32(99, calibration);
}

void test_rtc_facade_validates_dates_and_isolates_failed_provider_outputs(void) {
  esy_datetime_t datetime = {.year = 2025u, .month = 2u, .day = 29u};
  esy_datetime_t output = k_epoch;
  int32_t calibration = 99;
  bool pending = true;
  start_fake();
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_set_datetime(&s_ctx.rtc, &datetime));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_alarm_set(&s_ctx.rtc, &datetime));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.fake.calls);
  s_ctx.fake.datetime = datetime;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_rtc_get_datetime(&s_ctx.rtc, &output));
  TEST_ASSERT_EQUAL_UINT16(1970u, output.year);
  s_ctx.fake.status = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_rtc_get_datetime(&s_ctx.rtc, &output));
  TEST_ASSERT_EQUAL_UINT16(1970u, output.year);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, true, &pending));
  TEST_ASSERT_TRUE(pending);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_rtc_get_calibration(&s_ctx.rtc, &calibration));
  TEST_ASSERT_EQUAL_INT32(99, calibration);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_rtc_alarm_set(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_rtc_alarm_cancel(&s_ctx.rtc));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_hal_rtc_set_calibration(&s_ctx.rtc, 1));
  s_ctx.fake.status = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_calibration(&s_ctx.rtc, -10));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_get_calibration(&s_ctx.rtc, &calibration));
  TEST_ASSERT_EQUAL_INT32(-10, calibration);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_set(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_cancel(&s_ctx.rtc));
}

void test_software_rtc_init_failure_retry_and_complete_lifecycle(void) {
  esy_hal_rtc_software_config_t config = {
      .monotonic_us = fake_esy_hal_rtc_clock_sample,
      .clock_ctx = &s_ctx.clock,
      .initial_datetime = k_epoch,
  };
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_software_init(NULL, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_software_init(&s_ctx.software, NULL));
  config.monotonic_us = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_software_init(&s_ctx.software, &config));
  config.monotonic_us = fake_esy_hal_rtc_clock_sample;
  config.initial_datetime.month = 13u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_software_init(&s_ctx.software, &config));
  config.initial_datetime = k_epoch;
  s_ctx.clock.status = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_rtc_software_init(&s_ctx.software, &config));
  s_ctx.clock.status = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_software_init(&s_ctx.software, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_software_init(&s_ctx.software, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_software_deinit(&s_ctx.software));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_software_deinit(&s_ctx.software));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_software_deinit(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_software_init(&s_ctx.software, &config));
}

void test_software_rtc_uninitialized_and_null_provider_are_reported(void) {
  esy_hal_rtc_config_t config = {.ops = esy_hal_rtc_software_get_ops(), .provider_ctx = &s_ctx.software};
  esy_datetime_t datetime;
  bool pending;
  int32_t correction;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_init(&s_ctx.rtc, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_get_datetime(&s_ctx.rtc, &datetime));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_set(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_cancel(&s_ctx.rtc));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_set_calibration(&s_ctx.rtc, 0));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_get_calibration(&s_ctx.rtc, &correction));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_deinit(&s_ctx.rtc));
  config.provider_ctx = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_init(&s_ctx.rtc, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_get_datetime(&s_ctx.rtc, &datetime));
}

void test_software_rtc_advances_across_counter_wrap_and_leap_day(void) {
  esy_datetime_t initial = {.year = 2024u, .month = 2u, .day = 28u, .hour = 23u, .minute = 59u, .second = 59u};
  esy_datetime_t output;
  uint32_t caps;
  start_software(UINT64_MAX - UINT64_C(500000), &initial);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_get_caps(&s_ctx.rtc, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_RTC_CAP_ALARM | ESY_RTC_CAP_CALIBRATION, caps);
  s_ctx.clock.now_us += UINT64_C(999999);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_get_datetime(&s_ctx.rtc, &output));
  TEST_ASSERT_EQUAL_UINT8(28u, output.day);
  TEST_ASSERT_EQUAL_UINT8(59u, output.second);
  ++s_ctx.clock.now_us;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_get_datetime(&s_ctx.rtc, &output));
  TEST_ASSERT_EQUAL_UINT8(29u, output.day);
  TEST_ASSERT_EQUAL_UINT8(0u, output.second);
  s_ctx.clock.now_us += UINT64_C(86400000000);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_get_datetime(&s_ctx.rtc, &output));
  TEST_ASSERT_EQUAL_UINT8(3u, output.month);
  TEST_ASSERT_EQUAL_UINT8(1u, output.day);
}

void test_software_alarm_is_latched_once_at_exact_wrapping_deadline(void) {
  esy_datetime_t alarm = k_epoch;
  bool pending = false;
  alarm.second = 1u;
  start_software(UINT64_MAX - UINT64_C(500000), &k_epoch);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_set(&s_ctx.rtc, &alarm));
  s_ctx.clock.now_us += UINT64_C(999999);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_FALSE(pending);
  ++s_ctx.clock.now_us;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_TRUE(pending);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, true, &pending));
  TEST_ASSERT_TRUE(pending);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, true, &pending));
  TEST_ASSERT_FALSE(pending);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  s_ctx.clock.now_us += UINT64_C(2000000);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_FALSE(pending);
  alarm.second = 3u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_set(&s_ctx.rtc, &alarm));
  s_ctx.clock.now_us += UINT64_C(1000000);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, true, &pending));
  TEST_ASSERT_TRUE(pending);
}

void test_software_alarm_cancel_and_clock_rebase_never_reactivate_alarm(void) {
  esy_datetime_t alarm = k_epoch;
  bool pending = true;
  alarm.second = 1u;
  start_software(0u, &k_epoch);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_set(&s_ctx.rtc, &alarm));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_cancel(&s_ctx.rtc));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_cancel(&s_ctx.rtc));
  s_ctx.clock.now_us = UINT64_MAX;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_TRUE(pending);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  s_ctx.clock.now_us += UINT64_C(2000000);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_FALSE(pending);
  /* A full source cycle exceeds the valid observation range. Explicit rebase
   * resumes service while retaining canceled/completed one-shot state. */
  s_ctx.clock.now_us = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  s_ctx.clock.now_us += UINT64_C(2000000);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, true, &pending));
  TEST_ASSERT_FALSE(pending);
}

void test_software_alarm_clock_steps_and_invalid_replacement_preserve_state(void) {
  esy_datetime_t alarm = k_epoch;
  esy_datetime_t future = k_epoch;
  bool pending;
  alarm.second = 10u;
  future.second = 11u;
  start_software(0u, &k_epoch);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_set(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_set(&s_ctx.rtc, &alarm));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_set(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_datetime(&s_ctx.rtc, &future));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_TRUE(pending);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, true, &pending));
  TEST_ASSERT_TRUE(pending);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_set(&s_ctx.rtc, &alarm));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_cancel(&s_ctx.rtc));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, true, &pending));
  TEST_ASSERT_FALSE(pending);
}

void test_software_calibration_applies_signed_rate_without_retroactive_change(void) {
  int32_t correction;
  start_software(0u, &k_epoch);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_set_calibration(&s_ctx.rtc, 1000001));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_rtc_set_calibration(&s_ctx.rtc, -1000001));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_get_calibration(&s_ctx.rtc, &correction));
  TEST_ASSERT_EQUAL_INT32(0, correction);
  s_ctx.clock.now_us += UINT64_C(1000000000);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_calibration(&s_ctx.rtc, 1000000));
  TEST_ASSERT_EQUAL_INT64(1000, read_seconds());
  s_ctx.clock.now_us += UINT64_C(1000000000);
  TEST_ASSERT_EQUAL_INT64(2001, read_seconds());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_calibration(&s_ctx.rtc, -1000000));
  s_ctx.clock.now_us += UINT64_C(1000000000);
  TEST_ASSERT_EQUAL_INT64(3000, read_seconds());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_get_calibration(&s_ctx.rtc, &correction));
  TEST_ASSERT_EQUAL_INT32(-1000000, correction);
}

void test_software_calibration_retains_fraction_during_frequent_samples(void) {
  esy_datetime_t alarm = k_epoch;
  bool pending;
  alarm.second = 1u;
  start_software(0u, &k_epoch);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_calibration(&s_ctx.rtc, -1));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_set(&s_ctx.rtc, &alarm));
  for (uint32_t i = 0; i < 1000u; ++i) {
    s_ctx.clock.now_us += 1000u;
    TEST_ASSERT_EQUAL_INT64(0, read_seconds());
  }
  s_ctx.clock.now_us += 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  TEST_ASSERT_TRUE(pending);
}

void test_software_rtc_range_exhaustion_clock_errors_and_explicit_recovery(void) {
  esy_datetime_t last = {.year = 9999u, .month = 12u, .day = 31u, .hour = 23u, .minute = 59u, .second = 59u};
  esy_datetime_t output = k_epoch;
  esy_datetime_t alarm = last;
  bool pending = true;
  start_software(0u, &last);
  TEST_ASSERT_EQUAL_INT64(ESY_TIME_MAX_SECONDS, read_seconds());
  s_ctx.clock.now_us = 1000000u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_get_datetime(&s_ctx.rtc, &output));
  TEST_ASSERT_EQUAL_UINT16(1970u, output.year);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_set_calibration(&s_ctx.rtc, 1));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_set(&s_ctx.rtc, &alarm));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_alarm_get_pending(&s_ctx.rtc, false, &pending));
  s_ctx.clock.status = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_rtc_get_datetime(&s_ctx.rtc, &output));
  TEST_ASSERT_EQUAL_UINT16(1970u, output.year);
  s_ctx.clock.status = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_rtc_set_datetime(&s_ctx.rtc, &k_epoch));
  TEST_ASSERT_EQUAL_INT64(0, read_seconds());
  /* The entire supported UTC span is accepted without multiplication overflow. */
  s_ctx.clock.now_us += (uint64_t)ESY_TIME_MAX_SECONDS * UINT64_C(1000000);
  TEST_ASSERT_EQUAL_INT64(ESY_TIME_MAX_SECONDS, read_seconds());
  s_ctx.clock.now_us += UINT64_C(999999);
  TEST_ASSERT_EQUAL_INT64(ESY_TIME_MAX_SECONDS, read_seconds());
  ++s_ctx.clock.now_us;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_rtc_get_datetime(&s_ctx.rtc, &output));
}
