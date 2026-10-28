/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_adc.h"
#include "esy_hal/esy_hal_adc_device.h"
#include "esy_hal/esy_hal_i2c.h"
#include "esy_hal/esy_hal_i2c_device.h"
#include "esy_hal/esy_hal_pwm.h"
#include "esy_hal/esy_hal_pwm_device.h"
#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_spi_device.h"
#include "esy_hal/esy_hal_timer_capture.h"
#include "esy_hal/esy_hal_timer_capture_device.h"
#include "esy_hal/esy_hal_uart.h"
#include "esy_hal/esy_hal_uart_device.h"
#include "esy_hal/esy_hal_usb_dev.h"
#include "esy_hal/esy_hal_usb_dev_device.h"
#include "esy_hal/xio/esy_hal_xio_spi.h"
#include "esy_hal/xio/esy_hal_xio_spi_device.h"
#include "esy_hal/xio/esy_hal_xio_uart.h"
#include "esy_hal/xio/esy_hal_xio_uart_device.h"

#include <string.h>

#include "hal_test_fakes.h"
#include "esy_hal_usb_dev_internal.h"
#include "esy_hal_adc_internal.h"
#include "esy_hal_i2c_internal.h"
#include "esy_hal_spi_internal.h"
#include "esy_hal_uart_internal.h"
#include "esy_hal_pwm_internal.h"
#include "esy_hal_timer_capture_internal.h"
#include "unity.h"

#define HAL_DEVICE(handle) \
  _Generic(&(handle), \
      esy_adc_t *: esy_hal_adc_runtime, \
      esy_i2c_t *: esy_hal_i2c_runtime, \
      esy_spi_t *: esy_hal_spi_runtime, \
      esy_uart_t *: esy_hal_uart_runtime, \
      esy_pwm_t *: esy_hal_pwm_runtime, \
      esy_timer_capture_t *: esy_hal_timer_capture_runtime, \
      esy_usb_dev_t *: esy_hal_usb_runtime \
  )(&(handle))->device

void setUp(void) { platform_test_reset(); platform_test_set_adc_busy(false); }

void tearDown(void) {}

static void init_registry(esy_device_registry_t *registry,
                          esy_device_registry_static_resources_t *resources) {
  esy_device_registry_config_t config = {.static_resources = resources};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_init(registry, &config));
}

static esy_device_registration_config_t device_config(const char *name) {
  esy_device_registration_config_t config = {
      .name = name,
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
  };
  return config;
}

static void assert_one_reference_then_complete(esy_device_t *device,
                                               esy_dma_config_t *dma) {
  uint32_t usage_count = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_usage_count(device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);
  TEST_ASSERT_NULL(dma->callback);
  esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  TEST_ASSERT_NOT_NULL(submitted.callback);
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_usage_count(device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(0u, usage_count);
}

static void count_adc_notification(void *arg) { ++*(uint32_t *)arg; }

void test_adc_adapter_holds_one_shot_dma_reference(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("adc");
  esy_adc_config_t config = {
      .phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES,
      .inner = {.channel = 2u},
  };
  esy_adc_device_binding_t binding = {0};
  esy_adc_t adc = {0};
  uint16_t sample = 0u;
  esy_dma_config_t dma = {.dst = &sample, .length = sizeof(sample)};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_init(&adc, &config));
  platform_test_set_adc_busy(true);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_adc_register_device(
      &binding, &adc, &registry, &registration, &config));
  TEST_ASSERT_NULL(HAL_DEVICE(adc));
  platform_test_set_adc_busy(false);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_adc_register_device(&binding, &adc, &registry,
                                               &registration, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_adc_init(&adc, &config));
  TEST_ASSERT_EQUAL_PTR(&binding.device, HAL_DEVICE(adc));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_set_trigger(
      &adc, ESY_ADC_TRIGGER_EDGE_RISING, ESY_ADC_TRIGGER_SRC_TIM3_TRGO));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_start_dma(&adc, &dma));
  assert_one_reference_then_complete(&binding.device, &dma);
  platform_test_set_adc_busy(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(1234, esy_hal_adc_sample_once(&adc));
  TEST_ASSERT_EQUAL(ESY_ADC_TRIGGER_EDGE_RISING,
                    platform_test_state()->last_adc_trigger_edge);
  TEST_ASSERT_EQUAL(ESY_ADC_TRIGGER_SRC_TIM3_TRGO,
                    platform_test_state()->last_adc_trigger_source);
}

