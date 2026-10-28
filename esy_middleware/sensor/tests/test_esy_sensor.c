/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "esy_middleware/sensor/esy_sensor.h"

#define TEST_CHANNEL ESY_SENSOR_CHAN_TEMPERATURE
#define TEST_MASK ESY_SENSOR_CHANNEL_BIT(TEST_CHANNEL)

typedef struct {
  esy_sensor_t sensor;
  esy_sensor_driver_t driver;
  esy_sensor_sample_t next;
  esy_err_t fetch_error;
  esy_err_t attr_error;
  esy_err_t cleanup_error;
  int64_t attribute;
  unsigned acquisitions;
  unsigned cleanups;
  unsigned callbacks;
  esy_sensor_trigger_t event;
  esy_sensor_sample_t captured;
} sensor_test_ctx_t;

static sensor_test_ctx_t s_ctx;

static esy_err_t acquire(void *arg, esy_sensor_sample_t *sample) {
  sensor_test_ctx_t *ctx = arg;
  ++ctx->acquisitions;
  *sample = ctx->next;
  return ctx->fetch_error;
}

static esy_err_t set_attr(void *arg, esy_sensor_channel_t channel,
                          esy_sensor_attribute_t attr, int64_t value) {
  sensor_test_ctx_t *ctx = arg;
  TEST_ASSERT_EQUAL(TEST_CHANNEL, channel);
  TEST_ASSERT_EQUAL(ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, attr);
  ctx->attribute = value;
  return ctx->attr_error;
}

static esy_err_t get_attr(void *arg, esy_sensor_channel_t channel,
                          esy_sensor_attribute_t attr, int64_t *value) {
  sensor_test_ctx_t *ctx = arg;
  TEST_ASSERT_EQUAL(TEST_CHANNEL, channel);
  TEST_ASSERT_EQUAL(ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, attr);
  *value = ctx->attribute;
  return ctx->attr_error;
}

static esy_err_t cleanup(void *arg) {
  sensor_test_ctx_t *ctx = arg;
  ++ctx->cleanups;
  return ctx->cleanup_error;
}

static void capture(const esy_sensor_trigger_t *trigger,
                     const esy_sensor_sample_t *sample, void *arg) {
  int64_t value;
  uint32_t channels;
  sensor_test_ctx_t *ctx = arg;
  ++ctx->callbacks;
  ctx->event = *trigger;
  ctx->captured = *sample;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_sample_fetch(&ctx->sensor));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_get_channels(&ctx->sensor, &channels));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_channel_get(&ctx->sensor, TEST_CHANNEL, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_trigger_set(&ctx->sensor, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_attr_set(&ctx->sensor, TEST_CHANNEL,
                      ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, 1));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_attr_get(&ctx->sensor, TEST_CHANNEL,
                      ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_init(&ctx->sensor, &ctx->driver, ctx));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sensor_deinit(&ctx->sensor));
}

static void attach(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_init(&s_ctx.sensor, &s_ctx.driver, &s_ctx));
}

static void fetch_value(int64_t value) {
  s_ctx.next.micro[TEST_CHANNEL] = value;
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_sample_fetch(&s_ctx.sensor));
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.driver = (esy_sensor_driver_t){.channels = TEST_MASK, .fetch = acquire,
      .attr_set = set_attr, .attr_get = get_attr, .deinit = cleanup};
  s_ctx.next.channels = TEST_MASK;
}

void tearDown(void) {
}

void test_provider_attachment_validates_arguments_and_channels(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_init(NULL, &s_ctx.driver, &s_ctx));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_init(&s_ctx.sensor, NULL, &s_ctx));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_init(&s_ctx.sensor, &s_ctx.driver, NULL));
  s_ctx.driver.channels = 0;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_init(&s_ctx.sensor, &s_ctx.driver, &s_ctx));
  s_ctx.driver.channels = UINT32_C(0x80000000);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_init(&s_ctx.sensor, &s_ctx.driver, &s_ctx));
  s_ctx.driver.channels = TEST_MASK;
  s_ctx.driver.fetch = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_init(&s_ctx.sensor, &s_ctx.driver, &s_ctx));
  TEST_ASSERT_NULL(s_ctx.sensor.driver);
  s_ctx.driver.fetch = acquire;
  attach();
}

void test_lifecycle_preserves_live_handle_and_retries_failed_cleanup(void) {
  attach();
  fetch_value(-1250000);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_init(&s_ctx.sensor, &s_ctx.driver, &s_ctx));
  TEST_ASSERT_EQUAL(1, s_ctx.acquisitions);
  s_ctx.cleanup_error = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_deinit(&s_ctx.sensor));
  TEST_ASSERT_EQUAL_PTR(&s_ctx.driver, s_ctx.sensor.driver);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_init(&s_ctx.sensor, &s_ctx.driver, &s_ctx));
  s_ctx.cleanup_error = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  TEST_ASSERT_NULL(s_ctx.sensor.driver);
  TEST_ASSERT_EQUAL(2, s_ctx.cleanups);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(2, s_ctx.cleanups);
  attach();
}

