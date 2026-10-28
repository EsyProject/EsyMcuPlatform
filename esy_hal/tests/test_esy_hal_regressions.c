/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#include <string.h>
#include "unity.h"
#include "hal_test_fakes.h"
#include "esy_hal/esy_hal_uart.h"
#include "esy_hal/esy_hal_uart_device.h"
#include "esy_hal/esy_hal_adc.h"
#include "esy_hal/esy_hal_i2c.h"
#include "esy_hal/esy_hal_pwm.h"
#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_timer_capture.h"
#include "esy_hal/esy_hal_usb_dev.h"
#include "esy_hal/xio/esy_hal_xio_uart.h"
#include "esy_hal/xio/esy_hal_xio_uart_device.h"

void setUp(void) { platform_test_reset(); }
void tearDown(void) {}

#define CHECK_LIFECYCLE_CONFIG(module, type, config_type, controller_index, configure) do { \
  type handle = {0}; config_type config; \
  esy_hal_##module##_default_cfg(&config); \
  config.phys_index = controller_index; \
  configure; \
  const config_type original = config; \
  platform_test_set_init_result(ESY_ERR_FAIL); \
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_##module##_init(&handle, &config)); \
  TEST_ASSERT_EQUAL_MEMORY(&original, &config, sizeof(config)); \
  platform_test_set_init_result(ESY_OK); \
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_##module##_init(&handle, &config)); \
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_##module##_init(&handle, &config)); \
  esy_hal_##module##_deinit(&handle); \
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_##module##_init(&handle, &config)); \
  esy_hal_##module##_deinit(&handle); \
} while (0)

#define CHECK_LIFECYCLE(module, type, config_type, controller_index) \
  CHECK_LIFECYCLE_CONFIG(module, type, config_type, controller_index, (void)0)

void test_handles_reject_reinit_and_recover_from_failed_init(void) {
  CHECK_LIFECYCLE(i2c, esy_i2c_t, esy_i2c_config_t, TEST_I2C_INDEX);
  CHECK_LIFECYCLE(uart, esy_uart_t, esy_uart_config_t, TEST_UART_INDEX);
  CHECK_LIFECYCLE(spi, esy_spi_t, esy_spi_config_t, TEST_SPI_INDEX);
  CHECK_LIFECYCLE(adc, esy_adc_t, esy_adc_config_t, TEST_ADC_INDEX);
  CHECK_LIFECYCLE(pwm, esy_pwm_t, esy_pwm_config_t, TEST_PWM_INDEX);
  CHECK_LIFECYCLE_CONFIG(timer_capture, esy_timer_capture_t,
                        esy_timer_capture_config_t, TEST_TIMER_CAPTURE_INDEX,
                        config.inner.gpio = TEST_GPIO_PIN);
  CHECK_LIFECYCLE(usb_dev, esy_usb_dev_t, esy_usb_dev_config_t, TEST_USB_INDEX);
}

#define CHECK_DEFAULT(module, type) do { \
  type first, second; \
  memset(&first, 0xa5, sizeof(first)); \
  memset(&second, 0x5a, sizeof(second)); \
  esy_hal_##module##_default_cfg(&first); \
  esy_hal_##module##_default_cfg(&second); \
  TEST_ASSERT_EQUAL_MEMORY(&first, &second, sizeof(first)); \
} while (0)

void test_defaults_initialize_every_byte_from_uninitialized_storage(void) {
  CHECK_DEFAULT(adc, esy_adc_config_t);
  CHECK_DEFAULT(pwm, esy_pwm_config_t);
  CHECK_DEFAULT(i2c, esy_i2c_config_t);
  CHECK_DEFAULT(spi, esy_spi_config_t);
  CHECK_DEFAULT(uart, esy_uart_config_t);
  CHECK_DEFAULT(timer_capture, esy_timer_capture_config_t);
  CHECK_DEFAULT(usb_dev, esy_usb_dev_config_t);
}

