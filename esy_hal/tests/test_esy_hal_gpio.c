/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_gpio_pm.h"

#include <string.h>

#include "hal_test_fakes.h"
#include "esy_hal_gpio_internal.h"
#include "unity.h"

static esy_gpio_t s_gpio;

static esy_gpio_config_t output_config(void) {
  esy_gpio_config_t config;

  esy_hal_gpio_default_cfg(&config);
  config.pin = TEST_GPIO_PIN;
  config.mode = ESY_GPIO_MODE_OUTPUT;
  config.output_value = true;
  return config;
}

void setUp(void) {
  platform_test_reset();
  memset(&s_gpio, 0, sizeof(s_gpio));
}

void tearDown(void) {}

static void gpio_irq_handler(uint8_t pin, void *arg) {
  (void)pin;
  (void)arg;
}

static void count_gpio_irq(uint8_t pin, void *arg) {
  TEST_ASSERT_EQUAL_UINT8(TEST_GPIO_PIN, pin);
  ++*(uint32_t *)arg;
}

void test_gpio_queries_preserve_original_configuration_and_inactive_outputs(void) {
  esy_gpio_config_t config = output_config();
  esy_gpio_config_t result = {.pin = ESY_PIN_UNUSED,
                              .mode = ESY_GPIO_MODE_INPUT_PULLUP,
                              .output_value = false};
  esy_gpio_config_t original_result = result;
  const esy_gpio_state_t temporary = {.mode = ESY_GPIO_MODE_INPUT_PULLDOWN};

  TEST_ASSERT_FALSE(esy_hal_gpio_is_initialized(NULL));
  TEST_ASSERT_FALSE(esy_hal_gpio_is_initialized(&s_gpio));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_gpio_get_config(NULL, &result));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_gpio_get_config(&s_gpio, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_gpio_get_config(&s_gpio, &result));
  TEST_ASSERT_EQUAL_MEMORY(&original_result, &result, sizeof(result));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  TEST_ASSERT_TRUE(esy_hal_gpio_is_initialized(&s_gpio));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_apply_state(&s_gpio, &temporary));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_get_config(&s_gpio, &result));
  TEST_ASSERT_EQUAL_UINT8(config.pin, result.pin);
  TEST_ASSERT_EQUAL(config.mode, result.mode);
  TEST_ASSERT_EQUAL(config.output_value, result.output_value);
  TEST_ASSERT_EQUAL(ESY_GPIO_MODE_INPUT_PULLDOWN,
                    esy_hal_gpio_runtime(&s_gpio)->current_state.mode);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_gpio));
  TEST_ASSERT_FALSE(esy_hal_gpio_is_initialized(&s_gpio));
  original_result = result;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_gpio_get_config(&s_gpio, &result));
  TEST_ASSERT_EQUAL_MEMORY(&original_result, &result, sizeof(result));
}

void test_irq_state_change_rejection_preserves_callback_and_allows_reinit(void) {
  uint32_t deliveries = 0u;
  esy_gpio_config_t config = {.pin = TEST_GPIO_PIN, .mode = ESY_GPIO_MODE_INPUT};
  esy_gpio_state_t temporary = {.mode = ESY_GPIO_MODE_INPUT_PULLUP};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_attach_irq(&s_gpio,
      ESY_GPIO_IRQ_RISING, ESY_GPIO_PULL_NONE, count_gpio_irq, &deliveries));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_gpio_apply_state(&s_gpio, &temporary));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_gpio_apply_configured_state(&s_gpio));
  TEST_ASSERT_EQUAL(ESY_GPIO_MODE_INPUT, esy_hal_gpio_runtime(&s_gpio)->current_state.mode);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_init_calls);
  platform_test_fire_gpio_irq();
  TEST_ASSERT_EQUAL_UINT32(1u, deliveries);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_gpio));
  platform_test_fire_gpio_irq();
  TEST_ASSERT_EQUAL_UINT32(1u, deliveries);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_apply_state(&s_gpio, &temporary));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_gpio));
}

void test_gpio_handle_initializes_reads_writes_and_deinitializes(void) {
  esy_gpio_config_t config = output_config();
  bool value = false;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_write(&s_gpio, false));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_read(&s_gpio, &value));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(2u, state->driver_write_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_read_calls);
  TEST_ASSERT_EQUAL_UINT8(TEST_GPIO_PIN, state->last_pin);
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_OUTPUT, state->last_gpio_mode);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_deinit(&s_gpio));
  TEST_ASSERT_FALSE(esy_hal_gpio_runtime(&s_gpio)->initialized);
}