void test_null_and_detached_handle_operations_fail_without_output_writes(void) {
  int64_t value = 4321;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_sample_fetch(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_channel_get(NULL, TEST_CHANNEL, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(&s_ctx.sensor, TEST_CHANNEL, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_attr_get(&s_ctx.sensor, TEST_CHANNEL,
                                      ESY_SENSOR_ATTR_FULL_SCALE, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_trigger_set(&s_ctx.sensor, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_attr_set(&s_ctx.sensor, TEST_CHANNEL,
                                     ESY_SENSOR_ATTR_FULL_SCALE, 1));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_attr_get(&s_ctx.sensor, TEST_CHANNEL,
                                     ESY_SENSOR_ATTR_FULL_SCALE, &value));
  TEST_ASSERT_EQUAL_INT64(4321, value);
}

void test_channel_get_uses_cached_signed_physical_values_without_io(void) {
  int64_t value = 123;
  attach();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(&s_ctx.sensor, TEST_CHANNEL, &value));
  TEST_ASSERT_EQUAL_INT64(123, value);
  fetch_value(-1250000);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_get(&s_ctx.sensor, TEST_CHANNEL, &value));
  TEST_ASSERT_EQUAL_INT64(-1250000, value);
  TEST_ASSERT_EQUAL(1, s_ctx.acquisitions);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_channel_get(&s_ctx.sensor, TEST_CHANNEL, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_channel_get(&s_ctx.sensor, (esy_sensor_channel_t)-1, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_channel_get(&s_ctx.sensor, ESY_SENSOR_CHAN_COUNT, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_channel_get(&s_ctx.sensor, ESY_SENSOR_CHAN_CURRENT, &value));
  TEST_ASSERT_EQUAL_INT64(-1250000, value);
}

void test_supported_channel_query_requires_live_handle_and_preserves_errors(void) {
  uint32_t channels = 123;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_get_channels(NULL, &channels));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_get_channels(&s_ctx.sensor, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_get_channels(&s_ctx.sensor, &channels));
  TEST_ASSERT_EQUAL_UINT32(123, channels);
  attach();
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_get_channels(&s_ctx.sensor, &channels));
  TEST_ASSERT_EQUAL_UINT32(TEST_MASK, channels);
  TEST_ASSERT_EQUAL(0, s_ctx.acquisitions);
}

void test_failed_and_malformed_acquisitions_invalidate_cache(void) {
  int64_t value = 99;
  attach();
  fetch_value(1);
  s_ctx.fetch_error = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(&s_ctx.sensor, TEST_CHANNEL, &value));
  s_ctx.fetch_error = ESY_OK;
  s_ctx.next.channels = 0;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_sample_fetch(&s_ctx.sensor));
  s_ctx.next.channels = TEST_MASK | ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_CURRENT);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL_INT64(99, value);
  s_ctx.next.channels = TEST_MASK;
  fetch_value(2);
}

void test_all_channel_units_are_defined_and_invalid_units_preserve_output(void) {
  esy_sensor_unit_t unit = ESY_SENSOR_UNIT_WATT;
  for (unsigned ch = 0; ch < ESY_SENSOR_CHAN_COUNT; ++ch) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_unit((esy_sensor_channel_t)ch, &unit));
    TEST_ASSERT_LESS_OR_EQUAL(ESY_SENSOR_UNIT_WATT, unit);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_channel_unit(ESY_SENSOR_CHAN_TEMPERATURE, &unit));
  TEST_ASSERT_EQUAL(ESY_SENSOR_UNIT_CELSIUS, unit);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_channel_unit((esy_sensor_channel_t)-1, &unit));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_channel_unit(ESY_SENSOR_CHAN_COUNT, &unit));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_channel_unit(TEST_CHANNEL, NULL));
  TEST_ASSERT_EQUAL(ESY_SENSOR_UNIT_CELSIUS, unit);
}

void test_attributes_validate_ranges_and_only_publish_successful_get_results(void) {
  int64_t value = 42;
  attach();
  fetch_value(3);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_set(&s_ctx.sensor, TEST_CHANNEL,
                       ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, 100000000));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(&s_ctx.sensor, TEST_CHANNEL, &value));
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_attr_get(&s_ctx.sensor, TEST_CHANNEL,
                       ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, &value));
  TEST_ASSERT_EQUAL_INT64(100000000, value);
  s_ctx.attr_error = ESY_ERR_IO;
  s_ctx.attribute = 9;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_attr_get(&s_ctx.sensor, TEST_CHANNEL,
                       ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, &value));
  TEST_ASSERT_EQUAL_INT64(100000000, value);
  fetch_value(4);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sensor_attr_set(&s_ctx.sensor, TEST_CHANNEL,
                       ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, 200000000));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sensor_channel_get(&s_ctx.sensor, TEST_CHANNEL, &value));
}