void test_adc_partial_suspend_cleanup_faults_the_device_graph(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("adc-cleanup");
  esy_adc_config_t config = {.phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES, .inner = {.channel = 2u}};
  esy_adc_device_binding_t binding = {0};
  esy_adc_t adc = {0};
  esy_device_state_t state;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&adc, &config));
  platform_test_set_adc_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_register_device(
      &binding, &adc, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  platform_test_reset_order_counters();
  platform_test_set_adc_results(ESY_OK, ESY_OK, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_FAULTED, state);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
}

void test_adc_circular_lease_survives_callbacks_and_failed_abort(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("adc-loop");
  esy_adc_config_t config = {.phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES, .inner = {.channel = 2u}};
  esy_adc_device_binding_t binding = {0};
  esy_adc_t adc = {0};
  uint16_t samples[4] = {0};
  esy_dma_config_t dma = {.dst = samples, .length = sizeof(samples)};
  uint32_t usage;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&adc, &config));
  platform_test_set_adc_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_register_device(
      &binding, &adc, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_start_continuous_dma(&adc, &dma));
  TEST_ASSERT_FALSE(dma.circular);
  esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  TEST_ASSERT_TRUE(submitted.circular);
  submitted.callback(submitted.callback_arg);
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(1u, usage);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_adc_start_dma(&adc, &dma));
  platform_test_set_adc_results(ESY_OK, ESY_ERR_TIMEOUT, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_adc_abort_dma(&adc));
  TEST_ASSERT_TRUE(binding.dma_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(1u, usage);
  platform_test_set_adc_results(ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_abort_dma(&adc));
  TEST_ASSERT_FALSE(binding.dma_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
}

void test_adc_failed_start_keeps_lease_until_hardware_is_retired(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("adc-fail");
  esy_adc_config_t config = {.phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES, .inner = {.channel = 2u}};
  esy_adc_device_binding_t binding = {0};
  esy_adc_t adc = {0};
  uint16_t samples[2] = {0};
  esy_dma_config_t dma = {.dst = samples, .length = sizeof(samples)};
  uint32_t usage;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&adc, &config));
  platform_test_set_adc_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_register_device(
      &binding, &adc, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  platform_test_set_adc_busy(true);
  platform_test_set_adc_results(ESY_ERR_IO, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_adc_start_dma(&adc, &dma));
  TEST_ASSERT_TRUE(binding.dma_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(1u, usage);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_abort_dma(&adc));
  TEST_ASSERT_FALSE(binding.dma_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
}

void test_adc_terminal_error_releases_circular_lease_once(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("adc-error");
  esy_adc_config_t config = {.phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES, .inner = {.channel = 2u}};
  esy_adc_device_binding_t binding = {0};
  esy_adc_t adc = {0};
  uint16_t samples[2] = {0};
  uint32_t completions = 0u, errors = 0u, usage;
  esy_dma_config_t dma = {.dst = samples, .length = sizeof(samples),
      .callback = count_adc_notification, .callback_arg = &completions,
      .error_callback = count_adc_notification, .error_callback_arg = &errors};
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&adc, &config));
  platform_test_set_adc_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_register_device(
      &binding, &adc, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_start_continuous_dma(&adc, &dma));
  esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT32(1u, completions);
  submitted.error_callback(submitted.error_callback_arg);
  TEST_ASSERT_EQUAL_UINT32(1u, errors);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  submitted.error_callback(submitted.error_callback_arg);
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT32(1u, errors);
  TEST_ASSERT_EQUAL_UINT32(1u, completions);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_abort_dma(&adc));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
}

void test_i2c_adapter_holds_dma_reference(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("i2c");
  esy_i2c_config_t config = {
      .phys_index = TEST_I2C_INDEX,
      .speed_hz = 400000u,
      .addr_mode = ESY_I2C_ADDR_7BIT,
  };
  esy_i2c_device_binding_t binding = {0};
  esy_i2c_t i2c = {0};
  uint8_t data[2] = {0};
  esy_dma_config_t dma = {.src = data, .length = sizeof(data)};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_init(&i2c, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_i2c_register_master_device(&binding, &i2c, &registry,
                                                      &registration, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_i2c_write_dma(&i2c, 0x20u, &dma));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_set_speed(&i2c, 123000u));
  assert_one_reference_then_complete(&binding.device, &dma);
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT(ESY_BOARD_CONTROLLER_PIN_STATE_IDLE,
                        platform_test_state()->last_controller_pin_state);
  TEST_ASSERT_EQUAL_INT32(
      (int32_t)sizeof(data),
      esy_hal_i2c_write_blocking(&i2c, 0x20u, data, sizeof(data)));
  TEST_ASSERT_EQUAL_UINT32(123000u, platform_test_state()->resumed_speed_hz);
  dma.circular = true;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_i2c_write_dma(&i2c, 0x20u, &dma));
}

