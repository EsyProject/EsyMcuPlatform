/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_i2c.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) {
  platform_test_reset();
}

void tearDown(void) {}

void test_unsupported_slave_rejects_before_claim_pins_or_irq_setup(void) {
  esy_i2c_t i2c = {0};
  const esy_i2c_slave_config_t config = {
      .phys_index = TEST_I2C_INDEX, .address = 0x32u,
      .addr_mode = ESY_I2C_ADDR_7BIT};
  platform_test_set_i2c_slave_validate_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
                    esy_hal_i2c_slave_init(&i2c, &config));
  TEST_ASSERT_NULL(i2c.ctx);
  TEST_ASSERT_NULL(i2c.ops);
  TEST_ASSERT_NULL(platform_test_state()->i2c_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  platform_test_set_i2c_slave_validate_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_slave_init(&i2c, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&i2c));
}

static void init_i2c(esy_i2c_t *i2c) {
  esy_i2c_config_t cfg = {
      .phys_index = TEST_I2C_INDEX,
      .speed_hz = 400000u,
      .addr_mode = ESY_I2C_ADDR_7BIT,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_init(i2c, &cfg));
}

void test_i2c_all_transfer_entries_preserve_full_ten_bit_addresses(void) {
  esy_i2c_t i2c = {0};
  const esy_i2c_config_t config = {.phys_index = TEST_I2C_INDEX,
      .speed_hz = 100000u, .addr_mode = ESY_I2C_ADDR_10BIT};
  uint8_t byte = 0u;
  esy_dma_config_t tx = {.src = &byte, .length = sizeof(byte)};
  esy_dma_config_t rx = {.dst = &byte, .length = sizeof(byte)};
  static const uint16_t k_addresses[] = {0xffu, 0x100u, 0x355u, 0x3ffu};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&i2c, &config));
  for (size_t n = 0u; n < sizeof(k_addresses) / sizeof(k_addresses[0]); ++n) {
    const uint16_t address = k_addresses[n];
    TEST_ASSERT_EQUAL_INT32(1, esy_hal_i2c_write_blocking(&i2c, address, &byte, 1u));
    TEST_ASSERT_EQUAL_UINT16(address, platform_test_state()->last_i2c_address);
    TEST_ASSERT_EQUAL_INT32(1, esy_hal_i2c_read_blocking(&i2c, address, &byte, 1u));
    TEST_ASSERT_EQUAL_UINT16(address, platform_test_state()->last_i2c_address);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_write_dma(&i2c, address, &tx));
    TEST_ASSERT_EQUAL_UINT16(address, platform_test_state()->last_i2c_address);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_read_dma(&i2c, address, &rx));
    TEST_ASSERT_EQUAL_UINT16(address, platform_test_state()->last_i2c_address);
    esy_i2c_xfer_t transfer = {.addr = address, .tx = &byte, .tx_len = 1u};
    TEST_ASSERT_EQUAL_INT32(1, esy_hal_i2c_transfer(&i2c, &transfer));
    TEST_ASSERT_EQUAL_UINT16(address, platform_test_state()->last_i2c_address);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&i2c));
}

void test_i2c_rejects_addresses_above_ten_bits_and_unbounded_timeout_before_dispatch(void) {
  esy_i2c_t i2c = {0};
  uint8_t byte = 0u;
  esy_dma_config_t tx = {.src = &byte, .length = sizeof(byte)};
  esy_dma_config_t rx = {.dst = &byte, .length = sizeof(byte)};
  esy_i2c_xfer_t transfer = {.addr = 0x400u, .tx = &byte, .tx_len = 1u};
  init_i2c(&i2c);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_hal_i2c_write_blocking(&i2c, 0x400u, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_hal_i2c_read_blocking(&i2c, UINT16_MAX, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_write_dma(&i2c, 0x400u, &tx));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_read_dma(&i2c, UINT16_MAX, &rx));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_transfer(&i2c, &transfer));
  transfer.addr = 0x50u;
  transfer.timeout_ms = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_transfer(&i2c, &transfer));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL_UINT16(0u, platform_test_state()->last_i2c_address);
}

void test_esy_hal_i2c_default_cfg_fills_missing_bus_defaults(void) {
  esy_i2c_config_t cfg = {0};

  esy_hal_i2c_default_cfg(NULL);
  esy_hal_i2c_default_cfg(&cfg);

  TEST_ASSERT_EQUAL_UINT32(100000u, cfg.speed_hz);
  TEST_ASSERT_EQUAL_INT(ESY_I2C_ADDR_7BIT, cfg.addr_mode);
  TEST_ASSERT_EQUAL_UINT8(ESY_PIN_UNUSED, cfg.inner.sda_pin);
  TEST_ASSERT_EQUAL_UINT8(ESY_PIN_UNUSED, cfg.inner.scl_pin);

  cfg.speed_hz = 400000u;
  cfg.addr_mode = ESY_I2C_ADDR_10BIT;
  esy_hal_i2c_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_UINT32(100000u, cfg.speed_hz);
  TEST_ASSERT_EQUAL_INT(ESY_I2C_ADDR_7BIT, cfg.addr_mode);
  TEST_ASSERT_EQUAL_UINT8(ESY_PIN_UNUSED, cfg.inner.sda_pin);
  TEST_ASSERT_EQUAL_UINT8(ESY_PIN_UNUSED, cfg.inner.scl_pin);
}

