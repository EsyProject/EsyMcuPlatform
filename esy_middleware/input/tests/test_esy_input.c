/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include "unity.h"
#include "esy_middleware/input/esy_input.h"

#define CAPTURE_CAPACITY 64u

typedef struct {
  esy_input_event_t events[CAPTURE_CAPACITY];
  size_t count;
} input_capture_t;

typedef struct {
  input_capture_t captures[2];
  esy_input_button_t *reenter_button;
  esy_input_encoder_t *reenter_encoder;
  esy_err_t sample_result;
  esy_err_t deinit_result;
  esy_err_t init_result;
} input_test_ctx_t;

static input_test_ctx_t s_ctx;

static void capture_event(const esy_input_event_t *event, void *arg) {
  input_capture_t *capture = arg;
  TEST_ASSERT_NOT_NULL(event);
  TEST_ASSERT_NOT_NULL(capture);
  TEST_ASSERT_LESS_THAN(CAPTURE_CAPACITY, capture->count);
  capture->events[capture->count++] = *event;
  if (s_ctx.reenter_button != NULL) {
    esy_input_button_config_t config = {.on_event = capture_event, .event_arg = arg};
    s_ctx.sample_result = esy_input_button_sample(s_ctx.reenter_button, false,
                                                event->timestamp_ms);
    s_ctx.deinit_result = esy_input_button_deinit(s_ctx.reenter_button);
    s_ctx.init_result = esy_input_button_init(s_ctx.reenter_button, &config);
  }
  if (s_ctx.reenter_encoder != NULL) {
    esy_input_encoder_config_t config = {.edges_per_step = 1u,
                                        .on_event = capture_event, .event_arg = arg};
    s_ctx.sample_result = esy_input_encoder_sample(s_ctx.reenter_encoder, 0u,
                                                 event->timestamp_ms);
    s_ctx.deinit_result = esy_input_encoder_deinit(s_ctx.reenter_encoder);
    s_ctx.init_result = esy_input_encoder_init(s_ctx.reenter_encoder, &config);
  }
}

static esy_input_button_config_t button_config(void) {
  return (esy_input_button_config_t){.source_id = 17u, .code = 42u,
      .on_event = capture_event, .event_arg = &s_ctx.captures[0]};
}

static esy_input_encoder_config_t encoder_config(uint8_t edges) {
  return (esy_input_encoder_config_t){.source_id = 23u, .code = 55u,
      .edges_per_step = edges, .on_event = capture_event,
      .event_arg = &s_ctx.captures[0]};
}

static void expect_event(size_t index, esy_input_event_type_t type, int32_t value,
                         uint32_t timestamp_ms) {
  TEST_ASSERT_LESS_THAN(s_ctx.captures[0].count, index);
  const esy_input_event_t *event = &s_ctx.captures[0].events[index];
  TEST_ASSERT_EQUAL_INT(type, event->type);
  TEST_ASSERT_EQUAL_INT32(value, event->value);
  TEST_ASSERT_EQUAL_UINT32(timestamp_ms, event->timestamp_ms);
}

static void button_sample(esy_input_button_t *button, bool pressed, uint32_t now) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_sample(button, pressed, now));
}

static void encoder_sample(esy_input_encoder_t *encoder, uint8_t ab, uint32_t now) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_sample(encoder, ab, now));
}

static void advance_full_cycle(esy_input_button_t *button, bool pressed,
                               uint32_t origin) {
  button_sample(button, pressed, origin + (uint32_t)INT32_MAX);
  button_sample(button, pressed, origin + 2u * (uint32_t)INT32_MAX);
  button_sample(button, pressed, origin);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
}

void tearDown(void) {
}

void test_button_validates_arguments_and_init_failure_preserves_zero_storage(void) {
  esy_input_button_t button = {0};
  esy_input_button_t zero = {0};
  esy_input_button_config_t config = button_config();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_button_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_button_init(&button, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_button_sample(NULL, false, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_button_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_button_sample(&button, false, 0u));
  config.on_event = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_button_init(&button, &config));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &button, sizeof(button));
  config = button_config();
  config.debounce_ms = (uint32_t)INT32_MAX + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_button_init(&button, &config));
  config = button_config();
  config.long_press_ms = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_button_init(&button, &config));
  config = button_config();
  config.double_click_ms = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_button_init(&button, &config));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &button, sizeof(button));
  config = button_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
}