void test_i2c_suspend_keeps_controller_owner_and_ignores_foreign_dma(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("i2c-owner");
  const esy_i2c_config_t config = {.phys_index = TEST_I2C_INDEX,
      .speed_hz = 400000u, .addr_mode = ESY_I2C_ADDR_7BIT};
  esy_i2c_device_binding_t binding = {0};
  esy_i2c_t owner = {0};
  esy_i2c_t contender = {0};
  uint8_t data = 0x32u;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_register_master_device(
      &binding, &owner, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_i2c_deinit(&owner));
  platform_test_set_dma_busy(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_TRUE(binding.suspended);
  TEST_ASSERT_NOT_NULL(owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->i2c_owner);
  uint32_t configure_order = platform_test_state()->board_configure_order;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_i2c_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configure_order,
                          platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_INT32(1, esy_hal_i2c_write_blocking(&owner, 0x32u, &data, 1u));
  TEST_ASSERT_FALSE(binding.suspended);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&contender));
}

void test_i2c_partial_suspend_cleanup_faults_graph_and_keeps_native_owner(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("i2c-cleanup");
  const esy_i2c_config_t config = {.phys_index = TEST_I2C_INDEX};
  esy_i2c_device_binding_t binding = {0};
  esy_i2c_t owner = {0};
  esy_i2c_t contender = {0};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_register_master_device(
      &binding, &owner, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  platform_test_set_i2c_cleanup_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_FALSE(binding.suspended);
  TEST_ASSERT_NOT_NULL(owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->i2c_owner);
  TEST_ASSERT_EQUAL(ESY_BOARD_CONTROLLER_PIN_STATE_DEFAULT,
                    platform_test_state()->last_controller_pin_state);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_i2c_init(&contender, &config));
}

void test_i2c_failed_resume_keeps_controller_reserved(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("i2c-resume");
  const esy_i2c_config_t config = {.phys_index = TEST_I2C_INDEX};
  esy_i2c_device_binding_t binding = {0};
  esy_i2c_t owner = {0};
  esy_i2c_t contender = {0};
  uint8_t data = 0u;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_register_master_device(
      &binding, &owner, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  platform_test_set_init_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL,
                    esy_hal_i2c_write_blocking(&owner, 0x32u, &data, 1u));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_TRUE(binding.suspended);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->i2c_owner);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_i2c_init(&contender, &config));
}

void test_i2c_suspend_busy_rolls_back_pins_without_faulting_the_graph(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("i2c-busy");
  const esy_i2c_config_t config = {.phys_index = TEST_I2C_INDEX};
  esy_i2c_device_binding_t binding = {0};
  esy_i2c_t i2c = {0};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&i2c, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_register_master_device(
      &binding, &i2c, &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  platform_test_set_i2c_cleanup_result(ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
                    esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_FALSE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_FALSE(binding.suspended);
  TEST_ASSERT_EQUAL(ESY_BOARD_CONTROLLER_PIN_STATE_DEFAULT,
                    platform_test_state()->last_controller_pin_state);
  TEST_ASSERT_EQUAL_PTR(&i2c, platform_test_state()->i2c_owner);
  platform_test_set_i2c_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&i2c));
}

void test_uart_adapter_holds_tx_dma_reference(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("uart");
  esy_uart_config_t config = {
      .phys_index = TEST_UART_INDEX,
      .baudrate = 115200u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = ESY_UART_PARITY_NONE,
  };
  esy_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  uint8_t data[2] = {0};
  esy_dma_config_t dma = {.src = data, .length = sizeof(data)};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_init(&uart, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_uart_register_device(&binding, &uart, &registry,
                                                &registration));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_send_dma(&uart, &dma));
  assert_one_reference_then_complete(&binding.device, &dma);
  platform_test_set_dma_busy(false);
  platform_test_set_pin_state_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_device_registry_runtime_suspend_idle(&registry));
  esy_device_state_t state;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  platform_test_set_pin_state_result(ESY_OK);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT(ESY_BOARD_CONTROLLER_PIN_STATE_IDLE,
                        platform_test_state()->last_controller_pin_state);
  esy_uart_t contender = {0};
  uint32_t configure_order = platform_test_state()->board_configure_order;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configure_order,
                          platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_PTR(&uart, platform_test_state()->uart_owner);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_deinit(&uart));
  TEST_ASSERT_EQUAL_INT32(
      (int32_t)sizeof(data),
      esy_hal_uart_send_blocking(&uart, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_uart_suspend(&uart));
  platform_test_set_pin_state_result(ESY_ERR_NOT_SUPPORTED);
  platform_test_set_uart_resume_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_FAULTED, state);
}

