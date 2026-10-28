/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_wakeup_timer.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) { platform_test_reset(); }
void tearDown(void) {}

void test_wakeup_timer_preserves_absolute_deadline_and_expiry(void) {
  esy_hal_wakeup_timer_t timer = {0};
  esy_hal_wakeup_timer_config_t config = {.alarm_index = 2u};
  uint64_t now_us;
  uint32_t state_mask;
  bool expired;

  platform_test_set_always_on_time(1000u, 0u, true);
  platform_test_set_wakeup_timer_expired(true);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_wakeup_timer_init(&timer, &config));
  TEST_ASSERT_EQUAL_UINT8(2u, platform_test_state()->wakeup_timer_alarm_index);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_hal_wakeup_timer_get_supported_state_mask(&timer, &state_mask));
  TEST_ASSERT_BITS_HIGH(ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE),
                        state_mask);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_wakeup_timer_now_atomic(&timer, &now_us));
  TEST_ASSERT_EQUAL_UINT64(1000u, now_us);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_wakeup_timer_prepare(
                                      &timer, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_wakeup_timer_arm_absolute_atomic(
                                      &timer, ESY_POWER_STATE_CPU_IDLE, 5000u));
  TEST_ASSERT_EQUAL_UINT64(5000u,
                           platform_test_state()->wakeup_timer_armed_time_us);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_hal_wakeup_timer_capture_and_cancel_atomic(&timer, &expired));
  TEST_ASSERT_TRUE(expired);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_wakeup_timer_finish(&timer));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_wakeup_timer_deinit(&timer));
}

void test_wakeup_timer_abort_balances_prepare(void) {
  esy_hal_wakeup_timer_t timer = {0};
  esy_hal_wakeup_timer_config_t config = {.alarm_index = 1u};

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_wakeup_timer_init(&timer, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_wakeup_timer_prepare(
                                      &timer, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_wakeup_timer_abort(&timer));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->wakeup_timer_abort_calls);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_wakeup_timer_deinit(&timer));
}

void test_wakeup_timer_reinit_cannot_lose_original_alarm(void) {
  esy_hal_wakeup_timer_t timer = {0};
  esy_hal_wakeup_timer_config_t config = {.alarm_index = 1u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_wakeup_timer_init(&timer, &config));
  config.alarm_index = 2u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_wakeup_timer_init(&timer, &config));
  TEST_ASSERT_EQUAL_UINT8(1u, platform_test_state()->wakeup_timer_alarm_index);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_wakeup_timer_deinit(&timer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_wakeup_timer_init(&timer, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_wakeup_timer_deinit(&timer));
}