void test_button_lifecycle_repeated_init_copied_handle_and_reinit(void) {
  esy_input_button_t button = {0};
  esy_input_button_t zero = {0};
  esy_input_button_config_t config = button_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_deinit(&button));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_button_init(&button, &config));
  esy_input_button_t copy = button;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_button_sample(&copy, true, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_button_deinit(&copy));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_button_init(&copy, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_deinit(&button));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &button, sizeof(button));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_deinit(&button));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_button_sample(&button, true, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
}

void test_button_configuration_is_copied_and_event_identifiers_are_preserved(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  config.source_id = 99u;
  config.code = 100u;
  config.on_event = NULL;
  button_sample(&button, true, 123u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.captures[0].count);
  expect_event(0u, ESY_INPUT_EVENT_KEY, 1, 123u);
  TEST_ASSERT_EQUAL_UINT16(17u, s_ctx.captures[0].events[0].source_id);
  TEST_ASSERT_EQUAL_UINT16(42u, s_ctx.captures[0].events[0].code);
}

void test_button_debounces_both_edges_and_restarts_after_contact_bounce(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.debounce_ms = 5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, false, 0u);
  button_sample(&button, true, 1u);
  button_sample(&button, false, 4u);
  button_sample(&button, true, 5u);
  button_sample(&button, true, 9u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  button_sample(&button, true, 10u);
  button_sample(&button, false, 11u);
  button_sample(&button, true, 14u);
  button_sample(&button, false, 15u);
  button_sample(&button, false, 19u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.captures[0].count);
  button_sample(&button, false, 20u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
  expect_event(0u, ESY_INPUT_EVENT_KEY, 1, 10u);
  expect_event(1u, ESY_INPUT_EVENT_KEY, 0, 20u);
  expect_event(2u, ESY_INPUT_EVENT_CLICK, 1, 20u);
}

void test_button_held_on_first_sample_waits_for_debounce_before_hold_timer(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.debounce_ms = 5u;
  config.long_press_ms = 10u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 100u);
  button_sample(&button, true, 104u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  button_sample(&button, true, 105u);
  button_sample(&button, true, 114u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.captures[0].count);
  button_sample(&button, true, 115u);
  expect_event(1u, ESY_INPUT_EVENT_LONG_PRESS, 1, 115u);
}

void test_button_disabling_gestures_emits_immediate_single_click_after_key_release(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, false, 0u);
  button_sample(&button, true, 1u);
  button_sample(&button, true, 500u);
  button_sample(&button, false, 501u);
  button_sample(&button, false, 502u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
  expect_event(0u, ESY_INPUT_EVENT_KEY, 1, 1u);
  expect_event(1u, ESY_INPUT_EVENT_KEY, 0, 501u);
  expect_event(2u, ESY_INPUT_EVENT_CLICK, 1, 501u);
}

void test_button_single_click_is_deferred_until_double_click_window_expires(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.double_click_ms = 100u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 10u);
  button_sample(&button, false, 109u);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.captures[0].count);
  button_sample(&button, false, 110u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
  expect_event(2u, ESY_INPUT_EVENT_CLICK, 1, 110u);
  button_sample(&button, false, 111u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
}

void test_button_double_click_uses_short_release_to_release_window(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.double_click_ms = 100u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 10u);
  button_sample(&button, true, 50u);
  button_sample(&button, false, 109u);
  TEST_ASSERT_EQUAL_UINT32(5u, s_ctx.captures[0].count);
  expect_event(3u, ESY_INPUT_EVENT_KEY, 0, 109u);
  expect_event(4u, ESY_INPUT_EVENT_DOUBLE_CLICK, 2, 109u);
  button_sample(&button, false, 500u);
  TEST_ASSERT_EQUAL_UINT32(5u, s_ctx.captures[0].count);
}

void test_button_second_release_at_exact_window_becomes_two_single_clicks(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.double_click_ms = 100u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 10u);
  button_sample(&button, true, 50u);
  button_sample(&button, false, 110u);
  TEST_ASSERT_EQUAL_UINT32(5u, s_ctx.captures[0].count);
  expect_event(3u, ESY_INPUT_EVENT_CLICK, 1, 110u);
  expect_event(4u, ESY_INPUT_EVENT_KEY, 0, 110u);
  button_sample(&button, false, 209u);
  TEST_ASSERT_EQUAL_UINT32(5u, s_ctx.captures[0].count);
  button_sample(&button, false, 210u);
  expect_event(5u, ESY_INPUT_EVENT_CLICK, 1, 210u);
}

