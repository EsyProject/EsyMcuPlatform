/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lcd/esy_lcd.h"
#include "esy_middleware/lcd/esy_lcd_device.h"

#include <string.h>

#include "unity.h"
#include "fake_esy_hal_gpio.h"
#include "esy_hal/esy_hal_gpio_pm.h"

typedef struct {
  esy_err_t result;
  uint32_t flush_count;
  uint32_t deinit_count;
  esy_err_t deinit_result;
  uint16_t x0;
  uint16_t y0;
  uint16_t x1;
  uint16_t y1;
  const uint8_t *color_buf;
  void (*done_cb)(void *);
  void *done_arg;
  void *flush_arg;
} fake_lcd_backend_t;

static fake_lcd_backend_t s_backend;
static uint8_t s_color_buf[8];

static esy_err_t fake_flush(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                            const uint8_t *color_buf, void (*done_cb)(void *),
                            void *cb_arg, void *arg) {
  esy_lcd_t *lcd = (esy_lcd_t *)arg;
  fake_lcd_backend_t *backend = (fake_lcd_backend_t *)lcd->ctx;

  backend->flush_count++;
  backend->x0 = x0;
  backend->y0 = y0;
  backend->x1 = x1;
  backend->y1 = y1;
  backend->color_buf = color_buf;
  backend->done_cb = done_cb;
  backend->done_arg = cb_arg;
  backend->flush_arg = arg;
  return backend->result;
}

static esy_err_t fake_deinit(void *arg) {
  esy_lcd_t *lcd = (esy_lcd_t *)arg;
  fake_lcd_backend_t *backend = (fake_lcd_backend_t *)lcd->ctx;

  backend->deinit_count++;
  if (backend->deinit_result != ESY_OK) return backend->deinit_result;
  lcd->flush = NULL;
  lcd->deinit = NULL;
  lcd->ctx = NULL;
  return ESY_OK;
}

static void fill_attached_lcd(esy_lcd_t *lcd) {
  memset(lcd, 0, sizeof(*lcd));
  lcd->ctx = &s_backend;
  lcd->flush = fake_flush;
  lcd->deinit = fake_deinit;
}

void setUp(void) {
  fake_esy_hal_gpio_reset();
  memset(&s_backend, 0, sizeof(s_backend));
  s_backend.result = ESY_OK;
  memset(s_color_buf, 0xA5, sizeof(s_color_buf));
}

void tearDown(void) {}

void test_lcd_facade_reports_attachment_state(void) {
  esy_lcd_t lcd;

  memset(&lcd, 0, sizeof(lcd));
  TEST_ASSERT_FALSE(esy_lcd_is_attached(NULL));
  TEST_ASSERT_FALSE(esy_lcd_is_attached(&lcd));

  lcd.ctx = &s_backend;
  TEST_ASSERT_FALSE(esy_lcd_is_attached(&lcd));

  lcd.flush = fake_flush;
  TEST_ASSERT_TRUE(esy_lcd_is_attached(&lcd));
}

void test_lcd_facade_detach_uses_backend_hook(void) {
  esy_lcd_t lcd;

  fill_attached_lcd(&lcd);

  esy_lcd_detach(NULL);
  esy_lcd_detach(&lcd);

  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.deinit_count);
  TEST_ASSERT_FALSE(esy_lcd_is_attached(&lcd));
  TEST_ASSERT_NULL(lcd.ctx);
  TEST_ASSERT_NULL(lcd.flush);
  TEST_ASSERT_NULL(lcd.deinit);
}

void test_lcd_facade_detach_clears_handle_without_backend_hook(void) {
  esy_lcd_t lcd;

  memset(&lcd, 0, sizeof(lcd));
  lcd.ctx = &s_backend;
  lcd.flush = fake_flush;
  lcd.deinit = NULL;

  esy_lcd_detach(&lcd);

  TEST_ASSERT_FALSE(esy_lcd_is_attached(&lcd));
  TEST_ASSERT_NULL(lcd.ctx);
  TEST_ASSERT_NULL(lcd.flush);
  TEST_ASSERT_NULL(lcd.deinit);
}

void test_lcd_facade_flush_validates_arguments_and_state(void) {
  esy_lcd_t lcd;

  memset(&lcd, 0, sizeof(lcd));

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_lcd_flush(NULL, 0u, 0u, 1u, 1u, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_lcd_flush(&lcd, 2u, 0u, 1u, 1u, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_lcd_flush(&lcd, 0u, 2u, 1u, 1u, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 1u, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 1u, s_color_buf, NULL, NULL));
}

void test_lcd_facade_flush_calls_backend(void) {
  esy_lcd_t lcd;

  fill_attached_lcd(&lcd);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_lcd_flush(&lcd, 1u, 2u, 3u, 4u,
                                                s_color_buf, NULL, &s_backend));

  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.flush_count);
  TEST_ASSERT_EQUAL_UINT16(1u, s_backend.x0);
  TEST_ASSERT_EQUAL_UINT16(2u, s_backend.y0);
  TEST_ASSERT_EQUAL_UINT16(3u, s_backend.x1);
  TEST_ASSERT_EQUAL_UINT16(4u, s_backend.y1);
  TEST_ASSERT_EQUAL_PTR(s_color_buf, s_backend.color_buf);
  TEST_ASSERT_EQUAL_PTR(&lcd, s_backend.flush_arg);
  TEST_ASSERT_EQUAL_PTR(&s_backend, s_backend.done_arg);
}

