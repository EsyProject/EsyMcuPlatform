/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <string.h>
#include "unity.h"
#include "esy_middleware/sensor/esy_sensor.h"
#include "esy_middleware/sensor/esy_sensor_mpu6050.h"
#include "fake_esy_hal_i2c.h"
#include "fake_esy_hal_system.h"

#define TEST_TIMEOUT_MS 25u
#define TEST_ADDRESS 0x68u

typedef struct {
  esy_sensor_t sensor;
  esy_i2c_t bus;
  esy_sensor_mpu6050_config_t config;
  esy_err_t callback_init_result;
  size_t callback_count;
} mpu6050_test_ctx_t;

static mpu6050_test_ctx_t s_ctx;

static void expect_read(uint8_t reg, const uint8_t *bytes, size_t length,
                        int32_t result) {
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      s_ctx.config.address, &reg, 1u, bytes, length, result));
  fake_esy_hal_i2c_expect_timeout(s_ctx.config.timeout_ms);
  fake_esy_hal_i2c_expect_repeated_start(true);
}

static void expect_write(uint8_t reg, uint8_t value, int32_t result) {
  const uint8_t bytes[] = {reg, value};
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      s_ctx.config.address, bytes, sizeof(bytes), NULL, 0u, result));
  fake_esy_hal_i2c_expect_timeout(s_ctx.config.timeout_ms);
  fake_esy_hal_i2c_expect_repeated_start(false);
}

static void expect_identity(uint8_t identity, int32_t result) {
  expect_read(0x75u, &identity, 1u, result);
}

static void expect_initialization(void) {
  expect_identity(0x68u, 2);
  expect_write(0x6bu, 0u, 2);
  expect_write(0x6cu, 0u, 2);
  expect_write(0x6bu, 1u, 2);
  expect_write(0x1au, 3u, 2);
  expect_write(0x1bu, 0u, 2);
  expect_write(0x1cu, 0u, 2);
  expect_write(0x19u, 9u, 2);
}

static void init_sensor(void) {
  expect_initialization();
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
}

static void check_zero_handle(void) {
  const esy_sensor_t zero = {0};
  TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.sensor, sizeof(zero));
}

static void reenter_from_callback(const esy_sensor_trigger_t *trigger,
                                  const esy_sensor_sample_t *sample, void *arg) {
  TEST_ASSERT_NOT_NULL(trigger);
  TEST_ASSERT_NOT_NULL(sample);
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  ++s_ctx.callback_count;
  s_ctx.callback_init_result = esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_i2c_reset();
  fake_esy_hal_system_reset();
  esy_i2c_config_t bus_config;
  esy_hal_i2c_default_cfg(&bus_config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&s_ctx.bus, &bus_config));
  s_ctx.config.i2c = &s_ctx.bus;
  s_ctx.config.address = TEST_ADDRESS;
  s_ctx.config.timeout_ms = TEST_TIMEOUT_MS;
}

void tearDown(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_i2c_pending_count());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_i2c_mismatch_count());
}

void test_mpu6050_rejects_invalid_configuration_before_bus_access(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_mpu6050_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_mpu6050_init(&s_ctx.sensor, NULL));
  s_ctx.config.i2c = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.i2c = &s_ctx.bus;
  s_ctx.config.address = 0x67u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.address = 0xd0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.address = TEST_ADDRESS;
  s_ctx.config.timeout_ms = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.timeout_ms = (uint32_t)INT32_MAX + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_i2c_transfer_count());
  check_zero_handle();
}

void test_mpu6050_initialization_configures_nominal_ranges_and_rate_with_finite_waits(void) {
  init_sensor();
  TEST_ASSERT_EQUAL_UINT32(8u, fake_esy_hal_i2c_transfer_count());
  TEST_ASSERT_EQUAL_UINT32(TEST_TIMEOUT_MS, fake_esy_hal_i2c_last_timeout());
  TEST_ASSERT_EQUAL_UINT32(40u, fake_esy_hal_system_get_ctx()->tick_ms);
  TEST_ASSERT_EQUAL_UINT32(10u, fake_esy_hal_system_get_ctx()->last_delay_ms);
  int64_t value = 0;
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_get(&s_ctx.sensor,
      ESY_SENSOR_CHAN_TEMPERATURE, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, &value));
  TEST_ASSERT_EQUAL_INT64(INT64_C(100000000), value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_get(&s_ctx.sensor,
      ESY_SENSOR_CHAN_ACCEL_Y, ESY_SENSOR_ATTR_FULL_SCALE, &value));
  TEST_ASSERT_EQUAL_INT64(INT64_C(19613300), value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_get(&s_ctx.sensor,
      ESY_SENSOR_CHAN_GYRO_Z, ESY_SENSOR_ATTR_FULL_SCALE, &value));
  TEST_ASSERT_EQUAL_INT64(INT64_C(4363323), value);
}