void test_uart_partial_suspend_failure_faults_graph_and_preserves_claim(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("uart-cleanup");
  esy_uart_config_t config = {.phys_index = TEST_UART_INDEX,
      .baudrate = 115200u, .data_bits = 8u, .stop_bits = 1u};
  esy_uart_device_binding_t binding = {0};
  esy_uart_t owner = {0};
  esy_uart_t contender = {0};
  esy_device_state_t state;
  uint8_t byte = 0u;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_register_device(
      &binding, &owner, &registry, &registration));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  void *context = owner.ctx[0];
  uint32_t configure_order = platform_test_state()->board_configure_order;
  platform_test_reset_order_counters();
  platform_test_set_uart_suspend_result(ESY_ERR_STATE);

  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_FAULTED, state);
  TEST_ASSERT_FALSE(binding.suspended);
  TEST_ASSERT_FALSE(platform_test_state()->uart_enabled);
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx[0]);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->uart_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_pin_state_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_send_blocking(&owner, &byte, 1u));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configure_order,
                          platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_deinit(&owner));
}

void test_pwm_adapter_holds_one_shot_dma_reference(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("pwm");
  esy_pwm_config_t config = {
      .phys_index = TEST_PWM_INDEX,
      .freq_hz = 1000u,
      .duty_cycle = 50u,
  };
  esy_pwm_device_binding_t binding = {0};
  esy_pwm_t pwm = {0};
  uint16_t values[2] = {0};
  esy_dma_config_t dma = {.src = values, .length = sizeof(values)};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_init(&pwm, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_enable(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_pwm_register_device(&binding, &pwm, &registry,
                                               &registration));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_TRUE(binding.output_enabled);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, false));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_pwm_dma_output(&pwm, &dma, true));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_abort_dma(&pwm));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_pwm_dma_output(&pwm, &dma, false));
  assert_one_reference_then_complete(&binding.device, &dma);
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  esy_device_state_t state;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_TRUE(binding.output_enabled);
  TEST_ASSERT_TRUE(esy_hal_pwm_runtime(&pwm)->output_enabled);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_hal_pwm_set_duty(&pwm, 0.5f));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, true));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_set_duty(&pwm, 0.5f));
}

void test_typed_adapters_can_unregister_before_registry_start(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 4u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("adapter");
  esy_adc_config_t adc_config = {
      .phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES,
      .inner = {.channel = 2u},
  };
  esy_i2c_config_t i2c_config = {
      .phys_index = TEST_I2C_INDEX,
      .speed_hz = 400000u,
      .addr_mode = ESY_I2C_ADDR_7BIT,
  };
  esy_uart_config_t uart_config = {
      .phys_index = TEST_UART_INDEX,
      .baudrate = 115200u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = ESY_UART_PARITY_NONE,
  };
  esy_pwm_config_t pwm_config = {
      .phys_index = TEST_PWM_INDEX,
      .freq_hz = 1000u,
      .duty_cycle = 50u,
  };
  esy_adc_device_binding_t adc_binding = {0};
  esy_i2c_device_binding_t i2c_binding = {0};
  esy_uart_device_binding_t uart_binding = {0};
  esy_pwm_device_binding_t pwm_binding = {0};
  esy_adc_t adc = {0};
  esy_i2c_t i2c = {0};
  esy_uart_t uart = {0};
  esy_pwm_t pwm = {0};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_init(&adc, &adc_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_init(&i2c, &i2c_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_init(&uart, &uart_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_pwm_init(&pwm, &pwm_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_adc_register_device(&adc_binding, &adc, &registry,
                                               &registration, &adc_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_i2c_register_master_device(
                  &i2c_binding, &i2c, &registry, &registration, &i2c_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_uart_register_device(&uart_binding, &uart, &registry,
                                                &registration));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_pwm_register_device(&pwm_binding, &pwm, &registry,
                                               &registration));
  platform_test_set_adc_busy(false);
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_pwm_unregister_device(&pwm_binding));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_uart_unregister_device(&uart_binding));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_i2c_unregister_device(&i2c_binding));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_adc_unregister_device(&adc_binding));
  TEST_ASSERT_NULL(HAL_DEVICE(adc));
  TEST_ASSERT_NULL(HAL_DEVICE(i2c));
  TEST_ASSERT_NULL(HAL_DEVICE(uart));
  TEST_ASSERT_NULL(HAL_DEVICE(pwm));
}

