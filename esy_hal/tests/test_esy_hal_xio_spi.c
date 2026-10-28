/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/xio/esy_hal_xio_spi.h"
#include "esy_hal/xio/esy_hal_xio_spi_device.h"

#include "esy_hal/esy_hal_spi.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) { platform_test_reset(); }

void tearDown(void) {}

static void init_xio_spi(esy_spi_t *spi) {
  esy_xio_spi_config_t cfg = {
      .pin_sck = TEST_XIO_SCK_PIN,
      .pin_mosi = TEST_XIO_TX_PIN,
      .pin_miso = TEST_XIO_RX_PIN,
      .role = ESY_SPI_ROLE_MASTER,
      .mode = ESY_SPI_MODE_0,
      .is_msb_first = true,
      .data_bits = 8u,
      .speed_hz = 1000000u,
      .instance = 0u,
      .sm_index = 1u,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_xio_spi_init(spi, &cfg));
}

static void init_three_wire_xio_spi(esy_spi_t *spi) {
  esy_xio_spi_config_t cfg = {
      .pin_sck = TEST_XIO_SCK_PIN,
      .pin_mosi = TEST_XIO_TX_PIN,
      .pin_miso = TEST_XIO_TX_PIN,
      .role = ESY_SPI_ROLE_MASTER_NO_CS,
      .mode = ESY_SPI_MODE_0,
      .is_msb_first = true,
      .data_bits = 8u,
      .speed_hz = 1000000u,
      .instance = 0u,
      .sm_index = 1u,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_xio_spi_init(spi, &cfg));
}

void test_esy_hal_xio_spi_binds_ops_and_configures_board_pin(void) {
  esy_spi_t spi = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_xio_spi_init(NULL, NULL));
  init_xio_spi(&spi);

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  TEST_ASSERT_EQUAL_UINT8(TEST_XIO_SCK_PIN, state->last_xio_pin);
  TEST_ASSERT_EQUAL_UINT8(0u, state->last_xio_config.instance);
  TEST_ASSERT_EQUAL_UINT8(1u, state->last_xio_config.sm_index);
  TEST_ASSERT_EQUAL_UINT8(0u, state->last_xio_spi_config.instance);
  TEST_ASSERT_EQUAL_UINT8(1u, state->last_xio_spi_config.sm_index);
  TEST_ASSERT_NOT_NULL(spi.ops->write_blocking);
  TEST_ASSERT_NOT_NULL(spi.ops->transfer_dma);
}

void test_esy_hal_xio_spi_preserves_shared_data_pin_configuration(void) {
  esy_spi_t spi = {0};
  const uint8_t tx_buf[] = {0x80u, 0x06u};
  uint8_t rx_buf[1] = {0u};

  init_three_wire_xio_spi(&spi);

  const esy_xio_spi_config_t *cfg = &platform_test_state()->last_xio_spi_config;
  TEST_ASSERT_EQUAL_UINT8(TEST_XIO_TX_PIN, cfg->pin_mosi);
  TEST_ASSERT_EQUAL_UINT8(TEST_XIO_TX_PIN, cfg->pin_miso);
  TEST_ASSERT_EQUAL_INT(ESY_SPI_ROLE_MASTER_NO_CS, cfg->role);
  TEST_ASSERT_NOT_NULL(spi.ops->write_then_read_blocking);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      spi.ops->write_then_read_blocking(&spi, tx_buf, sizeof(tx_buf), rx_buf,
                                       sizeof(rx_buf)));
}

void test_esy_hal_xio_spi_dma_completes_xio_board_metadata(void) {
  esy_spi_t spi = {0};
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

  init_xio_spi(&spi);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_xio_spi_transfer_dma(
                                      &spi, &tx_dma, &rx_dma, NULL, NULL));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL_UINT32(0x700u + TEST_XIO_TX_PIN, state->last_tx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_XIO_TX_PIN & 0x0Fu,
                          state->last_tx_inner.channel);
  TEST_ASSERT_EQUAL_UINT32(0x700u + TEST_XIO_RX_PIN, state->last_rx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_XIO_RX_PIN & 0x0Fu,
                          state->last_rx_inner.channel);
}

