/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_timer.h"
#include "esy_hal/esy_hal_system.h"

#include "hal_test_fakes.h"
#include "unity.h"

#define TEST_TIMER_IRQ_PRIORITY_MS 3u

void setUp(void) {
  platform_test_reset();
}

void tearDown(void) {}

static void timer_callback(void *arg) {
  (void)arg;
}

void test_esy_hal_timer_start_validates_and_fills_inner_metadata(void) {
  esy_timer_task_config_t cfg = {
      .interval = 10,
      .callback = timer_callback,
      .callback_arg = NULL,
      .repeat = true,
      .mode = ESY_TIMER_MODE_MS,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_timer_init());

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_timer_start(NULL));
  cfg.callback = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_timer_start(&cfg));

  cfg.callback = timer_callback;
  TEST_ASSERT_EQUAL_INT32(2, esy_hal_timer_start(&cfg));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_start_calls);
  TEST_ASSERT_EQUAL_UINT32(10u, state->last_count);
  TEST_ASSERT_EQUAL_UINT8(TEST_TIMER_IRQ_PRIORITY_MS, state->last_irq_priority);
  TEST_ASSERT_EQUAL_INT(ESY_TIMER_SOURCE_DEFAULT, state->last_timer_source);
  esy_hal_timer_deinit();
}

void test_esy_hal_timer_cancel_checks_bounds_and_active_state(void) {
  esy_hal_timer_cancel(-1);
  esy_hal_timer_cancel(5);
  esy_hal_timer_cancel(1);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_write_calls);

  esy_hal_timer_cancel(2);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_EQUAL_UINT32(2u, platform_test_state()->last_count);
}

void test_esy_hal_timer_is_active_checks_bounds(void) {
  TEST_ASSERT_FALSE(esy_hal_timer_is_active(-1));
  TEST_ASSERT_FALSE(esy_hal_timer_is_active(5));
  TEST_ASSERT_FALSE(esy_hal_timer_is_active(1));
  TEST_ASSERT_TRUE(esy_hal_timer_is_active(2));
}

void test_esy_hal_timer_default_lifecycle_and_irq_helpers_route_to_driver(void) {
  esy_timer_task_config_t cfg = {0};

  esy_hal_timer_default_task_cfg(NULL);
  esy_hal_timer_default_task_cfg(&cfg);
  TEST_ASSERT_EQUAL_INT32(1000, cfg.interval);
  TEST_ASSERT_NULL(cfg.callback);
  TEST_ASSERT_NULL(cfg.callback_arg);
  TEST_ASSERT_TRUE(cfg.repeat);
  TEST_ASSERT_EQUAL_INT(ESY_TIMER_MODE_MS, cfg.mode);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_timer_init());
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_init_calls);

  esy_hal_timer_handle_ms_irq();
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->last_dreq);
  esy_hal_timer_handle_us_irq();
  TEST_ASSERT_EQUAL_UINT32(2u, platform_test_state()->last_dreq);

  esy_hal_timer_deinit();
  TEST_ASSERT_NOT_EQUAL(0u, platform_test_state()->driver_deinit_order);
}

void test_esy_hal_timer_reports_nearest_active_deadline(void) {
  esy_timer_task_config_t cfg = {
      .interval = 250,
      .callback = timer_callback,
      .repeat = false,
      .mode = ESY_TIMER_MODE_US,
  };
  uint32_t interval_us = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_timer_init());
  TEST_ASSERT_EQUAL_INT32(2, esy_hal_timer_start(&cfg));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_timer_get_next_wake_interval_us(&interval_us));
  TEST_ASSERT_EQUAL_UINT32(250u, interval_us);
  esy_hal_timer_cancel(2);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_hal_timer_get_next_wake_interval_us(&interval_us));
  esy_hal_timer_deinit();
}

static void start_nested_timer(void *arg) {
  esy_timer_task_config_t nested = {.interval = 100, .callback = timer_callback,
      .repeat = true, .mode = ESY_TIMER_MODE_US};
  *(int32_t *)arg = esy_hal_timer_start(&nested);
}

void test_timer_immediate_callback_does_not_overwrite_nested_timer_id(void) {
  int32_t nested_id = -1;
  uint32_t deadline;
  esy_timer_task_config_t config = {.interval = 1, .callback = start_nested_timer,
      .callback_arg = &nested_id, .repeat = false, .mode = ESY_TIMER_MODE_US};
  platform_test_set_timer_immediate(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_init());
  TEST_ASSERT_EQUAL(1, esy_hal_timer_start(&config));
  TEST_ASSERT_EQUAL(2, nested_id);
  esy_hal_timer_cancel(nested_id);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_timer_get_next_wake_interval_us(&deadline));
  esy_hal_timer_deinit();
}