void test_esy_hal_i2c_contracts_are_enforced(void) {
  esy_i2c_t i2c = {0};
  uint8_t data[3] = {1u, 2u, 3u};
  uint8_t rx_data[3] = {0u};
  esy_dma_config_t dma = {
      .src = data,
      .length = sizeof(data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  esy_dma_config_t rx_dma = {
      .dst = rx_data,
      .length = sizeof(rx_data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  esy_i2c_xfer_t xfer = {
      .addr = 0x50u,
      .tx = data,
      .tx_len = sizeof(data),
      .rx = rx_data,
      .rx_len = sizeof(rx_data),
      .repeated_start = true,
  };
  esy_i2c_slave_config_t slave_cfg = {
      .phys_index = TEST_I2C_INDEX,
      .address = 0x32u,
      .addr_mode = ESY_I2C_ADDR_7BIT,
  };
  uint32_t caps = 0u;
  uint8_t index = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_init(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_init(&i2c, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_slave_init(NULL, &slave_cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_slave_init(&i2c, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_i2c_write_blocking(NULL, 0x50u, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_i2c_write_blocking(&i2c, 0x50u, NULL, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_i2c_write_blocking(&i2c, 0x50u, data, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_i2c_write_blocking(&i2c, 0x50u, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_i2c_read_blocking(&i2c, 0x50u, NULL, sizeof(rx_data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_i2c_read_blocking(&i2c, 0x50u, rx_data, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_i2c_read_blocking(&i2c, 0x50u, rx_data, sizeof(rx_data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_write_dma(NULL, 0x50u, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_write_dma(&i2c, 0x50u, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_write_dma(&i2c, 0x50u, &rx_dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_i2c_write_dma(&i2c, 0x50u, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_read_dma(&i2c, 0x50u, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_read_dma(&i2c, 0x50u, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_i2c_read_dma(&i2c, 0x50u, &rx_dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_transfer(NULL, &xfer));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_transfer(&i2c, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_i2c_transfer(&i2c, &xfer));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_set_speed(NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_set_speed(&i2c, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_i2c_set_speed(&i2c, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_abort(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_i2c_abort(&i2c));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_recover_bus(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_i2c_recover_bus(&i2c));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_get_caps(&i2c, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_i2c_get_caps(&i2c, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_get_phys_index(NULL, &index));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_i2c_get_phys_index(&i2c, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_i2c_get_phys_index(&i2c, &index));

  init_i2c(&i2c);
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  TEST_ASSERT_NOT_NULL(i2c.ops->write_blocking);
  TEST_ASSERT_NOT_NULL(i2c.ops->write_dma);
  TEST_ASSERT_EQUAL_INT32(3, esy_hal_i2c_write_blocking(&i2c, 0x50u, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(3, esy_hal_i2c_read_blocking(&i2c, 0x50u, rx_data, sizeof(rx_data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, i2c.ops->write_dma(NULL, 0x50u, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, i2c.ops->read_dma(NULL, 0x50u, &rx_dma));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_write_dma(&i2c, 0x50u, &dma));
  TEST_ASSERT_EQUAL_UINT32(TEST_TX_DREQ, platform_test_state()->last_tx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_TX_CHANNEL, platform_test_state()->last_tx_inner.channel);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_read_dma(&i2c, 0x50u, &rx_dma));
  TEST_ASSERT_EQUAL_UINT32(TEST_RX_DREQ, platform_test_state()->last_rx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_RX_CHANNEL, platform_test_state()->last_rx_inner.channel);
  TEST_ASSERT_EQUAL_INT32(6, esy_hal_i2c_transfer(&i2c, &xfer));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_set_speed(&i2c, 400000u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_abort(&i2c));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_recover_bus(&i2c));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_get_caps(&i2c, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_I2C_CAP_MASTER_TRANSFER | ESY_I2C_CAP_REPEATED_START, caps);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_get_phys_index(&i2c, &index));
  TEST_ASSERT_EQUAL_UINT8(TEST_I2C_INDEX, index);

  platform_test_reset_order_counters();
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&i2c));
  state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT8(TEST_I2C_INDEX, state->reset_index);
  TEST_ASSERT_TRUE(state->driver_get_index_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->driver_deinit_order < state->board_reset_order);

  platform_test_reset();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_i2c_slave_init(&i2c, &slave_cfg));
  TEST_ASSERT_EQUAL_INT(ESY_I2C_ROLE_SLAVE, platform_test_state()->configure_role);
}

void test_i2c_master_uses_board_completed_pins_without_modifying_caller_config(void) {
  esy_i2c_t i2c = {0};
  const esy_i2c_config_t config = {.phys_index = TEST_I2C_INDEX,
      .speed_hz = 400000u, .addr_mode = ESY_I2C_ADDR_7BIT,
      .inner = {.irq_priority = 3u, .sda_pin = 4u, .scl_pin = 5u}};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&i2c, &config));
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT8(TEST_I2C_SDA_PIN, state->last_i2c_inner.sda_pin);
  TEST_ASSERT_EQUAL_UINT8(TEST_I2C_SCL_PIN, state->last_i2c_inner.scl_pin);
  TEST_ASSERT_EQUAL_UINT8(3u, state->last_i2c_inner.irq_priority);
  TEST_ASSERT_EQUAL_UINT8(4u, config.inner.sda_pin);
  TEST_ASSERT_EQUAL_UINT8(5u, config.inner.scl_pin);
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  esy_hal_i2c_deinit(&i2c);
}

void test_i2c_slave_uses_board_completed_pins_without_modifying_caller_config(void) {
  esy_i2c_t i2c = {0};
  const esy_i2c_slave_config_t config = {.phys_index = TEST_I2C_INDEX,
      .address = 0x32u, .addr_mode = ESY_I2C_ADDR_7BIT,
      .inner = {.irq_priority = 6u, .sda_pin = 7u, .scl_pin = 8u}};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_slave_init(&i2c, &config));
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT8(TEST_I2C_SDA_PIN, state->last_i2c_inner.sda_pin);
  TEST_ASSERT_EQUAL_UINT8(TEST_I2C_SCL_PIN, state->last_i2c_inner.scl_pin);
  TEST_ASSERT_EQUAL_UINT8(6u, state->last_i2c_inner.irq_priority);
  TEST_ASSERT_EQUAL_INT(ESY_I2C_ROLE_SLAVE, state->configure_role);
  TEST_ASSERT_EQUAL_UINT8(7u, config.inner.sda_pin);
  TEST_ASSERT_EQUAL_UINT8(8u, config.inner.scl_pin);
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  esy_hal_i2c_deinit(&i2c);
}

void test_i2c_controller_owner_rejects_other_handles_before_pin_changes(void) {
  esy_i2c_t owner = {0};
  esy_i2c_t contender = {0};
  const esy_i2c_config_t master = {.phys_index = TEST_I2C_INDEX};
  const esy_i2c_slave_config_t slave = {.phys_index = TEST_I2C_INDEX,
                                       .address = 0x32u};
  uint8_t data = 0x52u;

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&owner, &master));
  uint32_t configure_order = platform_test_state()->board_configure_order;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_i2c_init(&owner, &master));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_i2c_init(&contender, &master));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_i2c_slave_init(&contender, &slave));
  TEST_ASSERT_EQUAL_UINT32(configure_order,
                          platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_NULL(contender.ctx);
  TEST_ASSERT_EQUAL_INT32(1, esy_hal_i2c_write_blocking(&owner, 0x32u, &data, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_slave_init(&contender, &slave));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&contender));
}

void test_i2c_failed_cleanup_retains_handle_pins_and_exclusive_reservation(void) {
  esy_i2c_t owner = {0};
  esy_i2c_t contender = {0};
  const esy_i2c_config_t config = {.phys_index = TEST_I2C_INDEX};

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&owner, &config));
  void *context = owner.ctx;
  platform_test_set_i2c_busy(true);
  platform_test_reset_order_counters();
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_i2c_deinit(&owner));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);
  platform_test_set_i2c_busy(false);
  platform_test_set_i2c_cleanup_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_i2c_deinit(&owner));
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx);
  TEST_ASSERT_NOT_NULL(owner.ops->write_blocking);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->i2c_owner);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_i2c_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_i2c_init(&contender, &config));
  platform_test_set_i2c_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&owner));
  TEST_ASSERT_NULL(owner.ctx);
  TEST_ASSERT_NULL(owner.ops);
  TEST_ASSERT_NULL(platform_test_state()->i2c_owner);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&contender));
}

void test_i2c_failed_init_cleans_up_or_retains_ownership_for_deinit_retry(void) {
  esy_i2c_t owner = {0};
  esy_i2c_t contender = {0};
  const esy_i2c_config_t config = {.phys_index = TEST_I2C_INDEX};

  platform_test_set_init_result(ESY_ERR_INVALID_ARG);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_init(&owner, &config));
  TEST_ASSERT_NULL(owner.ctx);
  TEST_ASSERT_NULL(platform_test_state()->i2c_owner);
  platform_test_set_i2c_cleanup_result(ESY_ERR_FAIL);
  platform_test_reset_order_counters();
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_i2c_init(&owner, &config));
  TEST_ASSERT_NOT_NULL(owner.ctx);
  TEST_ASSERT_NULL(owner.ops);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->i2c_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_i2c_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_i2c_init(&contender, &config));
  platform_test_set_init_result(ESY_OK);
  platform_test_set_i2c_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&owner));
}
