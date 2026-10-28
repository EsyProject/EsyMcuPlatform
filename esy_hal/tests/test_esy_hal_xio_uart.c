/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/xio/esy_hal_xio_uart.h"
#include "esy_hal/xio/esy_hal_xio_uart_device.h"

#include "esy_hal/esy_hal_uart.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) {
  platform_test_reset();
  platform_test_set_dma_busy(true);
}

void tearDown(void) {}

static void init_xio_uart(esy_uart_t *uart) {
  esy_xio_uart_config_t tx_cfg = {
      .pin = TEST_XIO_TX_PIN,
      .baudrate = 115200u,
      .instance = 0u,
      .sm_index = 1u,
  };
  esy_xio_uart_config_t rx_cfg = {
      .pin = TEST_XIO_RX_PIN,
      .baudrate = 115200u,
      .instance = 0u,
      .sm_index = 2u,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_xio_uart_init(uart, &tx_cfg, &rx_cfg));
}

void test_esy_hal_xio_uart_binds_selected_tx_rx_ops(void) {
  esy_uart_t uart = {0};
  uint8_t data[3] = {1u, 2u, 3u};

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_xio_uart_init(NULL, NULL, NULL));
  init_xio_uart(&uart);

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(2u, state->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT8(TEST_XIO_RX_PIN, state->last_xio_pin);
  TEST_ASSERT_EQUAL_UINT8(0u, state->last_xio_config.instance);
  TEST_ASSERT_EQUAL_UINT8(2u, state->last_xio_config.sm_index);
  TEST_ASSERT_NOT_NULL(uart.ops->send_blocking);
  TEST_ASSERT_NOT_NULL(uart.ops->receive_blocking);
  TEST_ASSERT_NOT_NULL(uart.ops->send_dma);
  TEST_ASSERT_NOT_NULL(uart.ops->receive_dma);
  TEST_ASSERT_EQUAL_INT32(3, uart.ops->send_blocking(&uart, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(3,
                          uart.ops->receive_blocking(&uart, data, sizeof(data)));
}

void test_esy_hal_xio_uart_dma_completes_xio_board_metadata(void) {
  esy_uart_t uart = {0};
  uint8_t tx_buf[4] = {0};
  uint8_t rx_buf[4] = {0};
  esy_dma_config_t tx_dma = {
      .src = tx_buf,
      .length = sizeof(tx_buf),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  esy_dma_config_t rx_dma = {
      .dst = rx_buf,
      .length = sizeof(rx_buf),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };

  init_xio_uart(&uart);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, uart.ops->send_dma(&uart, &tx_dma));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, uart.ops->receive_dma(&uart, &rx_dma));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(2u, state->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL_UINT32(0x700u + TEST_XIO_TX_PIN, state->last_tx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_XIO_TX_PIN & 0x0Fu,
                          state->last_tx_inner.channel);
  TEST_ASSERT_EQUAL_UINT32(0x700u + TEST_XIO_RX_PIN, state->last_rx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_XIO_RX_PIN & 0x0Fu,
                          state->last_rx_inner.channel);
  TEST_ASSERT_FALSE(esy_hal_xio_uart_is_busy(&uart, true));
}

void test_xio_readability_uses_rx_backend_for_each_direction_assembly(void) {
  esy_uart_t uart = {0};
  esy_xio_uart_config_t config = {.pin = TEST_XIO_RX_PIN,
      .baudrate = 115200u, .instance = 0u, .sm_index = 2u};
  TEST_ASSERT_FALSE(esy_hal_uart_is_readable(&uart));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, &config, NULL));
  platform_test_set_xio_uart_readable(true);
  TEST_ASSERT_FALSE(esy_hal_uart_is_readable(&uart));
  esy_hal_xio_uart_deinit(&uart);

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, NULL, &config));
  TEST_ASSERT_TRUE(esy_hal_uart_is_readable(&uart));
  platform_test_set_xio_uart_readable(false);
  TEST_ASSERT_FALSE(esy_hal_uart_is_readable(&uart));
  esy_hal_xio_uart_deinit(&uart);
  TEST_ASSERT_FALSE(esy_hal_uart_is_readable(&uart));

  init_xio_uart(&uart);
  TEST_ASSERT_FALSE(esy_hal_uart_is_readable(&uart));
  platform_test_set_xio_uart_readable(true);
  TEST_ASSERT_TRUE(esy_hal_uart_is_readable(&uart));
  esy_hal_xio_uart_deinit(&uart);
  TEST_ASSERT_FALSE(esy_hal_uart_is_readable(&uart));
}