void test_button_long_press_emits_once_and_suppresses_click_after_release(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.long_press_ms = 50u;
  config.double_click_ms = 100u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 10u);
  button_sample(&button, true, 59u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.captures[0].count);
  button_sample(&button, true, 60u);
  button_sample(&button, true, 100u);
  button_sample(&button, false, 110u);
  button_sample(&button, false, 500u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
  expect_event(1u, ESY_INPUT_EVENT_LONG_PRESS, 1, 60u);
  expect_event(2u, ESY_INPUT_EVENT_KEY, 0, 110u);
  button_sample(&button, true, 501u);
  button_sample(&button, false, 502u);
  button_sample(&button, false, 602u);
  expect_event(5u, ESY_INPUT_EVENT_CLICK, 1, 602u);
}

void test_button_release_at_exact_long_threshold_is_long_and_precedes_key_release(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.long_press_ms = 50u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 10u);
  button_sample(&button, false, 60u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
  expect_event(1u, ESY_INPUT_EVENT_LONG_PRESS, 1, 60u);
  expect_event(2u, ESY_INPUT_EVENT_KEY, 0, 60u);
}

void test_button_long_second_press_does_not_turn_first_short_click_into_double(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.long_press_ms = 50u;
  config.double_click_ms = 100u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 1u);
  button_sample(&button, true, 2u);
  button_sample(&button, false, 52u);
  button_sample(&button, false, 101u);
  TEST_ASSERT_EQUAL_UINT32(6u, s_ctx.captures[0].count);
  expect_event(3u, ESY_INPUT_EVENT_LONG_PRESS, 1, 52u);
  expect_event(4u, ESY_INPUT_EVENT_KEY, 0, 52u);
  expect_event(5u, ESY_INPUT_EVENT_CLICK, 1, 101u);
}

void test_button_debounce_and_hold_boundaries_cross_timestamp_wrap(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.debounce_ms = 5u;
  config.long_press_ms = 5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, UINT32_MAX - 6u);
  button_sample(&button, true, UINT32_MAX - 2u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  button_sample(&button, true, UINT32_MAX - 1u);
  button_sample(&button, true, 2u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.captures[0].count);
  button_sample(&button, true, 3u);
  expect_event(1u, ESY_INPUT_EVENT_LONG_PRESS, 1, 3u);
  button_sample(&button, false, 4u);
  button_sample(&button, false, 9u);
  expect_event(2u, ESY_INPUT_EVENT_KEY, 0, 9u);
}

void test_button_double_click_and_single_expiry_cross_timestamp_wrap(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.double_click_ms = 10u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, UINT32_MAX - 5u);
  button_sample(&button, false, UINT32_MAX - 4u);
  button_sample(&button, true, 0u);
  button_sample(&button, false, 4u);
  expect_event(4u, ESY_INPUT_EVENT_DOUBLE_CLICK, 2, 4u);
  button_sample(&button, true, 5u);
  button_sample(&button, false, 6u);
  button_sample(&button, false, 15u);
  TEST_ASSERT_EQUAL_UINT32(7u, s_ctx.captures[0].count);
  button_sample(&button, false, 16u);
  expect_event(7u, ESY_INPUT_EVENT_CLICK, 1, 16u);
}

void test_button_completed_long_timer_cannot_reactivate_after_full_clock_cycle(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.long_press_ms = 5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, true, 5u);
  advance_full_cycle(&button, true, 5u);
  button_sample(&button, true, 6u);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.captures[0].count);
  button_sample(&button, false, 7u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
}

void test_button_completed_click_timer_cannot_reactivate_after_full_clock_cycle(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.double_click_ms = 5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 1u);
  button_sample(&button, false, 6u);
  advance_full_cycle(&button, false, 6u);
  button_sample(&button, false, 7u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
  button_sample(&button, true, 8u);
  button_sample(&button, false, 9u);
  button_sample(&button, false, 14u);
  expect_event(5u, ESY_INPUT_EVENT_CLICK, 1, 14u);
}

void test_button_cancelled_debounce_cannot_reactivate_after_full_clock_cycle(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.debounce_ms = 5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 1u);
  advance_full_cycle(&button, false, 1u);
  button_sample(&button, false, 5u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  button_sample(&button, true, 6u);
  button_sample(&button, true, 11u);
  expect_event(0u, ESY_INPUT_EVENT_KEY, 1, 11u);
}