void test_esy_hal_xio_spi_default_deinit_and_half_duplex_paths(void) {
  esy_xio_spi_config_t cfg = {0};
  esy_spi_t spi = {0};
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

  esy_hal_xio_spi_default_cfg(NULL);
  esy_hal_xio_spi_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_UINT8(ESY_PIN_UNUSED, cfg.pin_sck);
  TEST_ASSERT_EQUAL_INT(ESY_SPI_ROLE_MASTER, cfg.role);
  TEST_ASSERT_EQUAL_INT(ESY_SPI_MODE_0, cfg.mode);
  TEST_ASSERT_TRUE(cfg.is_msb_first);
  TEST_ASSERT_EQUAL_UINT8(8u, cfg.data_bits);
  TEST_ASSERT_EQUAL_UINT8(0xFFu, cfg.instance);
  TEST_ASSERT_EQUAL_UINT8(0xFFu, cfg.sm_index);
  TEST_ASSERT_EQUAL_UINT8(0xFFu, cfg.slave_cs_sm_index);

  init_xio_spi(&spi);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_xio_spi_transfer_dma(NULL, &tx_dma, &rx_dma, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_xio_spi_transfer_dma(
                                      &spi, &tx_dma, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_xio_spi_transfer_dma(
                                      &spi, NULL, &rx_dma, NULL, NULL));

  esy_hal_xio_spi_deinit(&spi);
  TEST_ASSERT_NOT_EQUAL(0u, platform_test_state()->driver_deinit_order);
}

void test_esy_hal_xio_spi_device_runtime_suspend_and_lazy_resume(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config = {0};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "xio-spi",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
  };
  esy_xio_spi_config_t config = {
      .pin_sck = TEST_XIO_SCK_PIN,
      .pin_mosi = TEST_XIO_TX_PIN,
      .pin_miso = TEST_XIO_RX_PIN,
      .role = ESY_SPI_ROLE_MASTER,
      .mode = ESY_SPI_MODE_0,
      .is_msb_first = true,
      .data_bits = 8u,
      .speed_hz = 1000000u,
      .instance = 0u,
      .sm_index = 1u,
  };
  esy_spi_device_binding_t binding = {0};
  esy_spi_t spi = {0};
  esy_device_state_t state;
  uint8_t data = 0x5au;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_xio_spi_init(&spi, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_xio_spi_register_device(&binding, &spi, &registry,
                                                   &registration, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL_INT32(1, esy_hal_spi_write_blocking(&spi, &data, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_UINT32(2u, platform_test_state()->driver_init_calls);
}

void test_esy_hal_xio_spi_device_unregisters_before_start(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config = {0};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "xio-spi",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
  };
  esy_xio_spi_config_t config = {
      .pin_sck = TEST_XIO_SCK_PIN,
      .pin_mosi = TEST_XIO_TX_PIN,
      .pin_miso = TEST_XIO_RX_PIN,
      .role = ESY_SPI_ROLE_MASTER,
      .mode = ESY_SPI_MODE_0,
      .is_msb_first = true,
      .data_bits = 8u,
      .speed_hz = 1000000u,
      .instance = 0u,
      .sm_index = 1u,
  };
  esy_spi_device_binding_t binding = {0};
  esy_spi_t spi = {0};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_xio_spi_init(&spi, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_xio_spi_register_device(&binding, &spi, &registry,
                                                   &registration, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_unregister_device(&binding));
  TEST_ASSERT_NULL(esy_hal_spi_get_device(&spi));
}

void test_xio_spi_exposes_poll_abort_without_hardware_context_queries(void) {
  esy_spi_t spi = {0};
  size_t remaining;
  init_xio_spi(&spi);
  TEST_ASSERT_NOT_NULL(spi.ops->poll);
  TEST_ASSERT_NOT_NULL(spi.ops->abort);
  TEST_ASSERT_NOT_NULL(spi.ops->is_busy);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_poll(&spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_abort(&spi));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
      esy_hal_spi_get_dma_trans_count_remaining(&spi, &remaining, NULL));
  platform_test_set_dma_busy(true);
  esy_hal_xio_spi_deinit(&spi);
  TEST_ASSERT_NOT_NULL(spi.ops->poll);
  platform_test_set_dma_busy(false);
  esy_hal_xio_spi_deinit(&spi);
  TEST_ASSERT_NULL(spi.ops);
  init_xio_spi(&spi);
  esy_hal_xio_spi_deinit(&spi);
}

void test_xio_spi_pm_preserves_dma_started_before_device_registration(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "xio-active-dma",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
  };
  esy_spi_device_binding_t binding = {0};
  esy_spi_t spi = {0};
  esy_device_state_t state;
  uint8_t data = 0x5au;
  esy_dma_config_t tx = {.src = &data, .length = sizeof(data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8};

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t registry_config = {.static_resources = &resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &registry_config));
  init_xio_spi(&spi);
  esy_xio_spi_config_t config = platform_test_state()->last_xio_spi_config;
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_xio_spi_transfer_dma(&spi, &tx, NULL, NULL, NULL));
  platform_test_set_dma_busy(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_register_device(
      &binding, &spi, &registry, &registration, &config));
  TEST_ASSERT_FALSE(binding.async_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  platform_test_reset_order_counters();

  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);

  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_FALSE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_abort(&spi));
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL_INT32(1, esy_hal_spi_write_blocking(&spi, &data, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_abort(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&binding));
  esy_hal_xio_spi_deinit(&spi);
}

void test_xio_slave_without_cs_rejects_before_board_or_backend_mutation_and_retries(void) {
  esy_spi_t spi = {0};
  esy_xio_spi_config_t config;
  esy_hal_xio_spi_default_cfg(&config);
  config.role = ESY_SPI_ROLE_SLAVE;
  config.pin_sck = TEST_XIO_SCK_PIN;
  config.pin_mosi = TEST_XIO_TX_PIN;
  config.pin_miso = TEST_XIO_RX_PIN;
  config.instance = 0u;
  config.sm_index = 1u;
  for (unsigned mode = 0u; mode < 4u; ++mode) {
    config.mode = (esy_spi_mode_t)mode;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_xio_spi_init(&spi, &config));
    TEST_ASSERT_NULL(spi.ctx);
    TEST_ASSERT_NULL(spi.ops);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_configure_order);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
  }
  config.pin_cs = TEST_GPIO_PIN;
  config.slave_cs_sm_index = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_init(&spi, &config));
  TEST_ASSERT_NOT_NULL(spi.ops->write_blocking);
  esy_hal_xio_spi_deinit(&spi);
}

