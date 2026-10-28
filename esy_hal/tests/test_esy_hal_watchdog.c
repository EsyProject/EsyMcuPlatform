/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_watchdog.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) {
  platform_test_reset();
}

void tearDown(void) {}

void test_esy_hal_watchdog_routes_lifecycle_to_driver(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_watchdog_init(250u, true));
  esy_hal_watchdog_enable();
  esy_hal_watchdog_feed();

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_start_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_write_calls);
  TEST_ASSERT_EQUAL_UINT32(250u, state->last_count);
  TEST_ASSERT_TRUE(state->last_pause_on_debug);
  TEST_ASSERT_TRUE(esy_hal_watchdog_was_reset());

  esy_hal_watchdog_clear_reset();
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_read_calls);
}


void test_watchdog_cleanup_and_deadline_capability_errors_propagate(void) {
  uint64_t timeout = 42u;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_watchdog_deinit());
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_hal_watchdog_get_timeout_lower_bound_us(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
                    esy_hal_watchdog_get_timeout_lower_bound_us(&timeout));
  TEST_ASSERT_EQUAL_UINT64(42u, timeout);
}