void test_button_cancelled_hold_and_double_click_timers_stay_inactive_after_full_cycle(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.long_press_ms = 50u;
  config.double_click_ms = 100u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 1u);
  button_sample(&button, true, 2u);
  button_sample(&button, false, 3u);
  advance_full_cycle(&button, false, 3u);
  button_sample(&button, false, 52u);
  button_sample(&button, false, 103u);
  TEST_ASSERT_EQUAL_UINT32(5u, s_ctx.captures[0].count);
  expect_event(4u, ESY_INPUT_EVENT_DOUBLE_CLICK, 2, 3u);
}

void test_button_deinit_cancels_pending_gestures_and_reinit_starts_released(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.long_press_ms = 50u;
  config.double_click_ms = 100u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 1u);
  button_sample(&button, true, 2u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_deinit(&button));
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, false, 1000u);
  advance_full_cycle(&button, false, 1000u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
}

void test_button_supports_maximum_duration_and_maximum_sample_gap(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.debounce_ms = INT32_MAX;
  config.long_press_ms = INT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, true, (uint32_t)INT32_MAX - 1u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  button_sample(&button, true, INT32_MAX);
  button_sample(&button, true, UINT32_MAX - 2u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.captures[0].count);
  button_sample(&button, true, UINT32_MAX - 1u);
  expect_event(1u, ESY_INPUT_EVENT_LONG_PRESS, 1, UINT32_MAX - 1u);
  button_sample(&button, true, (uint32_t)INT32_MAX - 2u);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.captures[0].count);
}

void test_button_maximum_double_click_duration_expires_at_boundary(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.double_click_ms = INT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 1u);
  button_sample(&button, false, INT32_MAX);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.captures[0].count);
  button_sample(&button, false, (uint32_t)INT32_MAX + 1u);
  expect_event(2u, ESY_INPUT_EVENT_CLICK, 1, (uint32_t)INT32_MAX + 1u);
}

void test_button_maximum_hold_expires_after_maximum_sample_gap_overshoots_threshold(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.long_press_ms = INT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, true, (uint32_t)INT32_MAX - 1u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.captures[0].count);
  button_sample(&button, true, UINT32_MAX - 2u);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.captures[0].count);
  expect_event(1u, ESY_INPUT_EVENT_LONG_PRESS, 1, UINT32_MAX - 2u);
}

void test_button_maximum_click_window_expires_after_maximum_sample_gap_overshoot(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.double_click_ms = INT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 0u);
  button_sample(&button, false, 1u);
  button_sample(&button, false, INT32_MAX);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.captures[0].count);
  button_sample(&button, false, UINT32_MAX - 1u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
  expect_event(2u, ESY_INPUT_EVENT_CLICK, 1, UINT32_MAX - 1u);
}

void test_button_rejects_backwards_and_oversized_gaps_without_consuming_state(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  config.debounce_ms = 5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  button_sample(&button, true, 100u);
  esy_input_button_t snapshot = button;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_button_sample(&button, false, 99u));
  TEST_ASSERT_EQUAL_MEMORY(&snapshot, &button, sizeof(button));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_button_sample(
      &button, false, 100u + (uint32_t)INT32_MAX + 1u));
  TEST_ASSERT_EQUAL_MEMORY(&snapshot, &button, sizeof(button));
  button_sample(&button, true, 105u);
  expect_event(0u, ESY_INPUT_EVENT_KEY, 1, 105u);
}

void test_button_callback_reentry_returns_busy_and_preserves_outer_sample(void) {
  esy_input_button_t button = {0};
  esy_input_button_config_t config = button_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&button, &config));
  s_ctx.reenter_button = &button;
  button_sample(&button, true, 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.sample_result);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.deinit_result);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.init_result);
  s_ctx.reenter_button = NULL;
  button_sample(&button, false, 2u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[0].count);
}