void test_esy_hal_xio_uart_default_tx_only_rx_only_and_deinit_paths(void) {
  esy_xio_uart_config_t cfg = {0};
  esy_xio_uart_config_t tx_cfg = {
      .pin = TEST_XIO_TX_PIN,
      .baudrate = 9600u,
      .instance = 0u,
      .sm_index = 1u,
  };
  esy_xio_uart_config_t rx_cfg = {
      .pin = TEST_XIO_RX_PIN,
      .baudrate = 9600u,
      .instance = 0u,
      .sm_index = 2u,
  };
  esy_uart_t uart = {0};

  esy_hal_xio_uart_default_cfg(NULL);
  esy_hal_xio_uart_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_UINT8(ESY_PIN_UNUSED, cfg.pin);
  TEST_ASSERT_EQUAL_UINT32(115200u, cfg.baudrate);
  TEST_ASSERT_EQUAL_UINT8(0xFFu, cfg.instance);
  TEST_ASSERT_EQUAL_UINT8(0xFFu, cfg.sm_index);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_xio_uart_init(&uart, &tx_cfg, NULL));
  TEST_ASSERT_NOT_NULL(uart.ops->send_blocking);
  TEST_ASSERT_NULL(uart.ops->receive_blocking);
  TEST_ASSERT_FALSE(esy_hal_xio_uart_is_busy(NULL, true));
  esy_hal_xio_uart_deinit(&uart);

  platform_test_reset();
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_xio_uart_init(&uart, NULL, &rx_cfg));
  TEST_ASSERT_NULL(uart.ops->send_blocking);
  TEST_ASSERT_NOT_NULL(uart.ops->receive_blocking);
  TEST_ASSERT_FALSE(esy_hal_xio_uart_is_busy(&uart, false));
  esy_hal_xio_uart_deinit(&uart);
  TEST_ASSERT_NOT_EQUAL(0u, platform_test_state()->driver_deinit_order);
}