void test_gpio_explicit_state_can_be_applied_then_configured_state_restored(void) {
  esy_gpio_config_t config = output_config();
  const esy_gpio_state_t temporary_state = {
      .mode = ESY_GPIO_MODE_INPUT_PULLDOWN,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_gpio_apply_state(&s_gpio, &temporary_state));
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_INPUT_PULLDOWN,
                        platform_test_state()->last_gpio_mode);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_gpio_apply_configured_state(&s_gpio));
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_OUTPUT,
                        platform_test_state()->last_gpio_mode);
  TEST_ASSERT_TRUE(platform_test_state()->last_gpio_value);
}

void test_gpio_irq_and_wakeup_use_handle_pin_and_board_priority(void) {
  esy_gpio_config_t config = output_config();

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_gpio_attach_irq(
                              &s_gpio, ESY_GPIO_IRQ_RISING, ESY_GPIO_PULL_DOWN,
                              gpio_irq_handler, NULL));
  TEST_ASSERT_EQUAL_UINT8(TEST_GPIO_PIN, platform_test_state()->last_pin);
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_PULL_DOWN,
                        platform_test_state()->last_gpio_pull);
  TEST_ASSERT_EQUAL_UINT8(TEST_GPIO_IRQ_PRIORITY,
                          platform_test_state()->last_irq_priority);

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_gpio_enable_wakeup(&s_gpio, ESY_GPIO_IRQ_FALLING,
                                              gpio_irq_handler, NULL));
  TEST_ASSERT_TRUE(esy_hal_gpio_runtime(&s_gpio)->wake_source);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_disable_wakeup(&s_gpio));
  TEST_ASSERT_FALSE(esy_hal_gpio_runtime(&s_gpio)->wake_source);
  TEST_ASSERT_EQUAL_UINT32(1u,
                           platform_test_state()->driver_disable_wakeup_calls);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_gpio_enable_wakeup(&s_gpio, ESY_GPIO_IRQ_FALLING,
                                              gpio_irq_handler, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_deinit(&s_gpio));
  TEST_ASSERT_EQUAL_UINT32(2u,
                           platform_test_state()->driver_disable_wakeup_calls);
}

void test_gpio_rejects_invalid_lifecycle_and_non_output_write(void) {
  esy_gpio_config_t config;

  esy_hal_gpio_default_cfg(&config);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_gpio_init(&s_gpio, &config));
  config.pin = TEST_GPIO_PIN;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_hal_gpio_disable_wakeup(&s_gpio));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_hal_gpio_write(&s_gpio, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_hal_gpio_init(&s_gpio, &config));
}

void test_failed_wakeup_enable_keeps_cleanup_ownership_until_deinit_succeeds(void) {
  esy_gpio_config_t config = output_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  platform_test_set_gpio_wakeup_results(ESY_ERR_IO, ESY_ERR_BUSY, true);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_gpio_enable_wakeup(
      &s_gpio, ESY_GPIO_IRQ_RISING, gpio_irq_handler, &s_gpio));
  TEST_ASSERT_TRUE(esy_hal_gpio_runtime(&s_gpio)->wake_source);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_gpio_enable_wakeup(
      &s_gpio, ESY_GPIO_IRQ_RISING, gpio_irq_handler, &s_gpio));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_gpio_disable_wakeup(&s_gpio));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_gpio_deinit(&s_gpio));
  TEST_ASSERT_TRUE(esy_hal_gpio_runtime(&s_gpio)->initialized);
  TEST_ASSERT_TRUE(esy_hal_gpio_runtime(&s_gpio)->wake_source);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);
  platform_test_set_gpio_wakeup_results(ESY_ERR_IO, ESY_OK, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_gpio));
  TEST_ASSERT_FALSE(esy_hal_gpio_runtime(&s_gpio)->initialized);
  TEST_ASSERT_FALSE(esy_hal_gpio_runtime(&s_gpio)->wake_source);
}

void test_wakeup_enable_failure_without_borrow_allows_ordinary_deinit(void) {
  esy_gpio_config_t config = output_config();
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  platform_test_set_gpio_wakeup_results(ESY_ERR_IO, ESY_ERR_BUSY, false);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_gpio_enable_wakeup(
      &s_gpio, ESY_GPIO_IRQ_RISING, gpio_irq_handler, &s_gpio));
  TEST_ASSERT_FALSE(esy_hal_gpio_runtime(&s_gpio)->wake_source);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_gpio));
  TEST_ASSERT_EQUAL_UINT32(0u,
      platform_test_state()->driver_disable_wakeup_calls);
}
