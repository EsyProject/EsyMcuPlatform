/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/touch/esy_touch.h"
#include "esy_middleware/touch/esy_touch_device.h"
#include "esy_hal/esy_hal_gpio_pm.h"
#include "fake_esy_hal_gpio.h"

#include <string.h>

#include "unity.h"

typedef struct {
  esy_err_t result;
  uint16_t x;
  uint16_t y;
  bool pressed;
  uint32_t poll_count;
  uint32_t get_sample_count;
} fake_touch_backend_t;

static fake_touch_backend_t s_backend;

static esy_err_t fake_poll(void *arg) {
  esy_touch_t *touch = (esy_touch_t *)arg;
  fake_touch_backend_t *backend = (fake_touch_backend_t *)touch->ctx;

  backend->poll_count++;
  return backend->result;
}

static esy_err_t fake_get_sample(esy_touch_data_t *sample, void *arg) {
  esy_touch_t *touch = (esy_touch_t *)arg;
  fake_touch_backend_t *backend = (fake_touch_backend_t *)touch->ctx;

  backend->get_sample_count++;
  sample->x = backend->x;
  sample->y = backend->y;
  sample->is_pressed = backend->pressed;
  return backend->result;
}

void setUp(void) {
  memset(&s_backend, 0, sizeof(s_backend));
  s_backend.result = ESY_OK;
  s_backend.x = 12u;
  s_backend.y = 34u;
  s_backend.pressed = true;
}

void tearDown(void) {}

void test_touch_facade_reports_attachment_state(void) {
  esy_touch_t touch;

  memset(&touch, 0, sizeof(touch));
  TEST_ASSERT_FALSE(esy_touch_is_attached(NULL));
  TEST_ASSERT_FALSE(esy_touch_is_attached(&touch));

  touch.ctx = &s_backend;
  TEST_ASSERT_FALSE(esy_touch_is_attached(&touch));

  touch.poll = fake_poll;
  TEST_ASSERT_FALSE(esy_touch_is_attached(&touch));
  touch.get_sample = fake_get_sample;
  TEST_ASSERT_TRUE(esy_touch_is_attached(&touch));
}

void test_touch_facade_detach_clears_handle(void) {
  esy_touch_t touch;

  memset(&touch, 0, sizeof(touch));
  touch.ctx = &s_backend;
  touch.poll = fake_poll;
  touch.get_sample = fake_get_sample;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_touch_detach(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_detach(&touch));

  TEST_ASSERT_FALSE(esy_touch_is_attached(&touch));
  TEST_ASSERT_NULL(touch.ctx);
  TEST_ASSERT_NULL(touch.poll);
  TEST_ASSERT_NULL(touch.get_sample);
}

void test_touch_facade_read_validates_arguments_and_state(void) {
  esy_touch_t touch;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  memset(&touch, 0, sizeof(touch));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(NULL, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(&touch, NULL, &y, &pressed));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(&touch, &x, NULL, &pressed));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(&touch, &x, &y, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
}

void test_touch_facade_read_after_detach_returns_state_error(void) {
  esy_touch_t touch;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  memset(&touch, 0, sizeof(touch));
  touch.ctx = &s_backend;
  touch.poll = fake_poll;
  touch.get_sample = fake_get_sample;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_detach(&touch));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.poll_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.get_sample_count);
}

void test_touch_facade_read_calls_split_backend_and_propagates_result(void) {
  esy_touch_t touch;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  memset(&touch, 0, sizeof(touch));
  touch.ctx = &s_backend;
  touch.poll = fake_poll;
  touch.get_sample = fake_get_sample;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.poll_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.get_sample_count);
  TEST_ASSERT_EQUAL_UINT16(12u, x);
  TEST_ASSERT_EQUAL_UINT16(34u, y);
  TEST_ASSERT_TRUE(pressed);

  s_backend.result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(2u, s_backend.poll_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.get_sample_count);
}

void test_touch_facade_poll_and_get_sample_use_split_backend_hooks(void) {
  esy_touch_t touch;
  esy_touch_data_t sample;

  memset(&touch, 0, sizeof(touch));
  touch.ctx = &s_backend;
  touch.poll = fake_poll;
  touch.get_sample = fake_get_sample;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_poll(&touch));
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.poll_count);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_get_sample(&touch, &sample));
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.get_sample_count);
  TEST_ASSERT_EQUAL_UINT16(12u, sample.x);
  TEST_ASSERT_EQUAL_UINT16(34u, sample.y);
  TEST_ASSERT_TRUE(sample.is_pressed);

  s_backend.result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_touch_poll(&touch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_touch_get_sample(&touch, &sample));
}