void test_lcd_facade_flush_propagates_backend_rejection(void) {
  esy_lcd_t lcd;

  fill_attached_lcd(&lcd);
  s_backend.result = ESY_ERR_BUSY;

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_BUSY,
      esy_lcd_flush(&lcd, 1u, 2u, 3u, 4u, s_color_buf, NULL, &s_backend));

  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.flush_count);
}

static esy_err_t fake_device_transition(esy_lcd_device_binding_t *binding,
                                        esy_device_pm_action_t action,
                                        esy_power_state_t system_state) {
  (void)binding;
  (void)action;
  (void)system_state;
  return ESY_OK;
}

static void fake_flush_done(void *arg) { (*(uint32_t *)arg)++; }

void test_lcd_device_tracks_async_flush_and_remains_runtime_active(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_lcd_device_config_t device_config = {
      .registration = {
          .name = "lcd",
          .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
      },
  };
  esy_lcd_device_binding_t binding = {0};
  esy_lcd_t lcd;
  uint32_t usage_count = 0u;
  uint32_t done_count = 0u;
  esy_device_state_t state;

  fill_attached_lcd(&lcd);
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_lcd_register_device(&binding, &lcd, &registry, &device_config,
                                      fake_device_transition, &s_backend));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_lcd_flush(&lcd, 0u, 0u, 1u, 1u, s_color_buf, fake_flush_done,
                            &done_count));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&binding.device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_lcd_flush(&lcd, 0u, 0u, 1u, 1u, s_color_buf,
                                        fake_flush_done, &done_count));
  s_backend.done_cb(s_backend.done_arg);
  TEST_ASSERT_EQUAL_UINT32(1u, done_count);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&binding.device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(0u, usage_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
}

void test_lcd_device_can_unregister_before_registry_start(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_lcd_device_config_t device_config = {
      .registration = {
          .name = "lcd",
          .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
      },
  };
  esy_lcd_device_binding_t binding = {0};
  esy_lcd_t lcd;

  fill_attached_lcd(&lcd);
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_lcd_register_device(&binding, &lcd, &registry, &device_config,
                                      fake_device_transition, &s_backend));
  esy_lcd_detach(&lcd);
  TEST_ASSERT_TRUE(esy_lcd_is_attached(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_lcd_unregister_device(&binding));
  TEST_ASSERT_NULL(lcd.device);
}

void test_lcd_detach_preserves_handle_when_backend_is_busy(void) {
  esy_lcd_t lcd;
  fill_attached_lcd(&lcd);
  s_backend.deinit_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_lcd_detach(&lcd));
  TEST_ASSERT_TRUE(esy_lcd_is_attached(&lcd));
  TEST_ASSERT_EQUAL_PTR(&s_backend, lcd.ctx);
  s_backend.deinit_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_FALSE(esy_lcd_is_attached(&lcd));
}

void test_live_binding_reuse_is_rejected_without_corrupting_graph(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t rc = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_lcd_device_config_t dc = {.registration = {.name = "lcd", .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)}};
  esy_lcd_device_binding_t binding = {0};
  esy_lcd_t a, b;
  fill_attached_lcd(&a); fill_attached_lcd(&b);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_device_registry_init(&registry, &rc));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_register_device(&binding, &a, &registry, &dc, fake_device_transition, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_lcd_register_device(&binding, &b, &registry, &dc, fake_device_transition, NULL));
  TEST_ASSERT_EQUAL_UINT(1, registry.device_count);
  TEST_ASSERT_EQUAL_PTR(&binding.device, resources.devices[0]);
  TEST_ASSERT_NULL(b.device);
  TEST_ASSERT_EQUAL_PTR(&a, ((esy_lcd_device_binding_t *)a.device->driver_ctx)->lcd);
}

void test_registry_teardown_allows_binding_cleanup_and_reinit(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t rc = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_lcd_device_config_t dc = {.registration = {.name = "lcd", .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)}};
  esy_lcd_device_binding_t binding = {0};
  esy_lcd_t lcd; fill_attached_lcd(&lcd);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_device_registry_init(&registry, &rc));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_register_device(&binding, &lcd, &registry, &dc, fake_device_transition, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_lcd_unregister_device(&binding));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_lcd_flush(&lcd, 0, 0, 0, 0, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_unregister_device(&binding));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_detach(&lcd));
  fill_attached_lcd(&lcd);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_device_registry_init(&registry, &rc));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_register_device(&binding, &lcd, &registry, &dc, fake_device_transition, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_unregister_device(&binding));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_detach(&lcd));
}

void test_gpio_release_failure_retains_binding_for_retry(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t rc = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_gpio_t gpio = {0}; esy_gpio_config_t gc;
  esy_hal_gpio_default_cfg(&gc); gc.pin = 31; gc.mode = ESY_GPIO_MODE_OUTPUT;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_gpio_init(&gpio, &gc));
  const esy_gpio_pm_pin_t pin = {.gpio = &gpio};
  const esy_device_gpio_pm_profile_t profile = {.pins = &pin, .pin_count = 1};
  esy_lcd_device_config_t dc = {.registration = {.name = "lcd", .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)}, .gpio_pm_profile = &profile};
  esy_lcd_device_binding_t binding = {0}; esy_lcd_t lcd; fill_attached_lcd(&lcd);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_device_registry_init(&registry, &rc));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_register_device(&binding, &lcd, &registry, &dc, fake_device_transition, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_gpio_pm_select(&profile, &binding, ESY_GPIO_PM_STATE_SLEEP));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_lcd_unregister_device(&binding));
  TEST_ASSERT_EQUAL_UINT(0, registry.device_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_gpio_pm_select(&profile, &binding, ESY_GPIO_PM_STATE_DEFAULT));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_unregister_device(&binding));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&gpio));
}