void test_esy_hal_xio_uart_device_runtime_suspend_and_lazy_resume(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config = {0};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "xio-uart",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
  };
  esy_xio_uart_config_t tx_config = {
      .pin = TEST_XIO_TX_PIN,
      .baudrate = 115200u,
      .instance = 0u,
      .sm_index = 1u,
  };
  esy_xio_uart_config_t rx_config = {
      .pin = TEST_XIO_RX_PIN,
      .baudrate = 115200u,
      .instance = 0u,
      .sm_index = 2u,
  };
  esy_xio_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  esy_device_state_t state;
  uint8_t data = 0x5au;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_xio_uart_init(&uart, &tx_config, &rx_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_xio_uart_register_device(
                                      &binding, &uart, &registry, &registration,
                                      &tx_config, &rx_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.base.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL_INT32(1, esy_hal_uart_send_blocking(&uart, &data, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.base.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_UINT32(4u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  platform_test_set_xio_rx_init_result(ESY_ERR_FAIL);
  TEST_ASSERT_NOT_EQUAL(1, esy_hal_uart_send_blocking(&uart, &data, 1u));
  TEST_ASSERT_EQUAL_PTR(&uart, binding.base.uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.base.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_FAULTED, state);
  TEST_ASSERT_NOT_EQUAL(1, esy_hal_uart_send_blocking(&uart, &data, 1u));
}

void test_esy_hal_xio_uart_device_unregisters_before_start(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config = {0};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "xio-uart",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
  };
  esy_xio_uart_config_t tx_config = {
      .pin = TEST_XIO_TX_PIN,
      .baudrate = 115200u,
      .instance = 0u,
      .sm_index = 1u,
  };
  esy_xio_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_xio_uart_init(&uart, &tx_config, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_xio_uart_register_device(
                  &binding, &uart, &registry, &registration, &tx_config, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_xio_uart_unregister_device(&binding));
  TEST_ASSERT_NULL(binding.base.uart);
}

void test_xio_dma_failed_abort_keeps_borrow_until_successful_retry(void) {
  esy_uart_t uart = {0};
  init_xio_uart(&uart);
  platform_test_set_dma_busy(false);
  uint8_t byte = 0u;
  esy_dma_config_t transfer = {.src = &byte, .length = sizeof(byte)};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_send_dma(&uart, &transfer));
  TEST_ASSERT_TRUE(esy_hal_xio_uart_is_busy(&uart, true));
  platform_test_set_dma_results(ESY_OK, ESY_ERR_TIMEOUT, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_uart_abort_dma(&uart, true));
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, true));
  esy_hal_xio_uart_deinit(&uart);
  TEST_ASSERT_NOT_NULL(uart.ops->send_dma);
  platform_test_set_uart_dma_results(ESY_OK, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_poll(&uart));
  platform_test_set_dma_results(ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_abort_dma(&uart, true));
  TEST_ASSERT_FALSE(esy_hal_xio_uart_is_busy(&uart, true));
  platform_test_set_uart_dma_results(ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_poll(&uart));
  esy_hal_xio_uart_deinit(&uart);
  TEST_ASSERT_NULL(uart.ops);
}

static void count_xio_completion(void *arg) { ++*(uint32_t *)arg; }

void test_xio_source_retirement_does_not_allow_pm_or_deinit_before_wire_drain(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  const esy_device_registration_config_t registration = {
      .name = "xio-drain", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  const esy_xio_uart_config_t config = {.pin = TEST_XIO_TX_PIN,
      .baudrate = 9600u, .instance = 0u, .sm_index = 1u};
  esy_xio_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  uint8_t byte = 0x55u;
  uint32_t callbacks = 0u, usage = 1u;
  esy_dma_config_t transfer = {.src = &byte, .length = sizeof(byte),
      .callback = count_xio_completion, .callback_arg = &callbacks};
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t registry_config = {.static_resources = &resources};
  esy_xio_uart_config_t tx = config;
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, &tx, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_register_device(
      &binding, &uart, &registry, &registration, &config, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_send_dma(&uart, &transfer));
  platform_test_set_xio_uart_wire_busy(true);
  const esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT32(1u, callbacks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.base.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_FALSE(binding.base.suspended);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_FALSE(binding.base.suspended);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_unregister_device(&binding));
  esy_hal_xio_uart_deinit(&uart);
  TEST_ASSERT_NOT_NULL(uart.ctx[0]);
  TEST_ASSERT_NOT_NULL(uart.ops->send_dma);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);

  platform_test_set_xio_uart_wire_busy(false);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  esy_hal_xio_uart_deinit(&uart);
  TEST_ASSERT_NULL(uart.ctx[0]);
  TEST_ASSERT_NULL(uart.ops);
  TEST_ASSERT_EQUAL_UINT32(1u, callbacks);
}

void test_xio_runtime_suspend_succeeds_only_after_wire_drain(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  const esy_device_registration_config_t registration = {
      .name = "xio-pm", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  esy_xio_uart_config_t config = {.pin = TEST_XIO_TX_PIN,
      .baudrate = 9600u, .instance = 0u, .sm_index = 1u};
  esy_xio_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  const esy_device_registry_config_t registry_config = {.static_resources = &resources};
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, &config, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_register_device(
      &binding, &uart, &registry, &registration, &config, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  platform_test_set_xio_uart_wire_busy(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_FALSE(binding.base.suspended);
  platform_test_set_xio_uart_wire_busy(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_TRUE(binding.base.suspended);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_abort(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_unregister_device(&binding));
  esy_hal_xio_uart_deinit(&uart);
  TEST_ASSERT_NULL(uart.ctx[0]);
}

void test_xio_uart_foreign_raw_dma_does_not_prevent_idle_graph_suspend(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  const esy_device_registry_config_t registry_config = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_xio_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  esy_xio_uart_config_t tx = {.pin = TEST_XIO_TX_PIN, .baudrate = 115200u,
      .instance = 0u, .sm_index = 1u};
  esy_xio_uart_config_t rx = {.pin = TEST_XIO_RX_PIN, .baudrate = 115200u,
      .instance = 0u, .sm_index = 2u};
  const esy_device_registration_config_t registration = {
      .name = "xio-foreign-dma", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  esy_device_state_t state;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&uart, &tx, &rx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_register_device(
      &binding, &uart, &registry, &registration, &tx, &rx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  platform_test_set_dma_busy(true);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.base.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_unregister_device(&binding));
  esy_hal_xio_uart_deinit(&uart);
}
