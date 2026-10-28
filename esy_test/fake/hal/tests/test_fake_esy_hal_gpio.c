/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_gpio.h"
#include "esy_hal/esy_hal_gpio_pm.h"
#include "unity.h"

void setUp(void) { fake_esy_hal_gpio_reset(); }
void tearDown(void) {}

void test_gpio_queries_preserve_outputs_on_invalid_or_uninitialized_handles(void) {
  esy_gpio_t gpio = {0};
  const esy_gpio_config_t sentinel = {.pin = 42u, .mode = ESY_GPIO_MODE_OUTPUT,
      .output_value = true};
  esy_gpio_config_t observed = sentinel;
  TEST_ASSERT_FALSE(esy_hal_gpio_is_initialized(NULL));
  TEST_ASSERT_FALSE(esy_hal_gpio_is_initialized(&gpio));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_gpio_get_config(NULL, &observed));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_gpio_get_config(&gpio, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_gpio_get_config(&gpio, &observed));
  TEST_ASSERT_EQUAL_MEMORY(&sentinel, &observed, sizeof(observed));
}

void test_gpio_configuration_snapshot_survives_writes_and_pm_then_deinit_releases_it(void) {
  esy_gpio_t gpio = {0};
  const esy_gpio_config_t config = {.pin = 7u, .mode = ESY_GPIO_MODE_OUTPUT,
      .output_value = true};
  esy_gpio_config_t observed = {0};
  const esy_gpio_state_t sleep = {.mode = ESY_GPIO_MODE_INPUT};
  const esy_gpio_pm_pin_t pin = {.gpio = &gpio, .sleep_state = &sleep};
  const esy_device_gpio_pm_profile_t profile = {.pins = &pin, .pin_count = 1u};
  const uint8_t owner = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&gpio, &config));
  TEST_ASSERT_TRUE(esy_hal_gpio_is_initialized(&gpio));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_gpio_init(&gpio, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_write(&gpio, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_get_config(&gpio, &observed));
  TEST_ASSERT_EQUAL_MEMORY(&config, &observed, sizeof(config));
  TEST_ASSERT_FALSE(fake_esy_hal_gpio_get_current_state(&gpio).output_value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_claim(&profile, &owner));
  TEST_ASSERT_EQUAL_PTR(&owner, fake_esy_hal_gpio_get_pm_owner(&gpio));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_gpio_deinit(&gpio));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(&profile, &owner,
      ESY_GPIO_PM_STATE_SLEEP));
  TEST_ASSERT_EQUAL(ESY_GPIO_MODE_INPUT,
      fake_esy_hal_gpio_get_current_state(&gpio).mode);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_gpio_write(&gpio, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_get_config(&gpio, &observed));
  TEST_ASSERT_EQUAL_MEMORY(&config, &observed, sizeof(config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_gpio_pm_release(&profile, &owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(&profile, &owner,
      ESY_GPIO_PM_STATE_DEFAULT));
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_current_state(&gpio).output_value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_release(&profile, &owner));
  TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&gpio));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&gpio));
  TEST_ASSERT_FALSE(esy_hal_gpio_is_initialized(&gpio));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_gpio_get_config(&gpio, &observed));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&gpio, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&gpio));
}
