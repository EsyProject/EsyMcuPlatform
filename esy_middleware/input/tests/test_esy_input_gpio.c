/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>
#include "unity.h"
#include "esy_middleware/input/esy_input.h"
#include "esy_middleware/input/esy_input_gpio.h"
#include "fake_esy_hal_gpio.h"

#define GPIO_CAPTURE_CAPACITY 16u
#define BUTTON_PIN 3u
#define PHASE_A_PIN 4u
#define PHASE_B_PIN 5u

typedef struct {
  esy_input_event_t events[GPIO_CAPTURE_CAPACITY];
  size_t count;
} input_gpio_test_ctx_t;

static input_gpio_test_ctx_t s_ctx;

static void capture_gpio_event(const esy_input_event_t *event, void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_LESS_THAN(GPIO_CAPTURE_CAPACITY, s_ctx.count);
  s_ctx.events[s_ctx.count++] = *event;
}

static void init_gpio(esy_gpio_t *gpio, uint8_t pin) {
  esy_gpio_config_t config;
  esy_hal_gpio_default_cfg(&config);
  config.pin = pin;
  config.mode = ESY_GPIO_MODE_INPUT;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(gpio, &config));
}

static void init_button(esy_input_button_t *button, uint32_t debounce_ms) {
  const esy_input_button_config_t config = {.source_id = 9u, .code = 10u,
      .debounce_ms = debounce_ms, .on_event = capture_gpio_event, .event_arg = &s_ctx};
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_init(button, &config));
}

static void init_encoder(esy_input_encoder_t *encoder, uint8_t edges) {
  const esy_input_encoder_config_t config = {.source_id = 11u, .code = 12u,
      .edges_per_step = edges, .on_event = capture_gpio_event, .event_arg = &s_ctx};
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_init(encoder, &config));
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_gpio_reset();
}

void tearDown(void) {
}

void test_gpio_button_rejects_null_pointers_and_uninitialized_gpio(void) {
  esy_input_button_t button = {0};
  esy_gpio_t gpio = {0};
  esy_input_gpio_button_t provider = {.gpio = &gpio};
  init_button(&button, 0u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_button_poll(NULL, &provider, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_button_poll(&button, NULL, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_button_poll(&button, &provider, 0u));
  provider.gpio = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_button_poll(&button, &provider, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.count);
}

void test_gpio_button_maps_active_high_and_active_low_to_the_same_key_events(void) {
  for (uint8_t inverted = 0u; inverted < 2u; ++inverted) {
    esy_input_button_t button = {0};
    esy_gpio_t gpio = {0};
    esy_input_gpio_button_t provider = {.gpio = &gpio, .active_low = inverted != 0u};
    init_gpio(&gpio, BUTTON_PIN);
    init_button(&button, 0u);
    fake_esy_hal_gpio_set_read_value_for_pin(BUTTON_PIN, inverted == 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_button_poll(&button, &provider, 1u));
    fake_esy_hal_gpio_set_read_value_for_pin(BUTTON_PIN, inverted != 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_button_poll(&button, &provider, 2u));
    TEST_ASSERT_EQUAL(ESY_INPUT_EVENT_KEY, s_ctx.events[3u * inverted].type);
    TEST_ASSERT_EQUAL_INT32(1, s_ctx.events[3u * inverted].value);
    TEST_ASSERT_EQUAL_UINT16(9u, s_ctx.events[3u * inverted].source_id);
    TEST_ASSERT_EQUAL_UINT16(10u, s_ctx.events[3u * inverted].code);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.events[3u * inverted].timestamp_ms);
    TEST_ASSERT_EQUAL(ESY_INPUT_EVENT_KEY, s_ctx.events[3u * inverted + 1u].type);
    TEST_ASSERT_EQUAL_INT32(0, s_ctx.events[3u * inverted + 1u].value);
    TEST_ASSERT_EQUAL(ESY_INPUT_EVENT_CLICK, s_ctx.events[3u * inverted + 2u].type);
    TEST_ASSERT_EQUAL(ESY_OK, esy_input_button_deinit(&button));
    TEST_ASSERT_TRUE(esy_hal_gpio_is_initialized(&gpio));
    TEST_ASSERT_FALSE(fake_esy_hal_gpio_has_written(BUTTON_PIN));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&gpio));
  }
  TEST_ASSERT_EQUAL_UINT32(6u, s_ctx.count);
}

