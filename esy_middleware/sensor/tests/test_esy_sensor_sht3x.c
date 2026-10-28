/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/sensor/esy_sensor.h"
#include "esy_middleware/sensor/esy_sensor_sht3x.h"
#include "fake_esy_hal_i2c.h"
#include "fake_esy_hal_system.h"

#include <limits.h>
#include <string.h>

typedef struct {
  esy_i2c_t i2c;
  esy_sensor_t sensor;
  esy_sensor_sht3x_config_t config;
  esy_err_t callback_init_result;
} test_sht3x_ctx_t;

static test_sht3x_ctx_t s_ctx;

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_i2c_reset();
  fake_esy_hal_system_reset();
  esy_i2c_config_t config;
  esy_hal_i2c_default_cfg(&config);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_init(&s_ctx.i2c, &config));
  s_ctx.config.i2c = &s_ctx.i2c;
  s_ctx.config.address = 0x44u;
  s_ctx.config.timeout_ms = 25u;
}

void tearDown(void) {
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_pending_count());
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_mismatch_count());
}

static void expect_command(int32_t result) {
  const uint8_t command[] = {0x24u, 0x00u};
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      s_ctx.config.address, command, sizeof(command), NULL, 0u, result));
  fake_esy_hal_i2c_expect_timeout(s_ctx.config.timeout_ms);
  fake_esy_hal_i2c_expect_repeated_start(false);
}

static void expect_response(const uint8_t *bytes, int32_t result) {
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_i2c_expect_transfer(
      s_ctx.config.address, NULL, 0u, bytes, 6u, result));
  fake_esy_hal_i2c_expect_timeout(s_ctx.config.timeout_ms);
  fake_esy_hal_i2c_expect_repeated_start(false);
}

void test_sht3x_rejects_invalid_configuration_without_bus_access(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_sht3x_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_sht3x_init(&s_ctx.sensor, NULL));
  s_ctx.config.i2c = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.i2c = &s_ctx.i2c;
  s_ctx.config.address = 0x46u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.address = 0x44u;
  s_ctx.config.timeout_ms = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  s_ctx.config.timeout_ms = (uint32_t)INT32_MAX + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  TEST_ASSERT_NULL(s_ctx.sensor.driver);
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_transfer_count());
}

void test_sht3x_fetch_checks_crc_and_converts_known_words(void) {
  const uint8_t response[] = {0xbeu, 0xefu, 0x92u, 0x80u, 0x00u, 0xa2u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_i2c_transfer_count());
  expect_command(2);
  expect_response(response, 6);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  int64_t value = 0;
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(
      &s_ctx.sensor, ESY_SENSOR_CHAN_TEMPERATURE, &value));
  TEST_ASSERT_EQUAL_INT64(INT64_C(85523002), value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(
      &s_ctx.sensor, ESY_SENSOR_CHAN_HUMIDITY, &value));
  TEST_ASSERT_EQUAL_INT64(INT64_C(50000762), value);
  TEST_ASSERT_EQUAL_UINT32(16u, fake_esy_hal_system_get_ctx()->last_delay_ms);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_attr_set(
      &s_ctx.sensor, ESY_SENSOR_CHAN_TEMPERATURE,
      ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, INT64_C(1000000)));
}