void test_mpu6050_high_address_still_requires_identity_68_and_accepts_maximum_timeout(void) {
  s_ctx.config.address = 0x69u;
  s_ctx.config.timeout_ms = INT32_MAX;
  expect_identity(0x69u, 2);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  check_zero_handle();
  init_sensor();
  TEST_ASSERT_EQUAL_UINT32(INT32_MAX, fake_esy_hal_i2c_last_timeout());
}

void test_mpu6050_identity_transport_errors_and_short_reads_leave_reusable_zero_handle(void) {
  expect_identity(0x68u, ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  check_zero_handle();
  expect_identity(0x68u, 1);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  check_zero_handle();
  expect_identity(0x70u, 2);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  check_zero_handle();
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->tick_ms);
  init_sensor();
}

void test_mpu6050_initialization_stops_on_short_or_failed_configuration_write(void) {
  expect_identity(0x68u, 2);
  expect_write(0x6bu, 0u, 1);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  check_zero_handle();
  expect_identity(0x68u, 2);
  expect_write(0x6bu, 0u, 2);
  expect_write(0x6cu, 0u, ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  check_zero_handle();
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->tick_ms);
  init_sensor();
}

void test_mpu6050_repeated_init_preserves_live_state_and_deinit_borrows_bus(void) {
  init_sensor();
  esy_sensor_t snapshot = s_ctx.sensor;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_mpu6050_init(&s_ctx.sensor, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(&snapshot, &s_ctx.sensor, sizeof(snapshot));
  TEST_ASSERT_EQUAL_UINT32(8u, fake_esy_hal_i2c_transfer_count());
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  check_zero_handle();
  uint32_t capabilities = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_get_caps(&s_ctx.bus, &capabilities));
  TEST_ASSERT_BITS_HIGH(ESY_I2C_CAP_MASTER_TRANSFER, capabilities);
  TEST_ASSERT_EQUAL_UINT32(8u, fake_esy_hal_i2c_transfer_count());
  init_sensor();
}

void test_mpu6050_callback_reentry_cannot_reinitialize_or_write_the_device(void) {
  init_sensor();
  const esy_sensor_trigger_t trigger = {
      .type = ESY_SENSOR_TRIGGER_DATA_READY, .channel = ESY_SENSOR_CHAN_ACCEL_X};
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(&s_ctx.sensor, &trigger,
      reenter_from_callback, &s_ctx));
  const uint8_t bytes[14] = {0};
  expect_read(0x3bu, bytes, sizeof(bytes), 15);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.callback_count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.callback_init_result);
  TEST_ASSERT_EQUAL_UINT32(9u, fake_esy_hal_i2c_transfer_count());
}

void test_mpu6050_burst_converts_positive_negative_and_minimum_signed_values_to_si_units(void) {
  init_sensor();
  const uint8_t bytes[] = {
      0x40u, 0x00u, 0xc0u, 0x00u, 0x80u, 0x00u,
      0xf9u, 0x5cu, 0x00u, 0x83u, 0xffu, 0x7du, 0x00u, 0x00u};
  expect_read(0x3bu, bytes, sizeof(bytes), 15);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  const esy_sensor_channel_t channels[] = {
      ESY_SENSOR_CHAN_TEMPERATURE, ESY_SENSOR_CHAN_ACCEL_X,
      ESY_SENSOR_CHAN_ACCEL_Y, ESY_SENSOR_CHAN_ACCEL_Z,
      ESY_SENSOR_CHAN_GYRO_X, ESY_SENSOR_CHAN_GYRO_Y, ESY_SENSOR_CHAN_GYRO_Z};
  const int64_t expected[] = {INT64_C(31530000), INT64_C(9806650),
      INT64_C(-9806650), INT64_C(-19613300), INT64_C(17453), INT64_C(-17453), 0};
  for (size_t i = 0u; i < sizeof(channels) / sizeof(channels[0]); ++i) {
    int64_t value = INT64_MIN;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(&s_ctx.sensor, channels[i], &value));
    TEST_ASSERT_EQUAL_INT64(expected[i], value);
  }
  int64_t absent = 123;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_channel_get(&s_ctx.sensor,
      ESY_SENSOR_CHAN_CURRENT, &absent));
  TEST_ASSERT_EQUAL_INT64(123, absent);
}