void test_touch_facade_read_uses_split_poll_and_get_sample_hooks(void) {
  esy_touch_t touch;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  memset(&touch, 0, sizeof(touch));
  touch.ctx = &s_backend;
  touch.poll = fake_poll;
  touch.get_sample = fake_get_sample;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.poll_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_backend.get_sample_count);
  TEST_ASSERT_EQUAL_UINT16(12u, x);
  TEST_ASSERT_EQUAL_UINT16(34u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_touch_facade_get_sample_requires_split_backend_hook(void) {
  esy_touch_t touch;
  esy_touch_data_t sample;

  memset(&touch, 0, sizeof(touch));
  touch.ctx = &s_backend;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_get_sample(NULL, &sample));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_get_sample(&touch, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_touch_get_sample(&touch, &sample));
}

static esy_err_t fake_touch_transition(esy_touch_device_binding_t *binding,
                                       esy_device_pm_action_t action,
                                       esy_power_state_t system_state) {
  (void)binding;
  (void)action;
  (void)system_state;
  return ESY_OK;
}

void test_touch_device_runtime_suspend_and_lazy_resume(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_touch_device_config_t device_config = {
      .registration = {
          .name = "touch",
          .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
      },
  };
  esy_touch_device_binding_t binding = {0};
  esy_touch_t touch = {
      .ctx = &s_backend,
      .poll = fake_poll,
      .get_sample = fake_get_sample,
  };
  esy_device_state_t state;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_touch_register_device(&binding, &touch, &registry, &device_config,
                                fake_touch_transition, &s_backend));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_poll(&touch));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
}

void test_touch_device_can_unregister_before_registry_start(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_touch_device_config_t device_config = {
      .registration = {
          .name = "touch",
          .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
      },
  };
  esy_touch_device_binding_t binding = {0};
  esy_touch_t touch = {
      .ctx = &s_backend,
      .poll = fake_poll,
      .get_sample = fake_get_sample,
  };

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_touch_register_device(&binding, &touch, &registry, &device_config,
                                fake_touch_transition, &s_backend));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_touch_detach(&touch));
  TEST_ASSERT_TRUE(esy_touch_is_attached(&touch));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_unregister_device(&binding));
  TEST_ASSERT_NULL(touch.device);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_detach(&touch));
  TEST_ASSERT_FALSE(esy_touch_is_attached(&touch));
  TEST_ASSERT_NULL(touch.ctx);
  TEST_ASSERT_NULL(touch.poll);
  TEST_ASSERT_NULL(touch.get_sample);
}

void test_touch_live_binding_rejects_reuse_and_preserves_graph_and_gpio_claim(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_touch_device_binding_t binding = {0};
  esy_touch_device_binding_t other_binding = {0};
  esy_touch_t first = {.ctx = &s_backend, .poll = fake_poll,
                       .get_sample = fake_get_sample};
  esy_touch_t second = {.ctx = &s_backend, .poll = fake_poll,
                        .get_sample = fake_get_sample};
  esy_gpio_t gpio = {0};
  esy_gpio_config_t gpio_config;
  const esy_gpio_state_t idle = {.mode = ESY_GPIO_MODE_OUTPUT};
  const esy_gpio_pm_pin_t pin = {.gpio = &gpio, .idle_state = &idle};
  const esy_device_gpio_pm_profile_t profile = {.pins = &pin, .pin_count = 1u};
  const esy_touch_device_config_t registration = {
      .registration = {.name = "touch-owner", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)},
      .gpio_pm_profile = &profile};
  uint8_t before[sizeof(binding)];

  fake_esy_hal_gpio_reset();
  esy_hal_gpio_default_cfg(&gpio_config);
  gpio_config.pin = 5u;
  gpio_config.mode = ESY_GPIO_MODE_OUTPUT;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&gpio, &gpio_config));
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  const esy_device_registry_config_t graph = {.static_resources = &resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &graph));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_register_device(&binding, &first,
      &registry, &registration, fake_touch_transition, &s_backend));
  memcpy(before, &binding, sizeof(binding));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_touch_register_device(&binding, &first,
      &registry, &registration, fake_touch_transition, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_touch_register_device(&binding, &second,
      &registry, &registration, fake_touch_transition, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_touch_register_device(&other_binding,
      &first, &registry, &registration, fake_touch_transition, NULL));
  TEST_ASSERT_EQUAL_MEMORY(before, &binding, sizeof(binding));
  TEST_ASSERT_EQUAL_UINT32(1u, registry.device_count);
  TEST_ASSERT_EQUAL_PTR(&binding.device, storage.devices[0]);
  TEST_ASSERT_NULL(storage.devices[1]);
  TEST_ASSERT_EQUAL_PTR(&binding.device, first.device);
  TEST_ASSERT_NULL(second.device);
  TEST_ASSERT_NULL(other_binding.touch);
  TEST_ASSERT_EQUAL_PTR(&binding, fake_esy_hal_gpio_get_pm_owner(&gpio));

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(
      &profile, &binding, ESY_GPIO_PM_STATE_IDLE));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_touch_unregister_device(&binding));
  TEST_ASSERT_EQUAL_UINT32(0u, registry.device_count);
  TEST_ASSERT_EQUAL_PTR(&binding.device, first.device);
  TEST_ASSERT_EQUAL_PTR(&binding, fake_esy_hal_gpio_get_pm_owner(&gpio));
  memcpy(before, &binding, sizeof(binding));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_touch_register_device(&binding, &second,
      &registry, &registration, fake_touch_transition, NULL));
  TEST_ASSERT_EQUAL_MEMORY(before, &binding, sizeof(binding));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_touch_detach(&first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(
      &profile, &binding, ESY_GPIO_PM_STATE_DEFAULT));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_unregister_device(&binding));
  TEST_ASSERT_NULL(first.device);
  TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&gpio));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_register_device(&binding, &second,
      &registry, &registration, fake_touch_transition, &s_backend));
  TEST_ASSERT_EQUAL_UINT32(1u, registry.device_count);
  TEST_ASSERT_EQUAL_PTR(&binding.device, second.device);
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&second));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&gpio));
}