void test_typed_adapters_unbind_after_started_registry_teardown_and_reinit(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 6u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("adapter");
  esy_device_registration_config_t usb_registration = device_config("usb");
  esy_adc_config_t adc_config = {.phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES, .inner = {.channel = 2u}};
  esy_i2c_config_t i2c_config = {.phys_index = TEST_I2C_INDEX,
      .speed_hz = 400000u, .addr_mode = ESY_I2C_ADDR_7BIT};
  esy_uart_config_t uart_config = {.phys_index = TEST_UART_INDEX,
      .baudrate = 115200u, .data_bits = 8u, .stop_bits = 1u};
  esy_spi_config_t spi_config = {.phys_index = TEST_SPI_INDEX,
      .role = ESY_SPI_ROLE_MASTER, .mode = ESY_SPI_MODE_0,
      .speed_hz = 1000000u, .data_bits = 8u, .is_msb_first = true};
  esy_timer_capture_config_t capture_config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX, .rising_edge = true,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u}};
  esy_usb_dev_config_t usb_config = {.phys_index = TEST_USB_INDEX};
  esy_adc_device_binding_t adc_binding = {0};
  esy_i2c_device_binding_t i2c_binding = {0};
  esy_uart_device_binding_t uart_binding = {0};
  esy_spi_device_binding_t spi_binding = {0};
  esy_timer_capture_device_binding_t capture_binding = {0};
  esy_usb_dev_device_binding_t usb_binding = {0};
  esy_adc_t adc = {0};
  esy_i2c_t i2c = {0};
  esy_uart_t uart = {0};
  esy_spi_t spi = {0};
  esy_timer_capture_t capture = {0};
  esy_usb_dev_t usb = {0};
  uint16_t samples[2] = {0};
  esy_dma_config_t dma = {.dst = samples, .length = sizeof(samples)};

  usb_registration.supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE);
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  platform_test_set_dma_busy(false);
  for (unsigned cycle = 0u; cycle < 2u; ++cycle) {
    init_registry(&registry, &resources);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&adc, &adc_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&i2c, &i2c_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&uart, &uart_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&spi, &spi_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&capture, &capture_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_usb_dev_init(&usb, &usb_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_register_device(&adc_binding,
        &adc, &registry, &registration, &adc_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_register_master_device(&i2c_binding,
        &i2c, &registry, &registration, &i2c_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_register_device(&uart_binding,
        &uart, &registry, &registration));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_register_device(&spi_binding,
        &spi, &registry, &registration, &spi_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_register_device(&capture_binding,
        &capture, &registry, &registration, &capture_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_usb_dev_register_device(&usb_binding,
        &usb, &registry, &usb_registration));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_start_continuous_dma(&adc, &dma));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_device_registry_deinit(&registry));
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_hal_adc_unregister_device(&adc_binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_abort_dma(&adc));
    /* No UART stream, capture callback or USB transfer was armed. */
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    TEST_ASSERT_EQUAL_PTR(&adc_binding.device, HAL_DEVICE(adc));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_unregister_device(&adc_binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_unregister_device(&i2c_binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_unregister_device(&uart_binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&spi_binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_unregister_device(&capture_binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_usb_dev_unregister_device(&usb_binding));
    TEST_ASSERT_NULL(HAL_DEVICE(adc));
    TEST_ASSERT_NULL(HAL_DEVICE(i2c));
    TEST_ASSERT_NULL(HAL_DEVICE(uart));
    TEST_ASSERT_NULL(HAL_DEVICE(spi));
    TEST_ASSERT_NULL(HAL_DEVICE(capture));
    TEST_ASSERT_NULL(HAL_DEVICE(usb));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&adc));
    esy_hal_i2c_deinit(&i2c);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&uart));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_deinit(&capture));
    esy_hal_usb_dev_deinit(&usb);
    TEST_ASSERT_NULL(adc.ctx);
    TEST_ASSERT_NULL(i2c.ctx);
    TEST_ASSERT_NULL(uart.ctx[0]);
    TEST_ASSERT_NULL(spi.ctx);
    TEST_ASSERT_NULL(capture.ctx);
    TEST_ASSERT_NULL(usb.ctx);
  }
}