void test_mpu6050_failed_or_short_fetch_invalidates_cache_and_preserves_get_output(void) {
  init_sensor();
  const uint8_t bytes[14] = {0};
  expect_read(0x3bu, bytes, sizeof(bytes), 15);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  expect_read(0x3bu, bytes, sizeof(bytes), 14);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_sample_fetch(&s_ctx.sensor));
  int64_t value = 4321;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(&s_ctx.sensor,
      ESY_SENSOR_CHAN_TEMPERATURE, &value));
  TEST_ASSERT_EQUAL_INT64(4321, value);
  expect_read(0x3bu, bytes, sizeof(bytes), ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(&s_ctx.sensor,
      ESY_SENSOR_CHAN_TEMPERATURE, &value));
  TEST_ASSERT_EQUAL_INT64(4321, value);
}

void test_mpu6050_rate_accepts_divider_limits_and_fractional_microhertz_representation(void) {
  init_sensor();
  const uint8_t dividers[] = {0u, 2u, 255u};
  const int64_t frequencies[] = {INT64_C(1000000000), INT64_C(333333333), INT64_C(3906250)};
  for (size_t i = 0u; i < sizeof(dividers); ++i) {
    expect_write(0x19u, dividers[i], 2);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_set(&s_ctx.sensor,
        ESY_SENSOR_CHAN_ACCEL_X, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, frequencies[i]));
    int64_t actual = 0;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_get(&s_ctx.sensor,
        ESY_SENSOR_CHAN_GYRO_Y, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, &actual));
    TEST_ASSERT_EQUAL_INT64(frequencies[i], actual);
  }
}

void test_mpu6050_rejects_unsupported_rates_ranges_and_channels_without_writes(void) {
  init_sensor();
  const int64_t frequencies[] = {INT64_C(3906249), INT64_C(1000000001),
      INT64_C(333333334), INT64_C(123000000)};
  for (size_t i = 0u; i < sizeof(frequencies) / sizeof(frequencies[0]); ++i) {
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_attr_set(&s_ctx.sensor,
        ESY_SENSOR_CHAN_ACCEL_Z, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, frequencies[i]));
  }
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_attr_set(&s_ctx.sensor,
      ESY_SENSOR_CHAN_ACCEL_X, ESY_SENSOR_ATTR_FULL_SCALE, INT64_C(19613300)));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_attr_set(&s_ctx.sensor,
      ESY_SENSOR_CHAN_HUMIDITY, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, INT64_C(100000000)));
  int64_t output = 123;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_attr_get(&s_ctx.sensor,
      ESY_SENSOR_CHAN_TEMPERATURE, ESY_SENSOR_ATTR_FULL_SCALE, &output));
  TEST_ASSERT_EQUAL_INT64(123, output);
  TEST_ASSERT_EQUAL_UINT32(8u, fake_esy_hal_i2c_transfer_count());
}

void test_mpu6050_failed_rate_write_retains_last_confirmed_software_rate(void) {
  init_sensor();
  expect_write(0x19u, 19u, 1);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_attr_set(&s_ctx.sensor,
      ESY_SENSOR_CHAN_GYRO_X, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, INT64_C(50000000)));
  int64_t frequency = 0;
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_get(&s_ctx.sensor,
      ESY_SENSOR_CHAN_ACCEL_Y, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, &frequency));
  TEST_ASSERT_EQUAL_INT64(INT64_C(100000000), frequency);
  expect_write(0x19u, 19u, ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sensor_attr_set(&s_ctx.sensor,
      ESY_SENSOR_CHAN_GYRO_X, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, INT64_C(50000000)));
  expect_write(0x19u, 19u, 2);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_set(&s_ctx.sensor,
      ESY_SENSOR_CHAN_TEMPERATURE, ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, INT64_C(50000000)));
}
