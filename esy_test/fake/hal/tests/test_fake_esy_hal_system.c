/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include "esy_hal/esy_hal_system.h"
#include "fake_esy_hal_system.h"

#define TEST_RESET_HOOK_DELAY_MS 77u

static void test_reset_hook(void) {
  fake_esy_hal_system_get_ctx()->last_delay_ms = TEST_RESET_HOOK_DELAY_MS;
}

void setUp(void) { fake_esy_hal_system_reset(); }

void tearDown(void) {}

void test_fake_platform_system_reset_request_is_observable(void) {
  fake_esy_hal_system_set_reset_hook(test_reset_hook);

  esy_hal_system_reset();

  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_system_get_reset_count());
  TEST_ASSERT_EQUAL_UINT32(TEST_RESET_HOOK_DELAY_MS,
                           fake_esy_hal_system_get_ctx()->last_delay_ms);
}

void test_fake_platform_system_reset_clears_request_state_and_hook(void) {
  fake_esy_hal_system_set_reset_hook(test_reset_hook);
  esy_hal_system_reset();

  fake_esy_hal_system_reset();
  esy_hal_system_reset();

  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_system_get_reset_count());
  TEST_ASSERT_EQUAL_UINT32(0u,
                           fake_esy_hal_system_get_ctx()->last_delay_ms);
}

void test_fake_platform_system_clear_reset_requests_preserves_other_state(
    void) {
  fake_esy_hal_system_set_tick(123u);
  esy_hal_system_reset();

  fake_esy_hal_system_clear_reset_requests();

  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_reset_count());
  TEST_ASSERT_EQUAL_UINT32(123u, esy_hal_system_get_tick());
}

void test_fake_platform_system_exposes_64_bit_always_on_time(void) {
  uint64_t now_us;

  fake_esy_hal_system_get_ctx()->time_us = (uint64_t)UINT32_MAX + 1234u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_system_always_on_time_us(&now_us));
  TEST_ASSERT_EQUAL_UINT64((uint64_t)UINT32_MAX + 1234u, now_us);
  TEST_ASSERT_EQUAL_UINT32(1233u, esy_hal_system_time_32_us());
}

void test_fake_platform_system_tick_interrupt_advances_one_millisecond(void) {
  fake_esy_hal_system_set_tick(123u);

  esy_hal_system_tick_from_isr();

  TEST_ASSERT_EQUAL_UINT32(124u, esy_hal_system_get_tick());
  TEST_ASSERT_EQUAL_UINT32(124000u, esy_hal_system_time_32_us());
}

void test_fake_platform_system_reads_tick_phase_without_stopping_tick(void) {
  uint32_t partial_tick_us = 0u;

  fake_esy_hal_system_get_ctx()->partial_tick_us = 500u;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_system_get_tick_partial_us(1000u, &partial_tick_us));

  TEST_ASSERT_EQUAL_UINT32(500u, partial_tick_us);
  TEST_ASSERT_FALSE(fake_esy_hal_system_get_ctx()->tick_suspended);
}

void test_large_millisecond_steps_preserve_the_full_microsecond_clock(void) {
  fake_esy_hal_system_set_tick(5000000u);
  TEST_ASSERT_EQUAL_UINT64(5000000000ULL, fake_esy_hal_system_get_ctx()->time_us);
  fake_esy_hal_system_advance_tick(5000000u);
  TEST_ASSERT_EQUAL_UINT64(10000000000ULL, fake_esy_hal_system_get_ctx()->time_us);
  esy_hal_system_delay_ms(5000000u);
  TEST_ASSERT_EQUAL_UINT64(15000000000ULL, fake_esy_hal_system_get_ctx()->time_us);
  fake_esy_hal_system_set_tick(UINT32_MAX - 1u);
  fake_esy_hal_system_advance_tick(3u);
  TEST_ASSERT_EQUAL_UINT32(1u, esy_hal_system_get_tick());
  TEST_ASSERT_EQUAL_UINT64(((uint64_t)UINT32_MAX + 2u) * 1000u,
      fake_esy_hal_system_get_ctx()->time_us);
}