void test_xio_uart_single_direction_and_partial_failure_are_reusable(void) {
  esy_uart_t uart = {0};
  esy_xio_uart_config_t tx = {.pin = TEST_XIO_TX_PIN};
  esy_xio_uart_config_t rx = {.pin = TEST_XIO_RX_PIN};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, &tx, NULL));
  TEST_ASSERT_NULL(uart.ctx[1]);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_xio_uart_init(&uart, NULL, &rx));
  esy_hal_xio_uart_deinit(&uart);
  platform_test_set_xio_rx_init_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_xio_uart_init(&uart, &tx, &rx));
  TEST_ASSERT_NULL(uart.ctx[0]);
  TEST_ASSERT_NULL(uart.ctx[1]);
  TEST_ASSERT_NULL(uart.ops);
  platform_test_set_xio_rx_init_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, NULL, &rx));
  TEST_ASSERT_NULL(uart.ctx[0]);
  esy_hal_xio_uart_deinit(&uart);
}

static void count_completion(void *arg) { (*(unsigned *)arg)++; }

void test_registered_xio_uart_generic_dma_keeps_callback_and_circular_reference(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t registry_config = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "xio", .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)};
  esy_xio_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  esy_xio_uart_config_t tx = {.pin = TEST_XIO_TX_PIN};
  unsigned completions = 0;
  uint8_t bytes[4] = {0};
  esy_dma_config_t dma = {.src = bytes, .length = sizeof(bytes),
      .circular = true, .callback = count_completion, .callback_arg = &completions};
  esy_dma_config_t original = dma;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, &tx, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_register_device(
      &binding, &uart, &registry, &registration, &tx, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_send_dma(&uart, &dma));
  TEST_ASSERT_EQUAL_MEMORY(&original, &dma, sizeof(dma));
  esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  submitted.callback(submitted.callback_arg);
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT(2, completions);
  uint32_t refs;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.base.device, &refs));
  TEST_ASSERT_EQUAL_UINT(1, refs);
  platform_test_set_dma_busy(false);
  TEST_ASSERT_TRUE(esy_hal_xio_uart_is_busy(&uart, true));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_send_dma(&uart, &dma));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_abort_dma(&uart, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.base.device, &refs));
  TEST_ASSERT_EQUAL_UINT(0, refs);
  dma.circular = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_send_dma(&uart, &dma));
  submitted = platform_test_state()->submitted_dma;
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT(3, completions);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.base.device, &refs));
  TEST_ASSERT_EQUAL_UINT(0, refs);
}

void test_dma_routing_failure_never_reaches_a_driver(void) {
  esy_uart_t uart = {0}; esy_uart_config_t uc;
  esy_adc_t adc = {0}; esy_adc_config_t ac;
  esy_pwm_t pwm = {0}; esy_pwm_config_t pc;
  esy_i2c_t i2c = {0}; esy_i2c_config_t ic;
  esy_hal_uart_default_cfg(&uc); esy_hal_adc_default_cfg(&ac);
  esy_hal_pwm_default_cfg(&pc); esy_hal_i2c_default_cfg(&ic);
  ic.phys_index = TEST_I2C_INDEX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&uart, &uc));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&adc, &ac));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_pwm_init(&pwm, &pc));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&i2c, &ic));
  uint32_t data = 0;
  esy_dma_config_t dma = {.src = &data, .dst = &data, .length = sizeof(data)};
  esy_dma_config_t original = dma;
  platform_test_reject_dma_mapping(true);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_uart_send_dma(&uart, &dma));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_uart_receive_dma(&uart, &dma));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_adc_start_dma(&adc, &dma));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_pwm_dma_output(&pwm, &dma, false));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_i2c_write_dma(&i2c, 0x50, &dma));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_i2c_read_dma(&i2c, 0x50, &dma));
  TEST_ASSERT_EQUAL_MEMORY(&original, &dma, sizeof(dma));
  TEST_ASSERT_EQUAL_UINT(0, platform_test_state()->driver_transfer_dma_calls);
}