void test_sht3x_measurement_endpoints_and_explicit_fetch_survive_tick_wrap(void) {
  const uint8_t zero[] = {0x00u, 0x00u, 0x81u, 0x00u, 0x00u, 0x81u};
  const uint8_t full[] = {0xffu, 0xffu, 0xacu, 0xffu, 0xffu, 0xacu};
  s_ctx.config.address = 0x45u;
  s_ctx.config.timeout_ms = INT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  fake_esy_hal_system_get_ctx()->tick_ms = UINT32_MAX - 7u;
  expect_command(2);
  expect_response(zero, 6);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL_UINT32(8u, fake_esy_hal_system_get_ctx()->tick_ms);
  int64_t value = 0;
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(
      &s_ctx.sensor, ESY_SENSOR_CHAN_TEMPERATURE, &value));
  TEST_ASSERT_EQUAL_INT64(INT64_C(-45000000), value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(
      &s_ctx.sensor, ESY_SENSOR_CHAN_HUMIDITY, &value));
  TEST_ASSERT_EQUAL_INT64(0, value);
  fake_esy_hal_system_get_ctx()->tick_ms = 8u;
  TEST_ASSERT_EQUAL_UINT(2u, fake_esy_hal_i2c_transfer_count());
  expect_command(2);
  expect_response(full, 6);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(
      &s_ctx.sensor, ESY_SENSOR_CHAN_TEMPERATURE, &value));
  TEST_ASSERT_EQUAL_INT64(INT64_C(130000000), value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(
      &s_ctx.sensor, ESY_SENSOR_CHAN_HUMIDITY, &value));
  TEST_ASSERT_EQUAL_INT64(INT64_C(100000000), value);
}

void test_sht3x_rejects_crc_failure_in_either_word_and_discards_cached_sample(void) {
  uint8_t response[] = {0x00u, 0x00u, 0x81u, 0x00u, 0x00u, 0x81u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  expect_command(2);
  expect_response(response, 6);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  response[2] = 0x80u;
  expect_command(2);
  expect_response(response, 6);
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sensor_sample_fetch(&s_ctx.sensor));
  int64_t value = 123;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(
      &s_ctx.sensor, ESY_SENSOR_CHAN_TEMPERATURE, &value));
  TEST_ASSERT_EQUAL_INT64(123, value);
  response[2] = 0x81u;
  response[5] = 0x80u;
  expect_command(2);
  expect_response(response, 6);
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sensor_sample_fetch(&s_ctx.sensor));
}

void test_sht3x_propagates_transfer_errors_and_rejects_short_transfers(void) {
  const uint8_t response[] = {0x00u, 0x00u, 0x81u, 0x00u, 0x00u, 0x81u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  expect_command(ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->last_delay_ms);
  expect_command(1);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_sample_fetch(&s_ctx.sensor));
  expect_command(2);
  expect_response(response, ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sensor_sample_fetch(&s_ctx.sensor));
  expect_command(2);
  expect_response(response, 5);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_sample_fetch(&s_ctx.sensor));
}

void test_sht3x_repeated_init_preserves_context_and_deinit_allows_reuse(void) {
  const uint8_t response[] = {0x00u, 0x00u, 0x81u, 0x00u, 0x00u, 0x81u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  esy_sensor_sht3x_config_t other = s_ctx.config;
  other.address = 0x45u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_sht3x_init(&s_ctx.sensor, &other));
  expect_command(2);
  expect_response(response, 6);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  uint32_t caps = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_i2c_get_caps(&s_ctx.i2c, &caps));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sht3x_init(&s_ctx.sensor, &other));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
}

static void reinitialize_during_callback(const esy_sensor_trigger_t *trigger,
                                         const esy_sensor_sample_t *sample,
                                         void *arg) {
  (void)trigger;
  (void)sample;
  test_sht3x_ctx_t *ctx = arg;
  ctx->callback_init_result = esy_sensor_sht3x_init(&ctx->sensor, &ctx->config);
}

void test_sht3x_reentry_during_fetch_callback_preserves_provider(void) {
  const uint8_t response[] = {0x00u, 0x00u, 0x81u, 0x00u, 0x00u, 0x81u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sht3x_init(&s_ctx.sensor, &s_ctx.config));
  const esy_sensor_trigger_t trigger = {
      .type = ESY_SENSOR_TRIGGER_DATA_READY,
      .channel = ESY_SENSOR_CHAN_TEMPERATURE,
  };
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(
      &s_ctx.sensor, &trigger, reinitialize_during_callback, &s_ctx));
  expect_command(2);
  expect_response(response, 6);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.callback_init_result);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
}