void test_button_instances_have_independent_levels_timers_and_sinks(void) {
  esy_input_button_t first = {0};
  esy_input_button_t second = {0};
  esy_input_button_config_t config = button_config();
  config.long_press_ms = 5u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&first, &config));
  config.source_id = 18u;
  config.event_arg = &s_ctx.captures[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(&second, &config));
  button_sample(&first, true, 1u);
  button_sample(&second, true, 100u);
  button_sample(&first, true, 6u);
  button_sample(&second, false, 101u);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.captures[0].count);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.captures[1].count);
  TEST_ASSERT_EQUAL_UINT16(18u, s_ctx.captures[1].events[2].source_id);
  TEST_ASSERT_EQUAL(ESY_INPUT_EVENT_CLICK, s_ctx.captures[1].events[2].type);
}

void test_encoder_validates_arguments_configuration_and_initial_storage(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_t zero = {0};
  esy_input_encoder_config_t config = encoder_config(1u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_init(&encoder, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_sample(NULL, 0u, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_encoder_sample(&encoder, 0u, 0u));
  config.on_event = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_init(&encoder, &config));
  config = encoder_config(0u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_init(&encoder, &config));
  config.edges_per_step = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_init(&encoder, &config));
  config.edges_per_step = 255u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_init(&encoder, &config));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &encoder, sizeof(encoder));
}

void test_encoder_lifecycle_repeated_init_copied_handle_deinit_and_reinit(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_t zero = {0};
  esy_input_encoder_config_t config = encoder_config(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_deinit(&encoder));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_encoder_init(&encoder, &config));
  esy_input_encoder_t copy = encoder;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_encoder_sample(&copy, 0u, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_encoder_deinit(&copy));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_encoder_init(&copy, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_deinit(&encoder));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &encoder, sizeof(encoder));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_deinit(&encoder));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
}