void test_gpio_button_read_failure_preserves_candidate_and_timestamp(void) {
  esy_input_button_t button = {0};
  esy_gpio_t gpio = {0};
  esy_input_gpio_button_t provider = {.gpio = &gpio};
  init_gpio(&gpio, BUTTON_PIN);
  init_button(&button, 5u);
  fake_esy_hal_gpio_set_read_value_for_pin(BUTTON_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_button_poll(&button, &provider, 0u));
  esy_input_button_t snapshot = button;
  fake_esy_hal_gpio_set_read_result(ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_input_gpio_button_poll(&button, &provider, 5u));
  TEST_ASSERT_EQUAL_MEMORY(&snapshot, &button, sizeof(button));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.count);
  fake_esy_hal_gpio_set_read_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_button_poll(&button, &provider, 5u));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.count);
  TEST_ASSERT_EQUAL_UINT32(5u, s_ctx.events[0].timestamp_ms);
}

void test_gpio_button_propagates_core_handle_and_time_errors_after_successful_read(void) {
  esy_input_button_t button = {0};
  esy_gpio_t gpio = {0};
  esy_input_gpio_button_t provider = {.gpio = &gpio};
  init_gpio(&gpio, BUTTON_PIN);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_gpio_button_poll(&button, &provider, 0u));
  init_button(&button, 0u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_button_poll(&button, &provider, 100u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_gpio_button_poll(&button, &provider, 99u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.count);
}

void test_gpio_encoder_rejects_null_phase_handles_and_duplicate_phases(void) {
  esy_input_encoder_t encoder = {0};
  esy_gpio_t phase_a = {0};
  esy_gpio_t phase_b = {0};
  esy_input_gpio_encoder_t provider = {.phase_a = &phase_a, .phase_b = &phase_b};
  init_encoder(&encoder, 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_encoder_poll(NULL, &provider, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_encoder_poll(&encoder, NULL, 0u));
  provider.phase_a = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
  provider.phase_a = &phase_a;
  provider.phase_b = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
  provider.phase_b = &phase_a;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.count);
}

void test_gpio_encoder_reads_a_as_high_bit_and_b_as_low_bit(void) {
  esy_input_encoder_t encoder = {0};
  esy_gpio_t phase_a = {0};
  esy_gpio_t phase_b = {0};
  const esy_input_gpio_encoder_t provider = {.phase_a = &phase_a, .phase_b = &phase_b};
  init_gpio(&phase_a, PHASE_A_PIN);
  init_gpio(&phase_b, PHASE_B_PIN);
  init_encoder(&encoder, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_B_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 1u));
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_A_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 2u));
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_B_PIN, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 3u));
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_A_PIN, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 4u));
  TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.count);
  for (size_t i = 0u; i < s_ctx.count; ++i) {
    TEST_ASSERT_EQUAL(ESY_INPUT_EVENT_RELATIVE, s_ctx.events[i].type);
    TEST_ASSERT_EQUAL_INT32(1, s_ctx.events[i].value);
    TEST_ASSERT_EQUAL_UINT16(11u, s_ctx.events[i].source_id);
    TEST_ASSERT_EQUAL_UINT16(12u, s_ctx.events[i].code);
    TEST_ASSERT_EQUAL_UINT32(i + 1u, s_ctx.events[i].timestamp_ms);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_deinit(&encoder));
  TEST_ASSERT_TRUE(esy_hal_gpio_is_initialized(&phase_a));
  TEST_ASSERT_TRUE(esy_hal_gpio_is_initialized(&phase_b));
  TEST_ASSERT_FALSE(fake_esy_hal_gpio_has_written(PHASE_A_PIN));
  TEST_ASSERT_FALSE(fake_esy_hal_gpio_has_written(PHASE_B_PIN));
}