void test_xio_adapters_unbind_after_registry_teardown_and_reinit(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("xio");
  esy_xio_uart_config_t tx = {.pin = TEST_XIO_TX_PIN,
      .baudrate = 115200u, .instance = 0u, .sm_index = 1u};
  esy_xio_uart_config_t rx = {.pin = TEST_XIO_RX_PIN,
      .baudrate = 115200u, .instance = 0u, .sm_index = 2u};
  esy_xio_spi_config_t spi_config = {.pin_sck = TEST_XIO_SCK_PIN,
      .pin_mosi = TEST_XIO_TX_PIN, .pin_miso = TEST_XIO_RX_PIN,
      .role = ESY_SPI_ROLE_MASTER, .mode = ESY_SPI_MODE_0,
      .is_msb_first = true, .data_bits = 8u, .speed_hz = 1000000u,
      .instance = 0u, .sm_index = 1u};
  esy_xio_uart_device_binding_t uart_binding = {0};
  esy_spi_device_binding_t spi_binding = {0};
  esy_uart_t uart = {0};
  esy_spi_t spi = {0};
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  platform_test_set_dma_busy(false);
  for (unsigned cycle = 0u; cycle < 2u; ++cycle) {
    init_registry(&registry, &resources);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, &tx, &rx));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_init(&spi, &spi_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_register_device(&uart_binding,
        &uart, &registry, &registration, &tx, &rx));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_register_device(&spi_binding,
        &spi, &registry, &registration, &spi_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_unregister_device(&uart_binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&spi_binding));
    TEST_ASSERT_NULL(HAL_DEVICE(uart));
    TEST_ASSERT_NULL(HAL_DEVICE(spi));
    esy_hal_xio_uart_deinit(&uart);
    esy_hal_xio_spi_deinit(&spi);
    TEST_ASSERT_NULL(uart.ctx[0]);
    TEST_ASSERT_NULL(uart.ctx[1]);
    TEST_ASSERT_NULL(spi.ctx);
  }
}

void test_live_adc_binding_reuse_preserves_owner_across_registry_lifecycle(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("adc-owner");
  esy_adc_config_t config = {.phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES, .inner = {.channel = 2u}};
  esy_adc_config_t contender_config = config;
  esy_adc_device_binding_t binding = {0};
  esy_adc_device_binding_t before;
  esy_adc_t owner = {0};
  esy_adc_t contender = {0};
  esy_device_state_t state;

  contender_config.phys_index = TEST_ADC_INDEX + 1u;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&contender, &contender_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_register_device(
      &binding, &owner, &registry, &registration, &config));

  for (unsigned phase = 0u; phase < 3u; ++phase) {
    if (phase == 1u) {
      TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
    } else if (phase == 2u) {
      TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    }
    memcpy(&before, &binding, sizeof(before));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_adc_register_device(
        &binding, &contender, &registry, &registration, &contender_config));
    TEST_ASSERT_EQUAL_MEMORY(&before, &binding, sizeof(binding));
    TEST_ASSERT_EQUAL_PTR(&binding.device, HAL_DEVICE(owner));
    TEST_ASSERT_NULL(HAL_DEVICE(contender));
    if (phase == 1u) {
      TEST_ASSERT_EQUAL(1234, esy_hal_adc_sample_once(&owner));
      TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
      TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
    }
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_unregister_device(&binding));
  TEST_ASSERT_NULL(HAL_DEVICE(owner));
  init_registry(&registry, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_register_device(
      &binding, &contender, &registry, &registration, &contender_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&contender));
}

#define ASSERT_REBIND_PRESERVES(binding, owner, candidate, node, call) do { \
  unsigned char before[sizeof(binding)]; \
  memcpy(before, &(binding), sizeof(binding)); \
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, (call)); \
  TEST_ASSERT_EQUAL_MEMORY(before, &(binding), sizeof(binding)); \
  TEST_ASSERT_EQUAL_PTR((node), HAL_DEVICE(owner)); \
  TEST_ASSERT_NULL(HAL_DEVICE(candidate)); \
} while (0)

