/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_spi_device.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) {
  platform_test_reset();
  platform_test_set_dma_busy(false);
}

void tearDown(void) {}

static void init_spi(esy_spi_t *spi) {
  esy_spi_config_t cfg = {
      .phys_index = TEST_SPI_INDEX,
      .role = ESY_SPI_ROLE_MASTER,
      .mode = ESY_SPI_MODE_0,
      .speed_hz = 1000000u,
      .data_bits = 8u,
      .is_msb_first = true,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_init(spi, &cfg));
}

void test_esy_hal_spi_default_cfg_sets_default_frame_format(void) {
  esy_spi_config_t cfg = {0};

  esy_hal_spi_default_cfg(NULL);
  esy_hal_spi_default_cfg(&cfg);

  TEST_ASSERT_EQUAL_UINT8(8u, cfg.data_bits);
  TEST_ASSERT_TRUE(cfg.is_msb_first);
  TEST_ASSERT_EQUAL_UINT8(ESY_PIN_UNUSED, cfg.inner.cs_pin);
}

void test_esy_hal_spi_init_binds_ops_after_board_configure(void) {
  esy_spi_t spi = {0};

  init_spi(&spi);

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT8(TEST_SPI_INDEX, state->configure_index);
  TEST_ASSERT_EQUAL_INT(ESY_SPI_ROLE_MASTER, state->configure_role);
  TEST_ASSERT_TRUE(state->spi_claim_order < state->board_configure_order);
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  TEST_ASSERT_NOT_NULL(spi.ops->write_blocking);
  TEST_ASSERT_NOT_NULL(spi.ops->transfer_dma);
  TEST_ASSERT_NOT_NULL(spi.ops->get_phys_index);
  TEST_ASSERT_NOT_NULL(spi.ops->abort);
}