void test_gpio_encoder_supports_all_per_phase_polarity_combinations(void) {
  for (uint8_t inversion = 0u; inversion < 4u; ++inversion) {
    esy_input_encoder_t encoder = {0};
    esy_gpio_t phase_a = {0};
    esy_gpio_t phase_b = {0};
    const esy_input_gpio_encoder_t provider = {.phase_a = &phase_a, .phase_b = &phase_b,
        .inverted_a = (inversion & 2u) != 0u, .inverted_b = (inversion & 1u) != 0u};
    init_gpio(&phase_a, PHASE_A_PIN);
    init_gpio(&phase_b, PHASE_B_PIN);
    init_encoder(&encoder, 1u);
    fake_esy_hal_gpio_set_read_value_for_pin(PHASE_A_PIN, provider.inverted_a);
    fake_esy_hal_gpio_set_read_value_for_pin(PHASE_B_PIN, provider.inverted_b);
    TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
    fake_esy_hal_gpio_set_read_value_for_pin(PHASE_B_PIN, !provider.inverted_b);
    TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 1u));
    TEST_ASSERT_EQUAL_INT32(1, s_ctx.events[inversion].value);
    TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_deinit(&encoder));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&phase_a));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&phase_b));
  }
  TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.count);
}

void test_gpio_encoder_first_read_error_is_propagated_before_second_gpio_error(void) {
  esy_input_encoder_t encoder = {0};
  esy_gpio_t phase_a = {0};
  esy_gpio_t phase_b = {0};
  const esy_input_gpio_encoder_t provider = {.phase_a = &phase_a, .phase_b = &phase_b};
  init_gpio(&phase_a, PHASE_A_PIN);
  init_encoder(&encoder, 1u);
  fake_esy_hal_gpio_set_read_result(ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.count);
  fake_esy_hal_gpio_set_read_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
}

void test_gpio_encoder_second_read_failure_does_not_consume_partial_motion(void) {
  esy_input_encoder_t encoder = {0};
  esy_gpio_t phase_a = {0};
  esy_gpio_t phase_b = {0};
  const esy_input_gpio_encoder_t provider = {.phase_a = &phase_a, .phase_b = &phase_b};
  init_gpio(&phase_a, PHASE_A_PIN);
  init_encoder(&encoder, 2u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_sample(&encoder, 0u, 0u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_encoder_sample(&encoder, 1u, 1u));
  esy_input_encoder_t snapshot = encoder;
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_A_PIN, true);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_input_gpio_encoder_poll(&encoder, &provider, 2u));
  TEST_ASSERT_EQUAL_MEMORY(&snapshot, &encoder, sizeof(encoder));
  init_gpio(&phase_b, PHASE_B_PIN);
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_B_PIN, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 2u));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.count);
  TEST_ASSERT_EQUAL_INT32(1, s_ctx.events[0].value);
}

void test_gpio_encoder_propagates_core_illegal_transition_and_handle_errors(void) {
  esy_input_encoder_t encoder = {0};
  esy_gpio_t phase_a = {0};
  esy_gpio_t phase_b = {0};
  const esy_input_gpio_encoder_t provider = {.phase_a = &phase_a, .phase_b = &phase_b};
  init_gpio(&phase_a, PHASE_A_PIN);
  init_gpio(&phase_b, PHASE_B_PIN);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
  init_encoder(&encoder, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 0u));
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_A_PIN, true);
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_B_PIN, true);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_input_gpio_encoder_poll(&encoder, &provider, 1u));
  fake_esy_hal_gpio_set_read_value_for_pin(PHASE_B_PIN, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_input_gpio_encoder_poll(&encoder, &provider, 2u));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.count);
  TEST_ASSERT_EQUAL_INT32(1, s_ctx.events[0].value);
}