void test_all_typed_adapters_preserve_live_bindings_on_other_handle_rebind(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 9u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = device_config("bound-owner");
  esy_adc_config_t adc_config = {.phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES, .inner = {.channel = 2u}};
  esy_i2c_config_t i2c_config = {.phys_index = TEST_I2C_INDEX,
      .speed_hz = 400000u, .addr_mode = ESY_I2C_ADDR_7BIT};
  esy_i2c_slave_config_t slave_config = {.phys_index = TEST_I2C_INDEX,
      .addr_mode = ESY_I2C_ADDR_7BIT, .address = 0x20u};
  esy_spi_config_t spi_config = {.phys_index = TEST_SPI_INDEX,
      .role = ESY_SPI_ROLE_MASTER, .mode = ESY_SPI_MODE_0,
      .speed_hz = 1000000u, .data_bits = 8u, .is_msb_first = true};
  esy_uart_config_t uart_config = {.phys_index = TEST_UART_INDEX,
      .baudrate = 115200u, .data_bits = 8u, .stop_bits = 1u};
  esy_pwm_config_t pwm_config = {.phys_index = TEST_PWM_INDEX,
      .freq_hz = 1000u, .duty_cycle = 50u};
  esy_timer_capture_config_t capture_config = {
      .phys_index = TEST_TIMER_CAPTURE_INDEX, .rising_edge = true,
      .inner = {.gpio = TEST_GPIO_PIN, .channel = 2u, .tim_id = 1u}};
  esy_usb_dev_config_t usb_config = {.phys_index = TEST_USB_INDEX};
  esy_xio_spi_config_t xio_config = {.pin_sck = TEST_XIO_SCK_PIN,
      .pin_mosi = TEST_XIO_TX_PIN, .pin_miso = TEST_XIO_RX_PIN,
      .role = ESY_SPI_ROLE_MASTER, .mode = ESY_SPI_MODE_0,
      .is_msb_first = true, .data_bits = 8u, .speed_hz = 1000000u,
      .instance = 0u, .sm_index = 1u};
  esy_xio_uart_config_t tx = {.pin = TEST_XIO_TX_PIN,
      .baudrate = 115200u, .instance = 0u, .sm_index = 1u};
  esy_adc_device_binding_t adc_binding = {0};
  esy_i2c_device_binding_t i2c_binding = {0};
  esy_spi_device_binding_t spi_binding = {0}, xio_spi_binding = {0};
  esy_uart_device_binding_t uart_binding = {0};
  esy_pwm_device_binding_t pwm_binding = {0};
  esy_timer_capture_device_binding_t capture_binding = {0};
  esy_usb_dev_device_binding_t usb_binding = {0};
  esy_xio_uart_device_binding_t xio_uart_binding = {0};
  esy_adc_t adc = {0}, adc_candidate = {0};
  esy_i2c_t i2c = {0}, i2c_candidate = {0};
  esy_spi_t spi = {0}, spi_candidate = {0};
  esy_spi_t xio_spi = {0}, xio_spi_candidate = {0};
  esy_uart_t uart = {0}, uart_candidate = {0};
  esy_uart_t xio_uart = {0}, xio_uart_candidate = {0};
  esy_pwm_t pwm = {0}, pwm_candidate = {0};
  esy_timer_capture_t capture = {0}, capture_candidate = {0};
  esy_usb_dev_t usb = {0}, usb_candidate = {0};
  uint8_t byte = 0x42u;
  uint32_t value;

  registration.supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE);
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  init_registry(&registry, &resources);
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&adc, &adc_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&i2c, &i2c_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&spi, &spi_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&uart, &uart_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_init(&pwm, &pwm_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_init(&capture, &capture_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_usb_dev_init(&usb, &usb_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_init(&xio_spi, &xio_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&xio_uart, &tx, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_register_device(&adc_binding,
      &adc, &registry, &registration, &adc_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_register_master_device(&i2c_binding,
      &i2c, &registry, &registration, &i2c_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_register_device(&spi_binding,
      &spi, &registry, &registration, &spi_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_register_device(&uart_binding,
      &uart, &registry, &registration));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_register_device(&pwm_binding,
      &pwm, &registry, &registration));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_register_device(&capture_binding,
      &capture, &registry, &registration, &capture_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_usb_dev_register_device(&usb_binding,
      &usb, &registry, &registration));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_register_device(&xio_spi_binding,
      &xio_spi, &registry, &registration, &xio_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_register_device(&xio_uart_binding,
      &xio_uart, &registry, &registration, &tx, NULL));

  /* Model independent facade candidates without extending the singleton native
   * fake. Rejection must precede any candidate hardware access. The separate ADC
   * regression also initializes two controllers through the complete HAL path. */
  adc_candidate.ops = adc.ops;
  i2c_candidate.ops = i2c.ops;
  spi_candidate.ops = spi.ops;
  uart_candidate.ops = uart.ops;
  pwm_candidate.ops = pwm.ops;
  capture_candidate.ops = capture.ops;
  capture_candidate.ctx = capture_candidate.backend.bytes;
  esy_hal_timer_capture_runtime(&capture_candidate)->pins_configured = true;
  usb_candidate.ops = usb.ops;
  xio_spi_candidate.ops = xio_spi.ops;
  xio_uart_candidate.ops = xio_uart.ops;
  for (unsigned phase = 0u; phase < 3u; ++phase) {
    if (phase == 1u) TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
    if (phase == 2u) TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    ASSERT_REBIND_PRESERVES(adc_binding, adc, adc_candidate, &adc_binding.device,
        esy_hal_adc_register_device(&adc_binding, &adc_candidate, &registry,
                                   &registration, &adc_config));
    ASSERT_REBIND_PRESERVES(i2c_binding, i2c, i2c_candidate, &i2c_binding.device,
        esy_hal_i2c_register_master_device(&i2c_binding, &i2c_candidate, &registry,
                                          &registration, &i2c_config));
    ASSERT_REBIND_PRESERVES(i2c_binding, i2c, i2c_candidate, &i2c_binding.device,
        esy_hal_i2c_register_slave_device(&i2c_binding, &i2c_candidate, &registry,
                                         &registration, &slave_config));
    ASSERT_REBIND_PRESERVES(spi_binding, spi, spi_candidate, &spi_binding.device,
        esy_hal_spi_register_device(&spi_binding, &spi_candidate, &registry,
                                   &registration, &spi_config));
    ASSERT_REBIND_PRESERVES(uart_binding, uart, uart_candidate, &uart_binding.device,
        esy_hal_uart_register_device(&uart_binding, &uart_candidate, &registry,
                                    &registration));
    ASSERT_REBIND_PRESERVES(pwm_binding, pwm, pwm_candidate, &pwm_binding.device,
        esy_hal_pwm_register_device(&pwm_binding, &pwm_candidate, &registry,
                                   &registration));
    ASSERT_REBIND_PRESERVES(capture_binding, capture, capture_candidate,
        &capture_binding.device,
        esy_hal_timer_capture_register_device(&capture_binding, &capture_candidate,
            &registry, &registration, &capture_config));
    ASSERT_REBIND_PRESERVES(usb_binding, usb, usb_candidate, &usb_binding.device,
        esy_hal_usb_dev_register_device(&usb_binding, &usb_candidate, &registry,
                                       &registration));
    ASSERT_REBIND_PRESERVES(xio_spi_binding, xio_spi, xio_spi_candidate,
        &xio_spi_binding.device,
        esy_hal_xio_spi_register_device(&xio_spi_binding, &xio_spi_candidate,
            &registry, &registration, &xio_config));
    ASSERT_REBIND_PRESERVES(xio_uart_binding, xio_uart, xio_uart_candidate,
        &xio_uart_binding.base.device,
        esy_hal_xio_uart_register_device(&xio_uart_binding, &xio_uart_candidate,
            &registry, &registration, &tx, NULL));
    if (phase == 1u) {
      TEST_ASSERT_EQUAL(1234, esy_hal_adc_sample_once(&adc));
      TEST_ASSERT_EQUAL(1, esy_hal_i2c_write_blocking(&i2c, 0x20u, &byte, 1u));
      TEST_ASSERT_EQUAL(1, esy_hal_spi_write_blocking(&spi, &byte, 1u));
      TEST_ASSERT_EQUAL(1, esy_hal_uart_send_blocking(&uart, &byte, 1u));
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, true));
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_set_duty(&pwm, 0.25f));
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_enable(&pwm, false));
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_get_value(&capture, &value));
      TEST_ASSERT_EQUAL_UINT32(100u, value);
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_usb_dev_get_caps(&usb, &value));
      TEST_ASSERT_EQUAL(1, esy_hal_spi_write_blocking(&xio_spi, &byte, 1u));
      TEST_ASSERT_EQUAL(1, esy_hal_uart_send_blocking(&xio_uart, &byte, 1u));
    }
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_unregister_device(&adc_binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_unregister_device(&i2c_binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&spi_binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_unregister_device(&uart_binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_unregister_device(&pwm_binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_timer_capture_unregister_device(&capture_binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_usb_dev_unregister_device(&usb_binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&xio_spi_binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_unregister_device(&xio_uart_binding));
}

#undef ASSERT_REBIND_PRESERVES