void test_esy_hal_spi_runtime_api_routes_through_ops(void) {
  esy_spi_t spi = {0};
  uint8_t data[3] = {1u, 2u, 3u};
  uint8_t rx_data[3] = {0u};
  uint16_t words[2] = {1u, 2u};
  uint16_t rx_words[2] = {0u};
  uint32_t caps = 0u;
  uint8_t index = 0u;
  size_t tx_remaining = 0u;
  size_t rx_remaining = 0u;

  init_spi(&spi);

  TEST_ASSERT_EQUAL_INT32(
      3, esy_hal_spi_write_blocking(&spi, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(
      3, esy_hal_spi_read_blocking(&spi, rx_data, sizeof(rx_data)));
  TEST_ASSERT_EQUAL_INT32(
      3, esy_hal_spi_transfer_blocking(&spi, data, rx_data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_IMPLEMENTED,
      esy_hal_spi_write_then_read_blocking(&spi, data, sizeof(data),
                                                rx_data, sizeof(rx_data)));
  TEST_ASSERT_EQUAL_INT32(2,
                          esy_hal_spi_write16_blocking(&spi, words, 2u));
  TEST_ASSERT_EQUAL_INT32(2,
                          esy_hal_spi_read16_blocking(&spi, rx_words, 2u));
  TEST_ASSERT_EQUAL_INT32(
      2, esy_hal_spi_transfer16_blocking(&spi, words, rx_words, 2u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_set_speed(&spi, 2000000u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_abort(&spi));
  platform_test_set_dma_busy(true);
  TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_get_caps(&spi, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_SPI_CAP_DMA_TRANSFER | ESY_SPI_CAP_TRANSFER16, caps);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_spi_get_phys_index(&spi, &index));
  TEST_ASSERT_EQUAL_UINT8(TEST_SPI_INDEX, index);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_spi_get_dma_trans_count_remaining(
                              &spi, &tx_remaining, &rx_remaining));
  TEST_ASSERT_EQUAL_size_t(11u, tx_remaining);
  TEST_ASSERT_EQUAL_size_t(22u, rx_remaining);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_write_calls);
}

void test_esy_hal_spi_rejects_invalid_args_and_missing_ops(void) {
  esy_spi_t spi = {0};
  esy_dma_config_t dma_cfg = {.src = "abc", .length = 3u};
  uint8_t data = 0u;
  uint8_t rx_data = 0u;
  uint16_t word = 0u;
  uint16_t rx_word = 0u;
  uint32_t caps = 0u;
  uint8_t index = 0u;
  size_t remaining = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_init(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_init(&spi, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_write_blocking(NULL, &data, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_write_blocking(&spi, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_write_blocking(&spi, &data, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_spi_write_blocking(&spi, &data, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_read_blocking(&spi, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_read_blocking(&spi, &rx_data, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_spi_read_blocking(&spi, &rx_data, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer_blocking(&spi, NULL, &rx_data, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer_blocking(&spi, &data, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer_blocking(&spi, &data, &rx_data, 0u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_IMPLEMENTED,
      esy_hal_spi_transfer_blocking(&spi, &data, &rx_data, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_then_read_blocking(NULL, &data, 1u, &rx_data, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_then_read_blocking(&spi, NULL, 1u, &rx_data, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_then_read_blocking(&spi, &data, 1u, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_IMPLEMENTED,
      esy_hal_spi_write_then_read_blocking(&spi, &data, 1u, &rx_data, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_write16_blocking(&spi, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_write16_blocking(&spi, &word, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_spi_write16_blocking(&spi, &word, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_read16_blocking(&spi, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_read16_blocking(&spi, &rx_word, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_spi_read16_blocking(&spi, &rx_word, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer16_blocking(&spi, NULL, &rx_word, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer16_blocking(&spi, &word, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer16_blocking(&spi, &word, &rx_word, 0u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_IMPLEMENTED,
      esy_hal_spi_transfer16_blocking(&spi, &word, &rx_word, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_write_dma(NULL, &dma_cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_write_dma(&spi, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_spi_write_dma(&spi, &dma_cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_read_dma(&spi, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_spi_read_dma(&spi, &dma_cfg));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer_dma(NULL, &dma_cfg, &dma_cfg, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer_dma(&spi, NULL, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_hal_spi_transfer_dma(&spi, &dma_cfg, &dma_cfg, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_get_dma_trans_count_remaining(NULL, &remaining, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_spi_get_dma_trans_count_remaining(&spi, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_set_speed(NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_set_speed(&spi, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_spi_set_speed(&spi, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_spi_abort(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_spi_abort(&spi));
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(NULL));
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_get_caps(&spi, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_spi_get_caps(&spi, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_get_phys_index(NULL, &index));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_spi_get_phys_index(&spi, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_spi_get_phys_index(&spi, &index));
}

void test_esy_hal_spi_dma_completes_board_metadata(void) {
  esy_spi_t spi = {0};
  uint8_t tx_data[4] = {0};
  esy_dma_config_t tx_dma = {
      .src = tx_data,
      .length = sizeof(tx_data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  esy_dma_config_t rx_dma = {
      .dst = tx_data,
      .length = sizeof(tx_data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };

  init_spi(&spi);

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      spi.ops->transfer_dma(NULL, &tx_dma, &rx_dma, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          spi.ops->transfer_dma(&spi, NULL, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_write_dma(&spi, &tx_dma));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_read_dma(&spi, &rx_dma));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_transfer_dma(
                                      &spi, &tx_dma, &rx_dma, NULL, NULL));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(3u, state->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL_INT(ESY_DMA_MEM_TO_PERIPH, state->last_tx_inner.direction);
  TEST_ASSERT_EQUAL_UINT32(TEST_TX_DREQ, state->last_tx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_TX_CHANNEL, state->last_tx_inner.channel);
  TEST_ASSERT_EQUAL_INT(ESY_DMA_PERIPH_TO_MEM, state->last_rx_inner.direction);
  TEST_ASSERT_EQUAL_UINT32(TEST_RX_DREQ, state->last_rx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_RX_CHANNEL, state->last_rx_inner.channel);
  TEST_ASSERT_FALSE(spi.ops->is_busy(NULL));
}

void test_esy_hal_spi_deinit_retires_native_before_reset_and_releases_after_reset(void) {
  esy_spi_t spi = {0};

  init_spi(&spi);
  platform_test_reset_order_counters();

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT8(TEST_SPI_INDEX, state->reset_index);
  TEST_ASSERT_TRUE(state->driver_get_index_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->driver_deinit_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->board_reset_order < state->spi_release_order);
  TEST_ASSERT_NULL(spi.ctx);
  TEST_ASSERT_NULL(spi.ops);
  TEST_ASSERT_NULL(state->spi_owner);
}

void test_esy_hal_spi_device_tracks_dma_and_runtime_resume(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t device_config = {
      .name = "spi-test",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
  };
  esy_spi_config_t spi_config = {
      .phys_index = TEST_SPI_INDEX,
      .role = ESY_SPI_ROLE_MASTER,
      .mode = ESY_SPI_MODE_0,
      .speed_hz = 1000000u,
      .data_bits = 8u,
      .is_msb_first = true,
  };
  esy_spi_device_binding_t binding = {0};
  esy_spi_t spi = {0};
  uint8_t data[4] = {0};
  esy_dma_config_t tx_dma = {
      .src = data,
      .length = sizeof(data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  uint32_t usage_count = 0u;
  esy_device_state_t state;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_init(&spi, &spi_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_spi_register_device(&binding, &spi, &registry,
                                               &device_config, &spi_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));

  esy_dma_config_t short_rx = {.dst = data, .length = 1u,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(
      &spi, &tx_dma, &short_rx, NULL, NULL));
  TEST_ASSERT_FALSE(binding.async_ref_held);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(0u, usage_count);

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_spi_transfer_dma(&spi, &tx_dma, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&binding.device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_set_speed(&spi, 234000u));
  platform_test_complete_spi_dma(&spi, ESY_SPI_DMA_TRANSFER_OK);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&binding.device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(0u, usage_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL_INT(ESY_BOARD_CONTROLLER_PIN_STATE_IDLE,
                        platform_test_state()->last_controller_pin_state);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_spi_abort(&spi));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_UINT32(2u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));

  TEST_ASSERT_EQUAL_INT32(
      (int32_t)sizeof(data),
      esy_hal_spi_write_blocking(&spi, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_UINT32(3u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(234000u, platform_test_state()->resumed_speed_hz);
}

static esy_err_t failing_poll(esy_spi_t *spi) { (void)spi; return ESY_ERR_BUSY; }
void test_spi_poll_requires_a_backend_hook_and_propagates_retained_cleanup(void) {
  esy_spi_t spi = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_poll(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_poll(&spi));
  init_spi(&spi);
  TEST_ASSERT_NOT_NULL(spi.ops->poll);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_poll(&spi));
  static const esy_spi_vtable_t k_failing_poll_ops = {.poll = failing_poll};
  spi.ops = &k_failing_poll_ops;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_poll(&spi));
}

typedef struct {
  uint32_t calls;
  esy_spi_dma_transfer_status_t status;
} spi_completion_t;

static void record_spi_completion(esy_spi_t *spi,
    const esy_spi_dma_transfer_result_t *result, void *arg) {
  spi_completion_t *completion = arg;
  (void)spi;
  completion->calls++;
  completion->status = result->status;
}

void test_spi_failed_submit_retains_borrow_and_graph_lease_until_poll_or_abort(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_spi_t spi = {0};
  esy_spi_device_binding_t binding = {0};
  esy_spi_config_t config;
  esy_hal_spi_default_cfg(&config);
  config.phys_index = TEST_SPI_INDEX;
  config.speed_hz = 1000000u;
  esy_device_registration_config_t registration = {
      .name = "spi-failed-submit",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)};
  uint8_t data[4] = {0};
  esy_dma_config_t dma = {.src = data, .length = sizeof(data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8};
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  const esy_device_registry_config_t graph = {.static_resources = &resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &graph));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&spi, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_register_device(&binding, &spi,
      &registry, &registration, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  platform_test_set_spi_submit_result(ESY_ERR_IO);
  for (unsigned use_abort = 0u; use_abort < 2u; ++use_abort) {
    spi_completion_t completion = {0};
    uint32_t usage = 0u;
    platform_test_set_dma_busy(true);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_transfer_dma(&spi, &dma,
        NULL, record_spi_completion, &completion));
    TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&spi));
    TEST_ASSERT_TRUE(binding.async_ref_held);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
    TEST_ASSERT_EQUAL_UINT32(1u, usage);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_device_registry_deinit(&registry));
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&binding));
    if (use_abort != 0u) {
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_abort(&spi));
      TEST_ASSERT_EQUAL_UINT32(0u, completion.calls);
    } else {
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_poll(&spi));
      TEST_ASSERT_EQUAL_UINT32(1u, completion.calls);
      TEST_ASSERT_EQUAL(ESY_SPI_DMA_TRANSFER_DMA_ERROR, completion.status);
    }
    TEST_ASSERT_FALSE(esy_hal_spi_is_busy(&spi));
    TEST_ASSERT_FALSE(binding.async_ref_held);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
    TEST_ASSERT_EQUAL_UINT32(0u, usage);
  }
  /* A clean rejection retains neither a callback nor a usage reference. */
  spi_completion_t completion = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_transfer_dma(&spi, &dma,
      NULL, record_spi_completion, &completion));
  TEST_ASSERT_FALSE(binding.async_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_poll(&spi));
  TEST_ASSERT_EQUAL_UINT32(0u, completion.calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));
  TEST_ASSERT_NULL(spi.ctx);
}

void test_spi_dma_rejects_unequal_buffer_ranges_without_dispatch_or_callback(void) {
  esy_spi_t spi = {0};
  uint8_t tx_data[4] = {1u, 2u, 3u, 4u};
  uint8_t rx_data[4] = {0xa5u, 0xa5u, 0xa5u, 0xa5u};
  esy_dma_config_t tx = {.src = tx_data, .length = sizeof(tx_data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8};
  esy_dma_config_t rx = {.dst = rx_data, .length = 1u,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8};
  const esy_dma_config_t tx_before = tx;
  const esy_dma_config_t rx_before = rx;
  spi_completion_t completion = {0};
  init_spi(&spi);

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(
      &spi, &tx, &rx, record_spi_completion, &completion));
  TEST_ASSERT_EQUAL_MEMORY(&tx_before, &tx, sizeof(tx));
  TEST_ASSERT_EQUAL_MEMORY(&rx_before, &rx, sizeof(rx));
  tx.length = 1u;
  rx.length = sizeof(rx_data);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(
      &spi, &tx, &rx, record_spi_completion, &completion));
  tx.length = SIZE_MAX / 8u + 1u;
  rx.length = tx.length;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(
      &spi, &tx, &rx, record_spi_completion, &completion));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, completion.calls);
  const uint8_t expected_rx[4] = {0xa5u, 0xa5u, 0xa5u, 0xa5u};
  TEST_ASSERT_EQUAL_MEMORY(expected_rx, rx_data, sizeof(rx_data));
}

void test_spi_dma_rejects_invalid_endpoints_elements_and_circular_mode(void) {
  esy_spi_t spi = {0};
  uint16_t tx_data[2] = {0};
  uint16_t rx_data[2] = {0};
  esy_dma_config_t tx = {.src = tx_data, .length = sizeof(tx_data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  esy_dma_config_t rx = {.dst = rx_data, .length = sizeof(rx_data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  init_spi(&spi);

  tx.src = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.src = tx_data;
  rx.dst = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_read_dma(&spi, &rx));
  rx.dst = rx_data;
  tx.length = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.length = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.length = sizeof(tx_data);
  tx.src = (const uint8_t *)tx_data + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.src = tx_data;
  rx.dst = (uint8_t *)rx_data + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_read_dma(&spi, &rx));
  rx.dst = rx_data;
  rx.transfer_size = ESY_DMA_TRANSFER_SIZE_8;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_transfer_dma(&spi, &tx, &rx, NULL, NULL));
  tx.transfer_size = (esy_dma_transfer_size_t)99;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.transfer_size = ESY_DMA_TRANSFER_SIZE_32;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_spi_write_dma(&spi, &tx));
  tx.transfer_size = ESY_DMA_TRANSFER_SIZE_16;
  tx.circular = true;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_spi_write_dma(&spi, &tx));
  rx.circular = true;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_spi_read_dma(&spi, &rx));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_transfer_dma_calls);
}

void test_spi_dma_single_direction_supplies_only_an_internal_missing_endpoint(void) {
  esy_spi_t spi = {0};
  uint16_t data[4] = {0};
  esy_dma_config_t tx = {.src = data, .length = sizeof(data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  esy_dma_config_t rx = {.dst = data, .length = sizeof(data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  const esy_dma_config_t tx_before = tx;
  const esy_dma_config_t rx_before = rx;
  esy_spi_config_t config;
  esy_hal_spi_default_cfg(&config);
  config.phys_index = TEST_SPI_INDEX;
  config.data_bits = 16u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&spi, &config));

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_write_dma(&spi, &tx));
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_PTR(data, state->last_spi_tx_dma.src);
  TEST_ASSERT_NULL(state->last_spi_rx_dma.dst);
  TEST_ASSERT_EQUAL_size_t(sizeof(data), state->last_spi_tx_dma.length);
  TEST_ASSERT_EQUAL_size_t(sizeof(data), state->last_spi_rx_dma.length);
  TEST_ASSERT_EQUAL(ESY_DMA_TRANSFER_SIZE_16,
                    state->last_spi_rx_dma.transfer_size);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_read_dma(&spi, &rx));
  TEST_ASSERT_NULL(state->last_spi_tx_dma.src);
  TEST_ASSERT_EQUAL_PTR(data, state->last_spi_rx_dma.dst);
  TEST_ASSERT_EQUAL_size_t(sizeof(data), state->last_spi_tx_dma.length);
  TEST_ASSERT_EQUAL_size_t(sizeof(data), state->last_spi_rx_dma.length);
  TEST_ASSERT_EQUAL(ESY_DMA_TRANSFER_SIZE_16,
                    state->last_spi_tx_dma.transfer_size);
  TEST_ASSERT_EQUAL_MEMORY(&tx_before, &tx, sizeof(tx));
  TEST_ASSERT_EQUAL_MEMORY(&rx_before, &rx, sizeof(rx));
}

void test_spi_controller_owner_rejects_reinit_and_contenders_before_board_changes(void) {
  esy_spi_t owner = {0};
  esy_spi_t contender = {0};
  esy_spi_config_t config;
  uint8_t byte = 0x52u;
  esy_hal_spi_default_cfg(&config);
  config.phys_index = TEST_SPI_INDEX;
  init_spi(&owner);
  const esy_spi_t before = owner;
  const uint32_t configure_order = platform_test_state()->board_configure_order;

  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_init(&contender, &config));
  TEST_ASSERT_EQUAL_MEMORY(&before, &owner, sizeof(owner));
  TEST_ASSERT_EQUAL_UINT32(configure_order,
                          platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_NULL(contender.ctx);
  TEST_ASSERT_NULL(contender.ops);
  TEST_ASSERT_EQUAL_INT32(1, esy_hal_spi_write_blocking(&owner, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&contender));
}

void test_spi_unsupported_configuration_is_rejected_before_board_or_native_init(void) {
  esy_spi_t spi = {0};
  esy_spi_config_t config;
  esy_hal_spi_default_cfg(&config);
  config.phys_index = TEST_SPI_INDEX;
  config.data_bits = 9u;
  /* The fake models a provider rejecting this width; native support is port-specific. */
  platform_test_set_spi_claim_result(ESY_ERR_NOT_SUPPORTED);

  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_spi_init(&spi, &config));
  TEST_ASSERT_NULL(spi.ctx);
  TEST_ASSERT_NULL(platform_test_state()->spi_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);
}

void test_spi_failed_init_cleans_its_claim_or_retains_it_for_cleanup_retry(void) {
  esy_spi_t owner = {0};
  esy_spi_t contender = {0};
  esy_spi_config_t config;
  esy_hal_spi_default_cfg(&config);
  config.phys_index = TEST_SPI_INDEX;
  platform_test_set_init_result(ESY_ERR_INVALID_ARG);

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_init(&owner, &config));
  TEST_ASSERT_NULL(owner.ctx);
  TEST_ASSERT_NULL(platform_test_state()->spi_owner);
  TEST_ASSERT_TRUE(platform_test_state()->driver_deinit_order <
                   platform_test_state()->board_reset_order);
  TEST_ASSERT_TRUE(platform_test_state()->board_reset_order <
                   platform_test_state()->spi_release_order);

  platform_test_reset_order_counters();
  platform_test_set_spi_cleanup_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_init(&owner, &config));
  TEST_ASSERT_NOT_NULL(owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->spi_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->spi_release_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_init(&contender, &config));
  platform_test_set_init_result(ESY_OK);
  platform_test_set_spi_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
}

void test_spi_deinit_preserves_storage_and_pins_while_busy_or_cleanup_fails(void) {
  esy_spi_t owner = {0};
  esy_spi_t contender = {0};
  esy_spi_config_t config;
  esy_hal_spi_default_cfg(&config);
  config.phys_index = TEST_SPI_INDEX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
  init_spi(&owner);
  const esy_spi_t before = owner;
  platform_test_reset_order_counters();
  platform_test_set_dma_busy(true);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_deinit(&owner));
  TEST_ASSERT_EQUAL_MEMORY(&before, &owner, sizeof(owner));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  platform_test_set_dma_busy(false);
  platform_test_set_spi_cleanup_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_deinit(&owner));
  TEST_ASSERT_EQUAL_PTR(before.ctx, owner.ctx);
  TEST_ASSERT_NOT_NULL(owner.ops->write_blocking);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->spi_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->spi_release_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_init(&contender, &config));
  platform_test_set_spi_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
  TEST_ASSERT_NULL(owner.ctx);
  TEST_ASSERT_NULL(owner.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&contender));
}

static void register_spi(esy_spi_t *spi, esy_spi_device_binding_t *binding,
                          esy_device_registry_t *registry,
                          esy_device_registry_static_resources_t *resources,
                          esy_spi_config_t *config) {
  const esy_device_registry_config_t graph = {.static_resources = resources};
  const esy_device_registration_config_t registration = {
      .name = "spi-owner", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  esy_hal_spi_default_cfg(config);
  config->phys_index = TEST_SPI_INDEX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(registry, &graph));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(spi, config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_register_device(
      binding, spi, registry, &registration, config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(registry));
}

void test_spi_runtime_and_system_suspend_keep_claim_until_full_deinit(void) {
  for (unsigned system_suspend = 0u; system_suspend < 2u; ++system_suspend) {
    ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
    esy_device_registry_static_resources_t resources;
    ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
    esy_device_registry_t registry = {0};
    esy_spi_device_binding_t binding = {0};
    esy_spi_t owner = {0}, contender = {0};
    esy_spi_config_t config;
    uint8_t byte = 0x31u;
    esy_device_state_t state;
    register_spi(&owner, &binding, &registry, &resources, &config);
    void *context = owner.ctx;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_deinit(&owner));
    platform_test_reset_order_counters();
    TEST_ASSERT_EQUAL(ESY_OK, system_suspend != 0u
        ? esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION)
        : esy_device_registry_runtime_suspend_idle(&registry));
    TEST_ASSERT_FALSE(platform_test_state()->spi_enabled);
    TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->spi_owner);
    TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
    TEST_ASSERT_TRUE(platform_test_state()->driver_deinit_order <
                     platform_test_state()->board_pin_state_order);
    TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->spi_release_order);
    const uint32_t configure_order = platform_test_state()->board_configure_order;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_init(&contender, &config));
    TEST_ASSERT_EQUAL_UINT32(configure_order,
                            platform_test_state()->board_configure_order);
    if (system_suspend != 0u) {
      TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_abort(&registry));
    }
    TEST_ASSERT_EQUAL_INT32(1, esy_hal_spi_write_blocking(&owner, &byte, 1u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
    TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
    TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
    TEST_ASSERT_TRUE(platform_test_state()->spi_enabled);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&contender, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&contender));
  }
}

static void check_spi_partial_suspend_fault(bool system_suspend) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_t registry = {0};
  esy_spi_device_binding_t binding = {0};
  esy_spi_t owner = {0}, contender = {0};
  esy_spi_config_t config;
  esy_device_state_t state;
  uint8_t byte = 0u;
  register_spi(&owner, &binding, &registry, &resources, &config);
  void *context = owner.ctx;
  platform_test_reset_order_counters();
  platform_test_set_spi_cleanup_result(ESY_ERR_IO);

  TEST_ASSERT_EQUAL(ESY_ERR_STATE, system_suspend
      ? esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION)
      : esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_FAULTED, state);
  TEST_ASSERT_FALSE(platform_test_state()->spi_enabled);
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->spi_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_pin_state_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->spi_release_order);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_write_blocking(&owner, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_init(&contender, &config));
  platform_test_set_spi_cleanup_result(ESY_OK);
  /* Faulted graphs retain ownership for system recovery. */
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_deinit(&owner));
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->spi_owner);
}

void test_spi_runtime_partial_cleanup_failure_faults_graph_and_preserves_claim(void) {
  check_spi_partial_suspend_fault(false);
}

void test_spi_system_partial_cleanup_failure_faults_graph_and_preserves_claim(void) {
  check_spi_partial_suspend_fault(true);
}

void test_spi_pin_state_failure_restores_native_owner_before_rejecting_suspend(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_t registry = {0};
  esy_spi_device_binding_t binding = {0};
  esy_spi_t owner = {0};
  esy_spi_config_t config;
  esy_device_state_t state;
  uint8_t byte = 0u;
  register_spi(&owner, &binding, &registry, &resources, &config);
  platform_test_set_pin_state_result(ESY_ERR_IO);

  TEST_ASSERT_EQUAL(ESY_ERR_IO,
                    esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_FALSE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_TRUE(platform_test_state()->spi_enabled);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->spi_owner);
  TEST_ASSERT_EQUAL_UINT32(2u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_INT32(1, esy_hal_spi_write_blocking(&owner, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
}

void test_spi_failed_suspend_rollback_faults_graph_without_releasing_controller(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_t registry = {0};
  esy_spi_device_binding_t binding = {0};
  esy_spi_t owner = {0}, contender = {0};
  esy_spi_config_t config;
  register_spi(&owner, &binding, &registry, &resources, &config);
  void *context = owner.ctx;
  platform_test_set_pin_state_result(ESY_ERR_IO);
  platform_test_set_init_result(ESY_ERR_FAIL);

  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->spi_owner);
  /* A failed native restart may reset its own pins while retaining the claim. */
  TEST_ASSERT_NOT_EQUAL(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->spi_release_order);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_deinit(&owner));
}

void test_spi_failed_resume_faults_graph_and_retains_suspended_owner(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_t registry = {0};
  esy_spi_device_binding_t binding = {0};
  esy_spi_t owner = {0}, contender = {0};
  esy_spi_config_t config;
  register_spi(&owner, &binding, &registry, &resources, &config);
  void *context = owner.ctx;
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_device_registry_runtime_suspend_idle(&registry));
  platform_test_set_init_result(ESY_ERR_FAIL);
  uint8_t byte = 0u;

  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_write_blocking(&owner, &byte, 1u));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->spi_owner);
  TEST_ASSERT_NOT_EQUAL(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->spi_release_order);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_deinit(&owner));
}

void test_spi_copied_registered_handle_cannot_resume_or_borrow_the_original_owner(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_t registry = {0};
  esy_spi_device_binding_t binding = {0};
  esy_spi_t owner = {0};
  esy_spi_config_t config;
  uint8_t byte = 0x25u;
  esy_dma_config_t dma = {.src = &byte, .length = 1u,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8};
  esy_device_state_t state;
  uint32_t usage = UINT32_MAX;
  register_spi(&owner, &binding, &registry, &resources, &config);
  /* Deliberately violate the no-copy contract and verify a harmless rejection. */
  esy_spi_t copied = owner;
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_device_registry_runtime_suspend_idle(&registry));
  const esy_spi_t owner_before = owner;
  const esy_spi_device_binding_t binding_before = binding;
  const uint32_t configure_order = platform_test_state()->board_configure_order;

  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_abort(&copied));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_write_blocking(&copied, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_read_blocking(&copied, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_set_speed(&copied, 2000000u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_hal_spi_transfer_dma(&copied, &dma, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_MEMORY(&owner_before, &owner, sizeof(owner));
  TEST_ASSERT_EQUAL_MEMORY(&binding_before, &binding, sizeof(binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  TEST_ASSERT_EQUAL_UINT32(configure_order,
                          platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->spi_abort_calls);
  TEST_ASSERT_FALSE(platform_test_state()->spi_enabled);

  TEST_ASSERT_EQUAL_INT32(1, esy_hal_spi_write_blocking(&owner, &byte, 1u));
  TEST_ASSERT_TRUE(platform_test_state()->spi_enabled);
  platform_test_set_dma_busy(true);
  TEST_ASSERT_EQUAL(ESY_OK,
                    esy_hal_spi_transfer_dma(&owner, &dma, NULL, NULL, NULL));
  TEST_ASSERT_TRUE(binding.async_ref_held);
  /* Reject before consulting the original binding's already-held async lease. */
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                    esy_hal_spi_transfer_dma(&copied, &dma, NULL, NULL, NULL));
  TEST_ASSERT_TRUE(binding.async_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(1u, usage);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_transfer_dma_calls);
  platform_test_complete_spi_dma(&owner, ESY_SPI_DMA_TRANSFER_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&owner));
}

typedef enum {
  PM_WRITE_VALID,
  PM_WRITE_NO_DEPENDENCY,
  PM_WRITE_FOREIGN_REQUESTER,
  PM_WRITE_SUSPENDED_PROVIDER,
  PM_WRITE_BUSY_PROVIDER,
  PM_WRITE_ASYNC_BORROW,
  PM_WRITE_MISSING_HOOK,
} pm_write_case_t;

typedef struct {
  esy_spi_t *spi;
  esy_spi_device_binding_t *binding;
  const esy_device_t *requester_override;
  pm_write_case_t test_case;
  int32_t results[4];
  uint32_t calls;
  uint32_t usage_before;
  uint32_t usage_after;
} pm_write_probe_t;

static esy_err_t dependent_pm_write(esy_device_t *device,
                                     esy_device_pm_action_t action,
                                     esy_power_state_t state) {
  (void)action;
  (void)state;
  pm_write_probe_t *probe = device->driver_ctx;
  const uint8_t byte = 0x5au;
  const esy_spi_vtable_t *saved_ops = probe->spi->ops;
  static const esy_spi_vtable_t k_missing_write_ops = {0};
  if (probe->test_case == PM_WRITE_SUSPENDED_PROVIDER) {
    /* Inject invalid graph state to exercise the HAL's defensive parent guard. */
    atomic_store_explicit(&probe->binding->device.state,
                          ESY_DEVICE_STATE_RUNTIME_SUSPENDED, memory_order_release);
  }
  if (probe->test_case == PM_WRITE_BUSY_PROVIDER)
    platform_test_set_dma_busy(true);
  if (probe->test_case == PM_WRITE_MISSING_HOOK)
    probe->spi->ops = &k_missing_write_ops;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(
      &probe->binding->device, &probe->usage_before));
  int32_t result = esy_hal_spi_write_blocking_for_pm(
      probe->spi, probe->requester_override != NULL ? probe->requester_override : device,
      &byte, sizeof(byte));
  TEST_ASSERT_LESS_THAN_UINT32(4u, probe->calls);
  probe->results[probe->calls++] = result;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(
      &probe->binding->device, &probe->usage_after));
  TEST_ASSERT_EQUAL_UINT32(probe->usage_before, probe->usage_after);
  probe->spi->ops = saved_ops;
  if (probe->test_case == PM_WRITE_SUSPENDED_PROVIDER)
    atomic_store_explicit(&probe->binding->device.state,
                          ESY_DEVICE_STATE_ACTIVE, memory_order_release);
  if (probe->test_case == PM_WRITE_BUSY_PROVIDER)
    platform_test_set_dma_busy(false);
  return ESY_OK;
}

static const esy_device_ops_t k_dependent_pm_ops = {.pm_action = dependent_pm_write};

static void init_spi_pm_graph(esy_spi_t *spi, esy_spi_device_binding_t *binding,
                               esy_device_registry_t *registry,
                               esy_device_registry_static_resources_t *resources,
                               esy_device_t *child, pm_write_probe_t *probe,
                               esy_device_t *const *dependencies, uint32_t count) {
  const esy_device_registry_config_t graph = {.static_resources = resources};
  const esy_device_registration_config_t registration = {
      .name = "spi-parent", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  esy_spi_config_t config;
  esy_hal_spi_default_cfg(&config);
  config.phys_index = TEST_SPI_INDEX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(registry, &graph));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(spi, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_register_device(
      binding, spi, registry, &registration, &config));
  const esy_device_config_t child_config = {
      .name = "spi-dependent", .ops = &k_dependent_pm_ops, .driver_ctx = probe,
      .dependencies = dependencies, .dependency_count = count,
      .supported_state_mask = registration.supported_state_mask};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_register(registry, child, &child_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(registry));
}

void test_spi_const_provider_and_private_binding_survive_complete_lifecycles(void) {
  esy_spi_t first = {0}, second = {0};
  TEST_ASSERT_NULL(esy_hal_spi_get_device(NULL));
  TEST_ASSERT_NULL(esy_hal_spi_get_device(&first));
  init_spi(&first);
  const esy_spi_vtable_t *first_ops = first.ops;
  TEST_ASSERT_NOT_NULL(first_ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&first));
  TEST_ASSERT_NULL(first.ops);
  init_spi(&second);
  TEST_ASSERT_EQUAL_PTR(first_ops, second.ops);
  TEST_ASSERT_NULL(esy_hal_spi_get_device(&second));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&second));
}

void test_spi_pm_write_rejects_invalid_arguments_and_unbound_handles(void) {
  esy_spi_t spi = {0};
  esy_device_t requester = {0};
  const uint8_t byte = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_blocking_for_pm(NULL, &requester, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_blocking_for_pm(&spi, NULL, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, NULL, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, &byte, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, &byte, 1u));
}

void test_spi_pm_write_requires_the_transitioning_direct_dependent_and_idle_parent(void) {
  const int32_t expected[] = {1, ESY_ERR_STATE, ESY_ERR_STATE, ESY_ERR_STATE,
                             ESY_ERR_BUSY, ESY_ERR_BUSY, ESY_ERR_NOT_SUPPORTED};
  for (pm_write_case_t mode = PM_WRITE_VALID; mode <= PM_WRITE_MISSING_HOOK; ++mode) {
    platform_test_reset();
    ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
    esy_device_registry_static_resources_t resources;
    ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
    esy_device_registry_t registry = {0};
    esy_spi_t spi = {0};
    esy_spi_device_binding_t binding = {0};
    esy_device_t child = {0};
    esy_device_t *dependencies[] = {&binding.device};
    pm_write_probe_t probe = {.spi = &spi, .binding = &binding, .test_case = mode};
    init_spi_pm_graph(&spi, &binding, &registry, &resources, &child, &probe,
                      mode == PM_WRITE_NO_DEPENDENCY ? NULL : dependencies,
                      mode == PM_WRITE_NO_DEPENDENCY ? 0u : 1u);
    TEST_ASSERT_EQUAL_PTR(&binding.device, esy_hal_spi_get_device(&spi));
    const uint8_t byte = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_hal_spi_write_blocking_for_pm(&spi, &child, &byte, 1u));
    esy_device_registry_t foreign_registry = {0};
    esy_device_t foreign = {.registry = &foreign_registry};
    if (mode == PM_WRITE_FOREIGN_REQUESTER) probe.requester_override = &foreign;
    if (mode == PM_WRITE_ASYNC_BORROW) {
      platform_test_set_dma_busy(true);
      esy_dma_config_t transfer = {.src = &byte, .length = sizeof(byte)};
      TEST_ASSERT_EQUAL(ESY_OK,
          esy_hal_spi_transfer_dma(&spi, &transfer, NULL, NULL, NULL));
    }
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&registry));
    TEST_ASSERT_EQUAL_UINT32(1u, probe.calls);
    TEST_ASSERT_EQUAL_INT32(expected[mode], probe.results[0]);
    if (mode == PM_WRITE_ASYNC_BORROW)
      platform_test_complete_spi_dma(&spi, ESY_SPI_DMA_TRANSFER_OK);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get(&binding.device));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_put(&binding.device));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get(&child));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_put(&child));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&binding));
    TEST_ASSERT_NULL(esy_hal_spi_get_device(&spi));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));
  }
}

