/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/sensor/esy_sensor.h"
#include "esy_middleware/sensor/esy_sensor_ina219.h"
#include "fake_esy_hal_i2c.h"

#include <limits.h>
#include <string.h>

typedef struct {
  esy_i2c_t i2c;
  esy_sensor_t sensor;
  esy_sensor_ina219_config_t config;
  esy_err_t callback_init_result;
} test_ina219_ctx_t;

static test_ina219_ctx_t s_ctx;

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_i2c_reset();
  esy_i2c_config_t config;
  esy_hal_i2c_default_cfg(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&s_ctx.i2c, &config));
  s_ctx.config.i2c = &s_ctx.i2c;
  s_ctx.config.address = 0x40u;
  s_ctx.config.timeout_ms = 25u;
  s_ctx.config.shunt_microohm = 100000u;
  s_ctx.config.current_lsb_microamp = 100u;
}

void tearDown(void) {
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_pending_count());
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_mismatch_count());
}

static void expect_write(uint8_t reg, uint16_t value, int32_t result) {
  const uint8_t bytes[] = {reg, (uint8_t)(value >> 8u), (uint8_t)value};
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      s_ctx.config.address, bytes, sizeof(bytes), NULL, 0u, result));
  fake_esy_hal_i2c_expect_timeout(s_ctx.config.timeout_ms);
  fake_esy_hal_i2c_expect_repeated_start(false);
}

static void expect_read(uint8_t reg, uint16_t value, int32_t result) {
  const uint8_t bytes[] = {(uint8_t)(value >> 8u), (uint8_t)value};
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      s_ctx.config.address, &reg, sizeof(reg), bytes, sizeof(bytes), result));
  fake_esy_hal_i2c_expect_timeout(s_ctx.config.timeout_ms);
  fake_esy_hal_i2c_expect_repeated_start(true);
}

static void initialize(uint16_t calibration) {
  expect_write(0x00u, 0x399fu, 3);
  expect_write(0x05u, calibration, 3);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
}

static void expect_sample(uint16_t bus, uint16_t shunt, uint16_t current,
                          uint16_t power) {
  expect_read(0x02u, bus, 3);
  expect_read(0x01u, shunt, 3);
  expect_read(0x04u, current, 3);
  expect_read(0x03u, power, 3);
}

static void assert_channel(esy_sensor_channel_t channel, int64_t expected) {
  int64_t value = 0;
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(&s_ctx.sensor, channel, &value));
  TEST_ASSERT_EQUAL_INT64(expected, value);
}

void test_ina219_validates_pointers_address_timeout_and_positive_calibration(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, NULL));
  s_ctx.config.i2c = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.i2c = &s_ctx.i2c;
  s_ctx.config.address = 0x3fu;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.address = 0x50u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.address = 0x40u;
  s_ctx.config.timeout_ms = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.timeout_ms = (uint32_t)INT32_MAX + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.timeout_ms = 25u;
  s_ctx.config.shunt_microohm = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.shunt_microohm = 100000u;
  s_ctx.config.current_lsb_microamp = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  TEST_ASSERT_NULL(s_ctx.sensor.driver);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_transfer_count());
}

void test_ina219_rejects_zero_or_overwide_effective_calibration_without_overflow(void) {
  s_ctx.config.shunt_microohm = UINT32_MAX;
  s_ctx.config.current_lsb_microamp = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.shunt_microohm = 1u;
  s_ctx.config.current_lsb_microamp = 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.shunt_microohm = 4000000000u;
  s_ctx.config.current_lsb_microamp = 10u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_transfer_count());
}

void test_ina219_configures_device_and_converts_signed_current_and_shunt(void) {
  initialize(4096u);
  expect_sample(0x5dc2u, 0xfc18u, 0xd8f0u, 3000u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  assert_channel(ESY_SENSOR_CHAN_VOLTAGE, INT64_C(12000000));
  assert_channel(ESY_SENSOR_CHAN_SHUNT_VOLTAGE, INT64_C(-10000));
  assert_channel(ESY_SENSOR_CHAN_CURRENT, INT64_C(-1000000));
  assert_channel(ESY_SENSOR_CHAN_POWER, INT64_C(6000000));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_attr_set(
      &s_ctx.sensor, ESY_SENSOR_CHAN_CURRENT, ESY_SENSOR_ATTR_FULL_SCALE, 1000000));
}