void test_xio_slave_miso_requires_distinct_explicit_cs_monitor_before_board_work(void) {
  esy_spi_t spi = {0};
  esy_xio_spi_config_t config;
  esy_hal_xio_spi_default_cfg(&config);
  config.role = ESY_SPI_ROLE_SLAVE;
  config.pin_sck = TEST_XIO_SCK_PIN;
  config.pin_mosi = TEST_XIO_TX_PIN;
  config.pin_miso = TEST_XIO_RX_PIN;
  config.pin_cs = TEST_GPIO_PIN;
  config.instance = 0u;
  config.sm_index = 1u;
  const uint8_t rejected[] = {0xFFu, 4u, 1u};
  for (size_t i = 0u; i < sizeof(rejected); ++i) {
    config.slave_cs_sm_index = rejected[i];
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_xio_spi_init(&spi, &config));
    TEST_ASSERT_NULL(spi.ctx);
    TEST_ASSERT_NULL(spi.ops);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_configure_order);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
  }
  config.slave_cs_sm_index = 2u;
  const uint8_t aliases[] = {config.pin_sck, config.pin_cs, config.pin_mosi};
  for (size_t i = 0u; i < sizeof(aliases); ++i) {
    config.pin_miso = aliases[i];
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_xio_spi_init(&spi, &config));
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_configure_order);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
  }
  config.pin_miso = TEST_XIO_RX_PIN;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_init(&spi, &config));
  TEST_ASSERT_EQUAL_UINT8(2u,
      platform_test_state()->last_xio_spi_config.slave_cs_sm_index);
  esy_hal_xio_spi_deinit(&spi);

  config.slave_cs_sm_index = 0xFFu;
  config.pin_miso = ESY_PIN_UNUSED;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_init(&spi, &config));
  esy_hal_xio_spi_deinit(&spi);
  config.role = ESY_SPI_ROLE_MASTER;
  config.pin_miso = TEST_XIO_RX_PIN;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_spi_init(&spi, &config));
  esy_hal_xio_spi_deinit(&spi);
}

void test_xio_dma_convenience_uses_shared_descriptor_validation_before_dispatch(void) {
  esy_spi_t spi = {0};
  uint32_t sample = 0u;
  esy_dma_config_t dma = {.src = &sample, .length = sizeof(sample),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_32};
  init_xio_spi(&spi);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
      esy_hal_xio_spi_transfer_dma(&spi, &dma, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_transfer_dma_calls);
  dma.transfer_size = ESY_DMA_TRANSFER_SIZE_16;
  dma.length = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_xio_spi_transfer_dma(&spi, &dma, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_transfer_dma_calls);
  esy_hal_xio_spi_deinit(&spi);
}