void test_spi_pm_write_remains_valid_during_runtime_and_system_rollback(void) {
  for (unsigned system = 0u; system < 2u; ++system) {
    platform_test_reset();
    ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
    esy_device_registry_static_resources_t resources;
    ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
    esy_device_registry_t registry = {0};
    esy_spi_t spi = {0};
    esy_spi_device_binding_t binding = {0};
    esy_device_t child = {0};
    esy_device_t *dependencies[] = {&binding.device};
    pm_write_probe_t probe = {.spi = &spi, .binding = &binding};
    init_spi_pm_graph(&spi, &binding, &registry, &resources, &child, &probe,
                      dependencies, 1u);
    platform_test_set_spi_cleanup_result(ESY_ERR_BUSY);
    esy_err_t result = system == 0u
        ? esy_device_registry_runtime_suspend_idle(&registry)
        : esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, result);
    TEST_ASSERT_EQUAL_UINT32(2u, probe.calls);
    TEST_ASSERT_EQUAL_INT32(1, probe.results[0]);
    TEST_ASSERT_EQUAL_INT32(1, probe.results[1]);
    esy_device_state_t state;
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&child, &state));
    TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&binding.device, &state));
    TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
    platform_test_set_spi_cleanup_result(ESY_OK);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_unregister_device(&binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));
  }
}