void test_ina219_uses_even_quantized_calibration_for_actual_physical_scaling(void) {
  s_ctx.config.current_lsb_microamp = 99u;
  initialize(4136u);
  expect_sample(0x0002u, 0x0000u, 10000u, 3000u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  assert_channel(ESY_SENSOR_CHAN_CURRENT, INT64_C(990328));
  assert_channel(ESY_SENSOR_CHAN_POWER, INT64_C(5941972));
}

void test_ina219_handles_maximum_raw_values_with_wide_scaling_arithmetic(void) {
  s_ctx.config.shunt_microohm = 1u;
  s_ctx.config.current_lsb_microamp = 1000000000u;
  s_ctx.config.address = 0x4fu;
  s_ctx.config.timeout_ms = INT32_MAX;
  initialize(40u);
  expect_sample(0xfffau, 0x8000u, 0x8000u, UINT16_MAX);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  assert_channel(ESY_SENSOR_CHAN_VOLTAGE, INT64_C(32764000));
  assert_channel(ESY_SENSOR_CHAN_SHUNT_VOLTAGE, INT64_C(-327680));
  assert_channel(ESY_SENSOR_CHAN_CURRENT, INT64_C(-33554432000000));
  assert_channel(ESY_SENSOR_CHAN_POWER, INT64_C(1342156800000000));
}

void test_ina219_not_ready_and_overflow_reject_sample_and_do_not_read_power(void) {
  initialize(4096u);
  expect_sample(0x5dc2u, 1u, 1u, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  expect_read(0x02u, 0x5dc0u, 3);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_sample_fetch(&s_ctx.sensor));
  int64_t value = 42;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(
      &s_ctx.sensor, ESY_SENSOR_CHAN_CURRENT, &value));
  TEST_ASSERT_EQUAL_INT64(42, value);
  expect_read(0x02u, 0x5dc3u, 3);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_sample_fetch(&s_ctx.sensor));
}

void test_ina219_fetch_propagates_bus_failure_at_each_register_and_short_read(void) {
  static const uint8_t k_registers[] = {0x02u, 0x01u, 0x04u, 0x03u};
  initialize(4096u);
  for (size_t failure = 0u; failure < sizeof(k_registers); ++failure) {
    for (size_t index = 0u; index < failure; ++index) {
      expect_read(k_registers[index], index == 0u ? 0x0002u : 0u, 3);
    }
    expect_read(k_registers[failure], 0u, ESY_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sensor_sample_fetch(&s_ctx.sensor));
  }
  expect_read(0x02u, 0x0002u, 2);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_sample_fetch(&s_ctx.sensor));
}

void test_ina219_init_failure_releases_references_and_allows_retry(void) {
  expect_write(0x00u, 0x399fu, ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  TEST_ASSERT_NULL(s_ctx.sensor.driver);
  TEST_ASSERT_NULL(s_ctx.sensor.ctx);
  expect_write(0x00u, 0x399fu, 3);
  expect_write(0x05u, 4096u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  TEST_ASSERT_NULL(s_ctx.sensor.driver);
  TEST_ASSERT_NULL(s_ctx.sensor.ctx);
  expect_write(0x00u, 0x399fu, 2);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_ina219_init(&s_ctx.sensor, &s_ctx.config));
  initialize(4096u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  uint32_t caps = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_get_caps(&s_ctx.i2c, &caps));
  initialize(4096u);
}

void test_ina219_repeated_init_preserves_live_context_and_hardware(void) {
  initialize(4096u);
  esy_sensor_ina219_config_t other = s_ctx.config;
  other.address = 0x41u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_ina219_init(&s_ctx.sensor, &other));
  TEST_ASSERT_EQUAL_UINT(2u, fake_esy_hal_i2c_transfer_count());
  expect_sample(0x0002u, 0u, 100u, 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  assert_channel(ESY_SENSOR_CHAN_CURRENT, INT64_C(10000));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  TEST_ASSERT_EQUAL_UINT(6u, fake_esy_hal_i2c_transfer_count());
}

static void reinitialize_during_callback(const esy_sensor_trigger_t *trigger,
                                         const esy_sensor_sample_t *sample,
                                         void *arg) {
  (void)trigger;
  (void)sample;
  test_ina219_ctx_t *ctx = arg;
  ctx->callback_init_result = esy_sensor_ina219_init(&ctx->sensor, &ctx->config);
}

void test_ina219_reentry_during_fetch_callback_does_not_write_device(void) {
  initialize(4096u);
  const esy_sensor_trigger_t trigger = {
      .type = ESY_SENSOR_TRIGGER_DATA_READY,
      .channel = ESY_SENSOR_CHAN_CURRENT,
  };
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(
      &s_ctx.sensor, &trigger, reinitialize_during_callback, &s_ctx));
  expect_sample(0x0002u, 0u, 0u, 0u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.callback_init_result);
  TEST_ASSERT_EQUAL_UINT(6u, fake_esy_hal_i2c_transfer_count());
}
