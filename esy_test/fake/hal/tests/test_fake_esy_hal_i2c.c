/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_i2c.h"
#include "unity.h"

void setUp(void) { fake_esy_hal_i2c_reset(); }
void tearDown(void) {}

static void initialize(esy_i2c_t *bus) {
  esy_i2c_config_t config;
  esy_hal_i2c_default_cfg(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(bus, &config));
}

void test_i2c_fake_copies_expectations_and_enforces_transaction_options(void) {
  esy_i2c_t bus = {0};
  initialize(&bus);
  uint8_t command = 0x10u, response[] = {0x11u, 0x22u}, received[2] = {0};
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      0x48u, &command, 1u, response, sizeof(response), 3));
  fake_esy_hal_i2c_expect_timeout(25u);
  fake_esy_hal_i2c_expect_repeated_start(true);
  command = 0x55u;
  response[0] = 0xffu;
  const uint8_t sent = 0x10u;
  esy_i2c_xfer_t transfer = {.addr = 0x48u, .tx = &sent, .tx_len = 1u,
      .rx = received, .rx_len = sizeof(received), .timeout_ms = 25u,
      .repeated_start = true};
  TEST_ASSERT_EQUAL(3, esy_hal_i2c_transfer(&bus, &transfer));
  TEST_ASSERT_EQUAL_HEX8(0x11u, received[0]);
  TEST_ASSERT_EQUAL_HEX8(0x22u, received[1]);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_pending_count());
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_mismatch_count());
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_hal_i2c_transfer_count());
  TEST_ASSERT_EQUAL_UINT32(25u, fake_esy_hal_i2c_last_timeout());
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_i2c_transfer(&bus, &transfer));
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_hal_i2c_mismatch_count());
}

void test_i2c_fake_consumes_mismatches_without_publishing_read_data(void) {
  esy_i2c_t bus = {0};
  initialize(&bus);
  const uint8_t command = 2u, response = 3u;
  uint8_t received = 0xa5u;
  esy_i2c_xfer_t transfer = {.addr = 0x40u, .tx = &command, .tx_len = 1u,
      .rx = &received, .rx_len = 1u, .timeout_ms = 10u};
  for (unsigned variant = 0u; variant < 5u; ++variant) {
    uint8_t expected_command = variant == 0u ? 1u : command;
    uint16_t address = variant == 1u ? 0x41u : 0x40u;
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
        address, &expected_command, 1u, &response, variant == 2u ? 0u : 1u, 2));
    if (variant == 3u) fake_esy_hal_i2c_expect_timeout(11u);
    if (variant == 4u) fake_esy_hal_i2c_expect_repeated_start(true);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_i2c_transfer(&bus, &transfer));
    TEST_ASSERT_EQUAL_HEX8(0xa5u, received);
  }
  TEST_ASSERT_EQUAL_UINT(5u, fake_esy_hal_i2c_mismatch_count());
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_pending_count());
}

void test_i2c_fake_preserves_errors_and_copies_only_completed_receive_bytes(void) {
  esy_i2c_t bus = {0};
  initialize(&bus);
  const uint8_t command = 0u, response[] = {1u, 2u};
  uint8_t received[] = {0xa5u, 0xa5u};
  esy_i2c_xfer_t transfer = {.addr = 0x40u, .tx = &command, .tx_len = 1u,
      .rx = received, .rx_len = 2u};
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      0x40u, &command, 1u, response, 2u, ESY_ERR_TIMEOUT));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_i2c_transfer(&bus, &transfer));
  TEST_ASSERT_EQUAL_HEX8(0xa5u, received[0]);
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      0x40u, &command, 1u, response, 2u, 2));
  TEST_ASSERT_EQUAL(2, esy_hal_i2c_transfer(&bus, &transfer));
  TEST_ASSERT_EQUAL_HEX8(1u, received[0]);
  TEST_ASSERT_EQUAL_HEX8(0xa5u, received[1]);
}

void test_i2c_fake_supports_read_write_lifecycle_and_ten_bit_addresses(void) {
  esy_i2c_t bus = {0};
  esy_i2c_config_t config;
  uint32_t caps = 0u;
  esy_hal_i2c_default_cfg(&config);
  config.addr_mode = ESY_I2C_ADDR_10BIT;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&bus, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_i2c_init(&bus, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_get_caps(&bus, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_I2C_CAP_MASTER_TRANSFER | ESY_I2C_CAP_REPEATED_START, caps);
  const uint8_t byte = 0x33u;
  uint8_t received = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(0x2aau, &byte, 1u, NULL, 0u, 1));
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(0x2aau, NULL, 0u, &byte, 1u, 1));
  TEST_ASSERT_EQUAL(1, esy_hal_i2c_write_blocking(&bus, 0x2aau, &byte, 1u));
  TEST_ASSERT_EQUAL(1, esy_hal_i2c_read_blocking(&bus, 0x2aau, &received, 1u));
  TEST_ASSERT_EQUAL_HEX8(byte, received);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&bus));
  TEST_ASSERT_NULL(bus.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_deinit(&bus));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_i2c_get_caps(&bus, &caps));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_i2c_read_blocking(&bus, 0x40u, &received, 1u));
}

void test_i2c_fake_rejects_invalid_buffers_and_bounded_storage_overflow(void) {
  esy_i2c_t bus = {0};
  uint8_t bytes[65] = {0};
  esy_i2c_xfer_t transfer = {0};
  esy_i2c_config_t config = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_init(&bus, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_get_caps(&bus, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_transfer(&bus, &transfer));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, fake_esy_hal_i2c_expect_transfer(0x40u, NULL, 1u, NULL, 0u, 1));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, fake_esy_hal_i2c_expect_transfer(0x400u, bytes, 1u, NULL, 0u, 1));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, fake_esy_hal_i2c_expect_transfer(0x40u, bytes, sizeof(bytes), NULL, 0u, 1));
  initialize(&bus);
  transfer.addr = 0x80u;
  transfer.tx = bytes;
  transfer.tx_len = 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_transfer(&bus, &transfer));
  transfer.addr = 0x40u;
  transfer.timeout_ms = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_i2c_transfer(&bus, &transfer));
  for (unsigned index = 0u; index < 128u; ++index) {
    TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(0x40u, bytes, 1u, NULL, 0u, 1));
  }
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, fake_esy_hal_i2c_expect_transfer(0x40u, bytes, 1u, NULL, 0u, 1));
  fake_esy_hal_i2c_reset();
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_pending_count());
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_transfer_count());
}