typedef struct {
  int32_t peer_id;
  uint32_t deliveries;
} timer_cancel_peer_t;

static void cancel_peer_timer(void *arg) {
  timer_cancel_peer_t *state = arg;
  ++state->deliveries;
  esy_hal_timer_cancel(state->peer_id);
}

void test_periodic_start_callback_cancels_peer_without_losing_new_timer(void) {
  timer_cancel_peer_t state = {.peer_id = 0};
  uint32_t deadline = 0u;
  esy_timer_task_config_t config = {.interval = 1000,
      .callback = timer_callback, .repeat = true, .mode = ESY_TIMER_MODE_US};
  platform_test_set_time_us(1000u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_init());
  /* HAL slots and native IDs can diverge after callback-created timers. */
  platform_test_set_timer_start_behavior(1, false);
  TEST_ASSERT_EQUAL(1, esy_hal_timer_start(&config));
  platform_test_set_timer_start_behavior(0, false);
  TEST_ASSERT_EQUAL(0, esy_hal_timer_start(&config));
  esy_hal_timer_cancel(1);

  config.interval = 100;
  config.callback = cancel_peer_timer;
  config.callback_arg = &state;
  platform_test_set_timer_start_behavior(1, true);
  TEST_ASSERT_EQUAL(1, esy_hal_timer_start(&config));
  TEST_ASSERT_FALSE(esy_hal_timer_is_active(0));
  TEST_ASSERT_TRUE(esy_hal_timer_is_active(1));
  TEST_ASSERT_EQUAL_UINT32(1u, state.deliveries);
  platform_test_set_time_us(1200u);
  platform_test_fire_timer();
  TEST_ASSERT_EQUAL_UINT32(2u, state.deliveries);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_get_next_wake_interval_us(&deadline));
  TEST_ASSERT_EQUAL_UINT32(100u, deadline);
  esy_hal_timer_cancel(1);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_timer_get_next_wake_interval_us(&deadline));
  esy_hal_timer_deinit();
}

void test_timer_delayed_periodic_callback_skips_missed_deadlines(void) {
  uint32_t deadline;
  esy_timer_task_config_t config = {.interval = 100, .callback = timer_callback,
      .repeat = true, .mode = ESY_TIMER_MODE_US};
  platform_test_set_time_us(1000u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_init());
  TEST_ASSERT_EQUAL(2, esy_hal_timer_start(&config));
  platform_test_set_time_us(1450u);
  platform_test_fire_timer();
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_get_next_wake_interval_us(&deadline));
  TEST_ASSERT_EQUAL_UINT32(50u, deadline);
  platform_test_set_time_us(1500u);
  platform_test_fire_timer();
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_get_next_wake_interval_us(&deadline));
  TEST_ASSERT_EQUAL_UINT32(100u, deadline);
  esy_hal_timer_deinit();
}

void test_timer_periodic_deadline_skip_preserves_counter_wrap(void) {
  uint32_t deadline;
  esy_timer_task_config_t config = {.interval = 100, .callback = timer_callback,
      .repeat = true, .mode = ESY_TIMER_MODE_US};
  platform_test_set_time_us(UINT32_MAX - 149u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_init());
  TEST_ASSERT_EQUAL(2, esy_hal_timer_start(&config));
  platform_test_set_time_us(75u);
  platform_test_fire_timer();
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_get_next_wake_interval_us(&deadline));
  TEST_ASSERT_EQUAL_UINT32(75u, deadline);
  esy_hal_timer_deinit();
}

static void timer_callback_after_irq_restore(void *arg) {
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(state->driver_irq_save_calls,
                           state->driver_irq_restore_calls);
  ++*(uint32_t *)arg;
}

void test_timer_callback_runs_after_deadline_update_and_irq_restore(void) {
  uint32_t calls = 0u;
  uint32_t deadline;
  esy_timer_task_config_t config = {.interval = 100,
      .callback = timer_callback_after_irq_restore, .callback_arg = &calls,
      .repeat = false, .mode = ESY_TIMER_MODE_US};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_init());
  TEST_ASSERT_EQUAL(2, esy_hal_timer_start(&config));
  platform_test_fire_timer();
  TEST_ASSERT_EQUAL_UINT32(1u, calls);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_hal_timer_get_next_wake_interval_us(&deadline));
  esy_hal_timer_deinit();
}
