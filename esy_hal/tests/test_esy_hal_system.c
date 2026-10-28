/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_system.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) { platform_test_reset(); }

void tearDown(void) {}

void test_esy_hal_system_init_initializes_board_before_driver(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_system_init());

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  TEST_ASSERT_EQUAL_UINT32(250000u, state->last_system_config.sys_clock_khz);
  TEST_ASSERT_TRUE(state->last_system_config.enable_us_tick);
}

void test_esy_hal_system_queries_route_to_driver(void) {
  uint32_t partial_tick_us = 0u;
  uint64_t always_on_time_us = 0u;

  platform_test_set_always_on_time(1000u, 0u, true);
  TEST_ASSERT_EQUAL_UINT32(125000000u, esy_hal_system_clk_sys_get_hz());
  esy_hal_system_delay_ms(3u);
  esy_hal_system_delay_us(7u);
  TEST_ASSERT_EQUAL_UINT32(123u, esy_hal_system_get_tick());
  TEST_ASSERT_EQUAL_UINT32(456u, esy_hal_system_time_32_us());
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_system_always_on_time_us(&always_on_time_us));
  TEST_ASSERT_EQUAL_UINT64(1000u, always_on_time_us);
  TEST_ASSERT_NOT_EQUAL(0u, esy_hal_system_always_on_state_mask());
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_system_get_tick_partial_us(1000u, &partial_tick_us));
  TEST_ASSERT_EQUAL_UINT32(250u, partial_tick_us);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_system_suspend_tick(1000u, &partial_tick_us));
  esy_hal_system_resume_tick();
  TEST_ASSERT_FALSE(esy_hal_system_tick_is_pending());
  TEST_ASSERT_EQUAL_UINT8(1u, esy_hal_system_get_core_id());
  TEST_ASSERT_EQUAL_UINT8(2u, esy_hal_system_get_core_count());
  TEST_ASSERT_TRUE(esy_hal_system_is_in_isr());
}

void test_esy_hal_system_low_level_primitives_route_to_driver(void) {
  uint32_t saved_status = esy_hal_system_irq_save();
  TEST_ASSERT_EQUAL_UINT32(0x55aau, saved_status);

  esy_hal_system_irq_restore(saved_status);
  esy_hal_system_memory_barrier();
  esy_hal_system_signal_event();
  esy_hal_system_wait_for_event();

  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_irq_save_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_irq_restore_calls);
  TEST_ASSERT_EQUAL_UINT32(0x55aau, platform_test_state()->last_irq_status);
  TEST_ASSERT_EQUAL_UINT32(1u,
                           platform_test_state()->driver_memory_barrier_calls);
  TEST_ASSERT_EQUAL_UINT32(1u,
                           platform_test_state()->driver_signal_event_calls);
  TEST_ASSERT_EQUAL_UINT32(1u,
                           platform_test_state()->driver_wait_for_event_calls);
}

void test_esy_hal_system_reset_routes_to_driver(void) {
  const uint32_t before = platform_test_state()->last_count;

  esy_hal_system_reset();

  TEST_ASSERT_EQUAL_UINT32(before + 1u, platform_test_state()->last_count);
}

void test_esy_hal_system_tick_from_isr_forwards_once(void) {
  esy_hal_system_tick_from_isr();

  TEST_ASSERT_EQUAL_UINT32(1u,
                         platform_test_state()->driver_tick_from_isr_calls);
}

void test_system_random_validates_empty_requests_before_calling_the_provider(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_system_random_bytes(NULL, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_system_random_bytes(NULL, 0u));
  uint8_t byte = 0xA5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_system_random_bytes(&byte, 0u));
  TEST_ASSERT_EQUAL_UINT8(0xA5u, byte);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_random_calls);
}

void test_system_random_forwards_the_complete_buffer_and_preserves_provider_errors(void) {
  uint8_t bytes[19] = {0};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_system_random_bytes(bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL_PTR(bytes, platform_test_state()->driver_random_buffer);
  TEST_ASSERT_EQUAL_UINT32(sizeof(bytes), platform_test_state()->driver_random_length);
  TEST_ASSERT_EQUAL_UINT8(18u, bytes[18]);
  platform_test_set_random_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_system_random_bytes(bytes, sizeof(bytes)));
  platform_test_set_random_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_system_random_bytes(bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL_UINT32(3u, platform_test_state()->driver_random_calls);
}
