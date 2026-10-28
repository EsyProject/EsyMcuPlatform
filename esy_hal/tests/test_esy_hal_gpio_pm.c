/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_gpio_pm.h"

#include <string.h>

#include "esy_hal/esy_hal_gpio.h"
#include "hal_test_fakes.h"
#include "esy_hal_gpio_internal.h"
#include "unity.h"

static esy_gpio_t s_gpio;
static esy_gpio_t s_second_gpio;
static uint32_t s_owner;
static uint32_t s_second_owner;

void setUp(void) {
  esy_gpio_config_t config;

  platform_test_reset();
  memset(&s_gpio, 0, sizeof(s_gpio));
  memset(&s_second_gpio, 0, sizeof(s_second_gpio));
  esy_hal_gpio_default_cfg(&config);
  config.pin = TEST_GPIO_PIN;
  config.mode = ESY_GPIO_MODE_OUTPUT;
  config.output_value = true;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(&s_gpio, &config));
  config.pin = TEST_GPIO_PIN + 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_gpio_init(&s_second_gpio, &config));
}

void tearDown(void) {}

void test_gpio_pm_claims_selects_blocks_bypass_and_releases(void) {
  bool value;
  const esy_gpio_state_t sleep_state = {
      .mode = ESY_GPIO_MODE_INPUT_PULLUP,
  };
  const esy_gpio_pm_pin_t pins[] = {
      {.gpio = &s_gpio, .sleep_state = &sleep_state},
      {.gpio = &s_second_gpio},
  };
  const esy_device_gpio_pm_profile_t profile = {
      .pins = pins,
      .pin_count = sizeof(pins) / sizeof(pins[0]),
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_gpio_pm_claim(&profile, &s_owner));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_gpio_pm_select(
                  &profile, &s_owner, ESY_GPIO_PM_STATE_SLEEP));
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_INPUT_PULLUP, esy_hal_gpio_runtime(&s_gpio)->current_state.mode);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_hal_gpio_read(&s_gpio, &value));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_hal_gpio_write(&s_second_gpio, false));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_hal_gpio_deinit(&s_gpio));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_gpio_pm_select(
                  &profile, &s_owner, ESY_GPIO_PM_STATE_DEFAULT));
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_OUTPUT, esy_hal_gpio_runtime(&s_gpio)->current_state.mode);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_gpio_pm_release(&profile, &s_owner));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_write(&s_gpio, false));
}

void test_gpio_pm_rejects_duplicate_physical_pin_owners(void) {
  esy_gpio_t alias_gpio;
  esy_gpio_config_t alias_config;
  const esy_gpio_pm_pin_t first_pin = {.gpio = &s_gpio};
  const esy_device_gpio_pm_profile_t first_profile = {
      .pins = &first_pin,
      .pin_count = 1u,
  };
  const esy_gpio_pm_pin_t alias_pin = {.gpio = &alias_gpio};
  const esy_device_gpio_pm_profile_t alias_profile = {
      .pins = &alias_pin,
      .pin_count = 1u,
  };

  memset(&alias_gpio, 0, sizeof(alias_gpio));
  esy_hal_gpio_default_cfg(&alias_config);
  alias_config.pin = TEST_GPIO_PIN;
  alias_config.mode = ESY_GPIO_MODE_OUTPUT;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_gpio_init(&alias_gpio, &alias_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_gpio_pm_claim(&first_profile, &s_owner));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_hal_gpio_write(&alias_gpio, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_hal_gpio_deinit(&alias_gpio));
  esy_gpio_t new_alias = {0};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_hal_gpio_init(&new_alias, &alias_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_BUSY,
      esy_hal_gpio_pm_claim(&alias_profile, &s_second_owner));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_gpio_pm_release(&first_profile, &s_owner));
}

void test_gpio_pm_rejects_wrong_owner_and_invalid_profile(void) {
  const esy_gpio_pm_pin_t duplicate_pins[] = {
      {.gpio = &s_gpio},
      {.gpio = &s_gpio},
  };
  const esy_device_gpio_pm_profile_t duplicate_profile = {
      .pins = duplicate_pins,
      .pin_count = sizeof(duplicate_pins) / sizeof(duplicate_pins[0]),
  };
  const esy_gpio_pm_pin_t pin = {.gpio = &s_gpio};
  const esy_device_gpio_pm_profile_t profile = {
      .pins = &pin,
      .pin_count = 1u,
  };

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_gpio_pm_claim(&duplicate_profile, &s_owner));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_gpio_pm_claim(&profile, &s_owner));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_hal_gpio_pm_select(&profile, &s_second_owner,
                                  ESY_GPIO_PM_STATE_SLEEP));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_gpio_pm_release(&profile, &s_owner));
}

static void count_irq(uint8_t pin, void *arg) {
  (void)pin;
  ++*(uint32_t *)arg;
}

void test_irq_rejection_rolls_back_profile_peers_without_detaching_wakeup(void) {
  uint32_t deliveries = 0u;
  const esy_gpio_state_t sleep = {.mode = ESY_GPIO_MODE_INPUT_PULLUP};
  const esy_gpio_pm_pin_t pins[] = {
      {.gpio = &s_second_gpio, .sleep_state = &sleep},
      {.gpio = &s_gpio, .sleep_state = &sleep},
  };
  const esy_device_gpio_pm_profile_t profile = {.pins = pins, .pin_count = 2u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_enable_wakeup(&s_gpio,
      ESY_GPIO_IRQ_RISING, count_irq, &deliveries));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_claim(&profile, &s_owner));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_hal_gpio_pm_select(&profile, &s_owner, ESY_GPIO_PM_STATE_SLEEP));
  TEST_ASSERT_EQUAL(ESY_GPIO_MODE_OUTPUT, esy_hal_gpio_runtime(&s_second_gpio)->current_state.mode);
  TEST_ASSERT_EQUAL(ESY_GPIO_PM_STATE_DEFAULT, esy_hal_gpio_runtime(&s_second_gpio)->pm_state);
  TEST_ASSERT_EQUAL(ESY_GPIO_MODE_OUTPUT, esy_hal_gpio_runtime(&s_gpio)->current_state.mode);
  TEST_ASSERT_EQUAL(ESY_GPIO_PM_STATE_DEFAULT, esy_hal_gpio_runtime(&s_gpio)->pm_state);
  TEST_ASSERT_TRUE(esy_hal_gpio_runtime(&s_gpio)->wake_source);
  platform_test_fire_gpio_irq();
  TEST_ASSERT_EQUAL_UINT32(1u, deliveries);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_release(&profile, &s_owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_disable_wakeup(&s_gpio));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_apply_state(&s_gpio, &sleep));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_apply_configured_state(&s_gpio));
}
