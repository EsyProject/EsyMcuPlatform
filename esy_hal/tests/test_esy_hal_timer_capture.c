/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_timer_capture.h"
#include "esy_hal/esy_hal_timer_capture_device.h"

#include "../src/esy_hal_timer_capture_internal.h"
#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) { platform_test_reset(); }

void tearDown(void) {}

static void init_timer_capture(esy_timer_capture_t *cap) {
  esy_timer_capture_config_t cfg = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX,
      .rising_edge = true,
      .inner =
          {
              .gpio = TEST_GPIO_PIN,
              .channel = 2u,
              .tim_id = 1u,
          },
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_timer_capture_init(cap, &cfg));
}

void test_timer_capture_native_ops_are_shared_and_cleanup_is_per_handle(void) {
  esy_timer_capture_t first = {0};
  esy_timer_capture_t second = {0};
  init_timer_capture(&first);
  const esy_timer_capture_vtable_t *ops = first.ops;
  TEST_ASSERT_NOT_NULL(ops);
  TEST_ASSERT_TRUE(esy_hal_timer_capture_runtime(&first)->pins_configured);
  TEST_ASSERT_FALSE(esy_hal_timer_capture_runtime(&second)->pins_configured);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&first));
  TEST_ASSERT_NULL(first.ops);
  TEST_ASSERT_FALSE(esy_hal_timer_capture_runtime(&first)->pins_configured);

  init_timer_capture(&second);
  TEST_ASSERT_EQUAL_PTR(ops, second.ops);
  TEST_ASSERT_TRUE(esy_hal_timer_capture_runtime(&second)->pins_configured);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&second));
}

void test_timer_capture_custom_immutable_ops_may_omit_optional_hooks(void) {
  static const esy_timer_capture_vtable_t k_empty_ops = {0};
  esy_timer_capture_t capture = {.ops = &k_empty_ops};
  uint32_t value = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED,
      esy_hal_timer_capture_get_value(&capture, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED,
      esy_hal_timer_capture_get_interval(&capture, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED,
      esy_hal_timer_capture_get_caps(&capture, &value));
}

void test_esy_hal_timer_capture_contracts_are_enforced(void) {
  esy_timer_capture_t cap = {0};
  uint32_t value = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_timer_capture_init(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_timer_capture_get_value(NULL, &value));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_timer_capture_get_value(&cap, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_timer_capture_get_value(&cap, &value));

  init_timer_capture(&cap);
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  TEST_ASSERT_NOT_NULL(cap.ops->get_value);
  TEST_ASSERT_NOT_NULL(cap.ops->get_interval);
  TEST_ASSERT_EQUAL_UINT8(TEST_GPIO_PIN, state->last_timer_capture_config.gpio);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_timer_capture_get_value(&cap, &value));
  TEST_ASSERT_EQUAL_UINT32(100u, value);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_timer_capture_get_interval(&cap, &value));
  TEST_ASSERT_EQUAL_UINT32(25u, value);

  platform_test_reset_order_counters();
  esy_hal_timer_capture_deinit(&cap);
  state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT8(TEST_TIMER_CAPTURE_INDEX, state->reset_index);
  TEST_ASSERT_TRUE(state->driver_get_index_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->driver_deinit_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->board_reset_order < state->capture_release_order);
}

void test_timer_capture_device_runtime_suspend_and_resume(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "capture",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
  };
  esy_timer_capture_config_t config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX,
      .rising_edge = true,
      .inner =
          {
              .gpio = TEST_GPIO_PIN,
              .channel = 2u,
              .tim_id = 1u,
          },
  };
  esy_timer_capture_device_binding_t binding = {0};
  esy_timer_capture_t capture = {0};
  esy_device_state_t state;
  uint32_t value = 0u;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_timer_capture_register_device(
                  &binding, &capture, &registry, &registration, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_timer_capture_get_value(&capture, &value));
  TEST_ASSERT_EQUAL_UINT32(100u, value);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
}

void test_timer_capture_device_unregisters_before_start(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config = {0};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "capture",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
  };
  esy_timer_capture_config_t config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX,
      .rising_edge = true,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u},
  };
  esy_timer_capture_device_binding_t binding = {0};
  esy_timer_capture_t capture = {0};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_timer_capture_register_device(
                  &binding, &capture, &registry, &registration, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_timer_capture_unregister_device(&binding));
  TEST_ASSERT_NULL(esy_hal_timer_capture_runtime(&capture)->device);
}

void test_timer_capture_capabilities_are_public_and_validated(void) {
  esy_timer_capture_t capture = {0};
  esy_timer_capture_config_t config;
  uint32_t caps;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_timer_capture_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_timer_capture_get_caps(&capture, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED, esy_hal_timer_capture_get_caps(&capture, &caps));
  esy_hal_timer_capture_default_cfg(&config);
  TEST_ASSERT_NULL(config.callback);
  TEST_ASSERT_EQUAL_UINT8(ESY_PIN_UNUSED, config.inner.gpio);
  config.inner.gpio = TEST_GPIO_PIN;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_get_caps(&capture, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_TIMER_CAPTURE_CAP_EDGE_CALLBACK |
                           ESY_TIMER_CAPTURE_CAP_INTERVAL, caps);
  esy_hal_timer_capture_deinit(&capture);
}