void test_encoder_first_sample_seeds_each_ab_state_without_movement(void) {
  for (uint8_t ab = 0u; ab < 4u; ++ab) {
    esy_input_encoder_t encoder = {0};
    esy_input_encoder_config_t config = encoder_config(1u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
    encoder_sample(&encoder, ab, 0u);
    encoder_sample(&encoder, ab, 1u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_deinit(&encoder));
  }
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
}

void test_encoder_decodes_positive_and_negative_gray_cycles_at_one_two_and_four_edges(void) {
  static const uint8_t k_positive[] = {0u, 1u, 3u, 2u, 0u};
  static const uint8_t k_negative[] = {0u, 2u, 3u, 1u, 0u};
  for (uint8_t edges = 1u; edges <= 4u; edges *= 2u) {
    for (uint8_t direction = 0u; direction < 2u; ++direction) {
      s_ctx.captures[0].count = 0u;
      esy_input_encoder_t encoder = {0};
      esy_input_encoder_config_t config = encoder_config(edges);
      TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
      for (uint8_t i = 0u; i < 5u; ++i) {
        encoder_sample(&encoder, direction == 0u ? k_positive[i] : k_negative[i], i);
      }
      TEST_ASSERT_EQUAL_UINT32(4u / edges, s_ctx.captures[0].count);
      for (size_t i = 0u; i < s_ctx.captures[0].count; ++i) {
        expect_event(i, ESY_INPUT_EVENT_RELATIVE, direction == 0u ? 1 : -1,
                     (uint32_t)(i + 1u) * edges);
        TEST_ASSERT_EQUAL_UINT16(23u, s_ctx.captures[0].events[i].source_id);
        TEST_ASSERT_EQUAL_UINT16(55u, s_ctx.captures[0].events[i].code);
      }
    }
  }
}

void test_encoder_configuration_is_copied_and_inversion_reverses_output(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_config_t config = encoder_config(1u);
  config.inverted = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  config.inverted = false;
  config.on_event = NULL;
  encoder_sample(&encoder, 0u, 0u);
  encoder_sample(&encoder, 1u, 1u);
  encoder_sample(&encoder, 0u, 2u);
  expect_event(0u, ESY_INPUT_EVENT_RELATIVE, -1, 1u);
  expect_event(1u, ESY_INPUT_EVENT_RELATIVE, 1, 2u);
}

void test_encoder_duplicate_samples_and_adjacent_reversal_cancel_partial_movement(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_config_t config = encoder_config(4u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  encoder_sample(&encoder, 0u, 0u);
  encoder_sample(&encoder, 1u, 1u);
  encoder_sample(&encoder, 1u, 2u);
  encoder_sample(&encoder, 0u, 3u);
  encoder_sample(&encoder, 1u, 4u);
  encoder_sample(&encoder, 3u, 5u);
  encoder_sample(&encoder, 2u, 6u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  encoder_sample(&encoder, 0u, 7u);
  expect_event(0u, ESY_INPUT_EVENT_RELATIVE, 1, 7u);
}

void test_encoder_illegal_two_bit_transition_resynchronizes_and_discards_partial_movement(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_config_t config = encoder_config(4u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  encoder_sample(&encoder, 0u, 0u);
  encoder_sample(&encoder, 1u, 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_input_encoder_sample(&encoder, 2u, 2u));
  encoder_sample(&encoder, 0u, 3u);
  encoder_sample(&encoder, 1u, 4u);
  encoder_sample(&encoder, 3u, 5u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  encoder_sample(&encoder, 2u, 6u);
  expect_event(0u, ESY_INPUT_EVENT_RELATIVE, 1, 6u);
}

void test_encoder_rejects_ab_range_and_time_order_without_consuming_partial_movement(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_config_t config = encoder_config(2u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  encoder_sample(&encoder, 0u, 100u);
  encoder_sample(&encoder, 1u, 101u);
  esy_input_encoder_t snapshot = encoder;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_encoder_sample(&encoder, 4u, 102u));
  TEST_ASSERT_EQUAL_MEMORY(&snapshot, &encoder, sizeof(encoder));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_encoder_sample(&encoder, 3u, 100u));
  TEST_ASSERT_EQUAL_MEMORY(&snapshot, &encoder, sizeof(encoder));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_encoder_sample(
      &encoder, 3u, 101u + (uint32_t)INT32_MAX + 1u));
  TEST_ASSERT_EQUAL_MEMORY(&snapshot, &encoder, sizeof(encoder));
  encoder_sample(&encoder, 3u, 102u);
  expect_event(0u, ESY_INPUT_EVENT_RELATIVE, 1, 102u);
}

void test_encoder_clock_wrap_maximum_sample_gap_and_equal_timestamp_are_accepted(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_config_t config = encoder_config(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  encoder_sample(&encoder, 0u, UINT32_MAX);
  encoder_sample(&encoder, 1u, 0u);
  encoder_sample(&encoder, 3u, 0u);
  encoder_sample(&encoder, 2u, INT32_MAX);
  encoder_sample(&encoder, 0u, UINT32_MAX - 1u);
  TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.captures[0].count);
  expect_event(0u, ESY_INPUT_EVENT_RELATIVE, 1, 0u);
  expect_event(3u, ESY_INPUT_EVENT_RELATIVE, 1, UINT32_MAX - 1u);
}

void test_encoder_deinit_discards_partial_movement_and_reinit_seeds_new_state(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_config_t config = encoder_config(2u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  encoder_sample(&encoder, 0u, 0u);
  encoder_sample(&encoder, 1u, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_deinit(&encoder));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  encoder_sample(&encoder, 3u, 0u);
  encoder_sample(&encoder, 2u, 1u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  encoder_sample(&encoder, 0u, 2u);
  expect_event(0u, ESY_INPUT_EVENT_RELATIVE, 1, 2u);
}

void test_encoder_callback_reentry_returns_busy_and_outer_decoder_remains_usable(void) {
  esy_input_encoder_t encoder = {0};
  esy_input_encoder_config_t config = encoder_config(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&encoder, &config));
  encoder_sample(&encoder, 0u, 0u);
  s_ctx.reenter_encoder = &encoder;
  encoder_sample(&encoder, 1u, 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.sample_result);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.deinit_result);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.init_result);
  s_ctx.reenter_encoder = NULL;
  encoder_sample(&encoder, 3u, 2u);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.captures[0].count);
}

void test_encoder_instances_have_independent_partial_motion_and_sinks(void) {
  esy_input_encoder_t first = {0};
  esy_input_encoder_t second = {0};
  esy_input_encoder_config_t config = encoder_config(2u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&first, &config));
  config.event_arg = &s_ctx.captures[1];
  config.inverted = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(&second, &config));
  encoder_sample(&first, 0u, 0u);
  encoder_sample(&second, 0u, 100u);
  encoder_sample(&first, 1u, 1u);
  encoder_sample(&second, 1u, 101u);
  encoder_sample(&second, 3u, 102u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.captures[0].count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.captures[1].count);
  TEST_ASSERT_EQUAL_INT32(-1, s_ctx.captures[1].events[0].value);
  encoder_sample(&first, 3u, 2u);
  expect_event(0u, ESY_INPUT_EVENT_RELATIVE, 1, 2u);
}