void test_touch_registration_failure_releases_gpio_and_allows_retry(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_t occupied = {0};
  esy_touch_device_binding_t binding = {0};
  esy_touch_t touch = {.ctx = &s_backend, .poll = fake_poll,
                       .get_sample = fake_get_sample};
  esy_gpio_t gpio = {0};
  esy_gpio_config_t gpio_config;
  const esy_gpio_pm_pin_t pin = {.gpio = &gpio};
  const esy_device_gpio_pm_profile_t profile = {.pins = &pin, .pin_count = 1u};
  const esy_device_config_t occupied_config = {
      .name = "occupied", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)};
  const esy_touch_device_config_t registration = {
      .registration = {.name = "touch-retry", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)},
      .gpio_pm_profile = &profile};

  fake_esy_hal_gpio_reset();
  esy_hal_gpio_default_cfg(&gpio_config);
  gpio_config.pin = 5u;
  gpio_config.mode = ESY_GPIO_MODE_OUTPUT;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&gpio, &gpio_config));
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  const esy_device_registry_config_t graph = {.static_resources = &resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &graph));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_register(
      &registry, &occupied, &occupied_config));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_touch_register_device(&binding, &touch,
      &registry, &registration, fake_touch_transition, &s_backend));
  TEST_ASSERT_TRUE(esy_touch_is_attached(&touch));
  TEST_ASSERT_NULL(touch.device);
  TEST_ASSERT_NULL(binding.touch);
  TEST_ASSERT_NULL(binding.transition);
  TEST_ASSERT_NULL(binding.gpio_pm_profile);
  TEST_ASSERT_NULL(binding.owner);
  TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&gpio));
  TEST_ASSERT_EQUAL_UINT32(1u, registry.device_count);
  TEST_ASSERT_EQUAL_PTR(&occupied, storage.devices[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_unregister(&occupied));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_register_device(&binding, &touch,
      &registry, &registration, fake_touch_transition, &s_backend));
  TEST_ASSERT_EQUAL_PTR(&binding, fake_esy_hal_gpio_get_pm_owner(&gpio));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&gpio));
}

void test_touch_unbind_after_graph_teardown_retries_gpio_release_and_reuses_handle(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_touch_device_binding_t binding = {0};
  esy_touch_t touch = {0};
  esy_gpio_t gpio = {0};
  esy_gpio_config_t gpio_config;
  esy_hal_gpio_default_cfg(&gpio_config);
  gpio_config.pin = 5u;
  gpio_config.mode = ESY_GPIO_MODE_OUTPUT;
  const esy_gpio_state_t idle = {.mode = ESY_GPIO_MODE_OUTPUT};
  const esy_gpio_pm_pin_t pin = {.gpio = &gpio, .idle_state = &idle};
  const esy_device_gpio_pm_profile_t profile = {.pins = &pin, .pin_count = 1u};
  const esy_touch_device_config_t registration = {
      .registration = {.name = "touch-teardown",
          .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)},
      .gpio_pm_profile = &profile};
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  const esy_device_registry_config_t graph = {.static_resources = &resources};
  fake_esy_hal_gpio_reset();
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&gpio, &gpio_config));
  for (unsigned cycle = 0u; cycle < 2u; ++cycle) {
    touch.ctx = &s_backend;
    touch.poll = fake_poll;
    touch.get_sample = fake_get_sample;
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &graph));
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_register_device(&binding, &touch,
        &registry, &registration, fake_touch_transition, &s_backend));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_poll(&touch));
    /* The synchronous producer has returned before graph retirement. */
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(&profile,
        &binding, ESY_GPIO_PM_STATE_IDLE));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_touch_unregister_device(&binding));
    TEST_ASSERT_EQUAL_PTR(&binding.device, touch.device);
    TEST_ASSERT_EQUAL_PTR(&binding, fake_esy_hal_gpio_get_pm_owner(&gpio));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_touch_detach(&touch));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(&profile,
        &binding, ESY_GPIO_PM_STATE_DEFAULT));
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_unregister_device(&binding));
    TEST_ASSERT_NULL(touch.device);
    TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&gpio));
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
    TEST_ASSERT_FALSE(esy_touch_is_attached(&touch));
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&gpio));
}