void test_optional_attribute_and_cleanup_hooks_are_not_required(void) {
  int64_t value = 12;
  s_ctx.driver.attr_set = NULL;
  s_ctx.driver.attr_get = NULL;
  s_ctx.driver.deinit = NULL;
  attach();
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_attr_set(&s_ctx.sensor, TEST_CHANNEL,
                       ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, 1));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_attr_get(&s_ctx.sensor, TEST_CHANNEL,
                       ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, &value));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_attr_set(&s_ctx.sensor, TEST_CHANNEL,
                       ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, 0));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_attr_set(&s_ctx.sensor, TEST_CHANNEL,
                       ESY_SENSOR_ATTR_SAMPLING_FREQUENCY, -1));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_attr_set(&s_ctx.sensor, TEST_CHANNEL,
                       (esy_sensor_attribute_t)-1, 1));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_attr_get(&s_ctx.sensor, TEST_CHANNEL,
                       (esy_sensor_attribute_t)2, &value));
  TEST_ASSERT_EQUAL_INT64(12, value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
}

void test_data_ready_trigger_is_copied_and_reentry_is_rejected(void) {
  esy_sensor_trigger_t trigger = {.type = ESY_SENSOR_TRIGGER_DATA_READY, .channel = TEST_CHANNEL};
  attach();
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, capture, &s_ctx));
  trigger.channel = ESY_SENSOR_CHAN_CURRENT;
  fetch_value(5);
  fetch_value(6);
  TEST_ASSERT_EQUAL(2, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(TEST_CHANNEL, s_ctx.event.channel);
  TEST_ASSERT_EQUAL_INT64(6, s_ctx.captured.micro[TEST_CHANNEL]);
  s_ctx.fetch_error = ESY_ERR_CRC_FAIL;
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sensor_sample_fetch(&s_ctx.sensor));
  TEST_ASSERT_EQUAL(2, s_ctx.callbacks);
}

void test_high_threshold_emits_on_first_qualifying_sample_and_crossing(void) {
  const esy_sensor_trigger_t trigger = {.type = ESY_SENSOR_TRIGGER_THRESHOLD_HIGH,
      .channel = TEST_CHANNEL, .threshold_micro = 100};
  attach();
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, capture, &s_ctx));
  fetch_value(100);
  fetch_value(101);
  TEST_ASSERT_EQUAL(1, s_ctx.callbacks);
  fetch_value(99);
  fetch_value(100);
  TEST_ASSERT_EQUAL(2, s_ctx.callbacks);
}

void test_low_threshold_cancellation_and_explicit_rearm_have_no_time_dependency(void) {
  const esy_sensor_trigger_t trigger = {.type = ESY_SENSOR_TRIGGER_THRESHOLD_LOW,
      .channel = TEST_CHANNEL, .threshold_micro = -100};
  attach();
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, capture, &s_ctx));
  fetch_value(-99);
  fetch_value(-100);
  fetch_value(-101);
  TEST_ASSERT_EQUAL(1, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(&s_ctx.sensor, NULL, NULL, &s_ctx));
  for (unsigned i = 0; i < 10; ++i) {
    fetch_value(i % 2 == 0 ? 100 : -200);
  }
  TEST_ASSERT_EQUAL(1, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, capture, &s_ctx));
  fetch_value(-100);
  TEST_ASSERT_EQUAL(2, s_ctx.callbacks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_deinit(&s_ctx.sensor));
  attach();
  fetch_value(-100);
  TEST_ASSERT_EQUAL(2, s_ctx.callbacks);
}

void test_trigger_validation_preserves_existing_subscription(void) {
  esy_sensor_trigger_t trigger = {.type = ESY_SENSOR_TRIGGER_DATA_READY, .channel = TEST_CHANNEL};
  attach();
  TEST_ASSERT_EQUAL(ESY_OK, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, capture, &s_ctx));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_trigger_set(&s_ctx.sensor, NULL, capture, &s_ctx));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, NULL, &s_ctx));
  trigger.type = (esy_sensor_trigger_type_t)-1;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, capture, &s_ctx));
  trigger.type = ESY_SENSOR_TRIGGER_DATA_READY;
  trigger.channel = ESY_SENSOR_CHAN_COUNT;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, capture, &s_ctx));
  trigger.channel = ESY_SENSOR_CHAN_CURRENT;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sensor_trigger_set(&s_ctx.sensor, &trigger, capture, &s_ctx));
  fetch_value(7);
  TEST_ASSERT_EQUAL(1, s_ctx.callbacks);
}