void test_capture_claim_rejects_competitors_without_resetting_owner_pins(void) {
  esy_timer_capture_t owner = {0};
  esy_timer_capture_t contender = {0};
  esy_timer_capture_config_t config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX, .rising_edge = true,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u}};
  uint32_t value = 0u;
  init_timer_capture(&owner);
  const platform_test_state_t *state = platform_test_state();
  uint32_t configure_order = state->board_configure_order;
  TEST_ASSERT_TRUE(state->capture_claim_order < configure_order);

  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_timer_capture_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_timer_capture_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configure_order, state->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, state->driver_deinit_order);
  TEST_ASSERT_EQUAL_UINT32(0u, state->board_reset_order);
  TEST_ASSERT_NULL(contender.ctx);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_get_value(&owner, &value));
  TEST_ASSERT_EQUAL_UINT32(100u, value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&contender));
}

void test_capture_rejected_board_pin_does_not_initialize_or_reset_hardware(void) {
  esy_timer_capture_t capture = {0};
  esy_timer_capture_config_t config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u}};
  platform_test_set_capture_reject_pin(true);

  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
                    esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_NULL(capture.ctx);
  TEST_ASSERT_NULL(platform_test_state()->capture_owner);
  platform_test_set_capture_reject_pin(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&capture));

  platform_test_reset_order_counters();
  uint32_t init_calls = platform_test_state()->driver_init_calls;
  platform_test_set_capture_reject_pin(true);
  platform_test_set_capture_cleanup_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_NOT_NULL(capture.ctx);
  TEST_ASSERT_FALSE(esy_hal_timer_capture_runtime(&capture)->pins_configured);
  platform_test_set_capture_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&capture));
  TEST_ASSERT_EQUAL_UINT32(init_calls, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
}

void test_capture_failed_cleanup_retains_owner_pins_and_can_be_retried(void) {
  esy_timer_capture_t owner = {0};
  esy_timer_capture_t contender = {0};
  esy_timer_capture_config_t config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u}};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_timer_capture_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&owner));
  init_timer_capture(&owner);
  void *context = owner.ctx;
  platform_test_reset_order_counters();
  platform_test_set_capture_cleanup_result(ESY_ERR_IO);

  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_timer_capture_deinit(&owner));
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
  TEST_ASSERT_NOT_NULL(owner.ops->get_value);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->capture_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->capture_release_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_timer_capture_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_timer_capture_init(&contender, &config));
  platform_test_set_capture_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&owner));
  TEST_ASSERT_NULL(owner.ctx);
  TEST_ASSERT_NULL(owner.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&contender));
}

void test_capture_failed_init_cleans_only_its_claim_or_retains_cleanup_retry(void) {
  esy_timer_capture_t capture = {0};
  esy_timer_capture_t contender = {0};
  esy_timer_capture_config_t config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u}};
  platform_test_set_init_result(ESY_ERR_INVALID_ARG);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_NULL(capture.ctx);
  TEST_ASSERT_NULL(platform_test_state()->capture_owner);
  TEST_ASSERT_TRUE(platform_test_state()->driver_deinit_order <
                   platform_test_state()->board_reset_order);
  TEST_ASSERT_TRUE(platform_test_state()->board_reset_order <
                   platform_test_state()->capture_release_order);

  platform_test_reset_order_counters();
  platform_test_set_capture_cleanup_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_NOT_NULL(capture.ctx);
  TEST_ASSERT_EQUAL_PTR(&capture, platform_test_state()->capture_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_timer_capture_init(&capture, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_timer_capture_init(&contender, &config));
  platform_test_set_init_result(ESY_OK);
  platform_test_set_capture_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&capture));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&contender));
}

void test_capture_system_suspend_keeps_exclusive_claim_and_resumes_owner(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t registry_config = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "capture", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  esy_timer_capture_config_t config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u}};
  esy_timer_capture_t owner = {0};
  esy_timer_capture_t contender = {0};
  esy_timer_capture_device_binding_t binding = {0};
  uint32_t value = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_register_device(
      &binding, &owner, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_prepare(
      &registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_TRUE(binding.suspended);
  TEST_ASSERT_FALSE(platform_test_state()->capture_enabled);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->capture_owner);
  uint32_t configure_order = platform_test_state()->board_configure_order;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_timer_capture_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configure_order,
                          platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_abort(&registry));
  TEST_ASSERT_FALSE(binding.suspended);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_get_value(&owner, &value));
  TEST_ASSERT_EQUAL_UINT32(100u, value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&owner));
}

void test_capture_partial_suspend_failure_faults_graph_and_preserves_claim(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t registry_config = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "capture-failed-suspend", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  esy_timer_capture_config_t config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u}};
  esy_timer_capture_t owner = {0};
  esy_timer_capture_t contender = {0};
  esy_timer_capture_device_binding_t binding = {0};
  esy_device_state_t state;
  uint32_t value = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_register_device(
      &binding, &owner, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  void *context = owner.ctx;
  platform_test_reset_order_counters();
  platform_test_set_capture_cleanup_result(ESY_ERR_IO);

  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_device_registry_system_prepare(
      &registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_FAULTED, state);
  TEST_ASSERT_FALSE(binding.suspended);
  TEST_ASSERT_FALSE(platform_test_state()->capture_enabled);
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->capture_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->capture_release_order);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_timer_capture_get_value(&owner, &value));
  TEST_ASSERT_EQUAL_UINT32(0u, value);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_timer_capture_init(&contender, &config));
  platform_test_set_capture_cleanup_result(ESY_OK);
  /* Faulted graphs require system recovery and cannot bypass retained owners. */
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_timer_capture_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_timer_capture_deinit(&owner));
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->capture_owner);
}
