/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <string.h>
#include "esy_hal/esy_hal_system.h"
#include "esy_middleware/sensor/esy_sensor_mpu6050.h"

#define MPU6050_REG_SAMPLE_DIVIDER 0x19u
#define MPU6050_REG_CONFIG 0x1au
#define MPU6050_REG_GYRO_CONFIG 0x1bu
#define MPU6050_REG_ACCEL_CONFIG 0x1cu
#define MPU6050_REG_ACCEL_X_HIGH 0x3bu
#define MPU6050_REG_POWER_1 0x6bu
#define MPU6050_REG_POWER_2 0x6cu
#define MPU6050_REG_WHO_AM_I 0x75u
#define MPU6050_BURST_BYTES 14u
#define MPU6050_CHANNELS (ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_TEMPERATURE) | \
    ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_ACCEL_X) | \
    ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_ACCEL_Y) | \
    ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_ACCEL_Z) | \
    ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_GYRO_X) | \
    ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_GYRO_Y) | \
    ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_GYRO_Z))

typedef struct {
  esy_i2c_t *i2c;
  uint32_t timeout_ms;
  uint16_t address;
  uint8_t divider;
} sensor_mpu6050_ctx_t;

_Static_assert(sizeof(sensor_mpu6050_ctx_t) <= ESY_SENSOR_DRIVER_BYTES,
               "MPU-6050 context must fit in sensor storage");

static const uint8_t k_device_identity = 0x68u;
static const uint8_t k_internal_clock = 0u;
static const uint8_t k_x_gyro_pll = 1u;
static const uint8_t k_all_axes_active = 0u;
static const uint8_t k_low_pass_filter = 3u;
static const uint8_t k_default_range = 0u;
static const uint8_t k_default_divider = 9u;
static const uint32_t k_startup_delay_ms = 30u;
static const uint32_t k_first_sample_delay_ms = 10u;
static const int64_t k_output_microhertz = INT64_C(1000000000);
static const int64_t k_gravity_micro = INT64_C(9806650);
static const int64_t k_accel_sensitivity = INT64_C(16384);
static const int64_t k_pi_nano = INT64_C(3141592654);
static const int64_t k_gyro_divisor = INT64_C(23580000);
static const int64_t k_temperature_offset_micro = INT64_C(36530000);
static const int64_t k_temperature_sensitivity = INT64_C(340);
static const int64_t k_gyro_full_scale_micro = INT64_C(4363323);

static esy_err_t read_registers(sensor_mpu6050_ctx_t *ctx, uint8_t reg,
                                uint8_t *data, size_t length) {
  const esy_i2c_xfer_t transfer = {
      .addr = ctx->address, .tx = &reg, .tx_len = 1u,
      .rx = data, .rx_len = length, .repeated_start = true,
      .timeout_ms = ctx->timeout_ms};
  const int32_t result = esy_hal_i2c_transfer(ctx->i2c, &transfer);
  if (result < 0) {
    return (esy_err_t)result;
  }
  return (size_t)result == length + 1u ? ESY_OK : ESY_ERR_IO;
}

static esy_err_t write_register(sensor_mpu6050_ctx_t *ctx, uint8_t reg,
                               uint8_t value) {
  const uint8_t bytes[] = {reg, value};
  const esy_i2c_xfer_t transfer = {
      .addr = ctx->address, .tx = bytes, .tx_len = sizeof(bytes),
      .timeout_ms = ctx->timeout_ms};
  const int32_t result = esy_hal_i2c_transfer(ctx->i2c, &transfer);
  if (result < 0) {
    return (esy_err_t)result;
  }
  return (size_t)result == sizeof(bytes) ? ESY_OK : ESY_ERR_IO;
}

static int32_t signed_word(const uint8_t *bytes) {
  const int32_t word = (int32_t)(((uint32_t)bytes[0] << 8u) | bytes[1]);
  return word >= 32768 ? word - 65536 : word;
}

static esy_err_t fetch(void *state, esy_sensor_sample_t *sample) {
  sensor_mpu6050_ctx_t ctx;
  memcpy(&ctx, state, sizeof(ctx));
  uint8_t bytes[MPU6050_BURST_BYTES];
  const esy_err_t result = read_registers(&ctx, MPU6050_REG_ACCEL_X_HIGH,
                                        bytes, sizeof(bytes));
  if (result != ESY_OK) {
    return result;
  }
  esy_sensor_sample_t acquired = {.channels = MPU6050_CHANNELS};
  for (size_t axis = 0u; axis < 3u; ++axis) {
    acquired.micro[ESY_SENSOR_CHAN_ACCEL_X + axis] =
        signed_word(&bytes[2u * axis]) * k_gravity_micro / k_accel_sensitivity;
    acquired.micro[ESY_SENSOR_CHAN_GYRO_X + axis] =
        signed_word(&bytes[8u + 2u * axis]) * k_pi_nano / k_gyro_divisor;
  }
  acquired.micro[ESY_SENSOR_CHAN_TEMPERATURE] =
      signed_word(&bytes[6u]) * ESY_SENSOR_MICRO / k_temperature_sensitivity +
      k_temperature_offset_micro;
  *sample = acquired;
  return ESY_OK;
}

static esy_err_t attr_set(void *state, esy_sensor_channel_t channel,
                         esy_sensor_attribute_t attr, int64_t micro) {
  (void)channel;
  sensor_mpu6050_ctx_t ctx;
  memcpy(&ctx, state, sizeof(ctx));
  if (attr != ESY_SENSOR_ATTR_SAMPLING_FREQUENCY) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  for (uint16_t divisor = 1u; divisor <= 256u; ++divisor) {
    if (micro == k_output_microhertz / divisor) {
      const uint8_t divider = (uint8_t)(divisor - 1u);
      const esy_err_t result = write_register(&ctx, MPU6050_REG_SAMPLE_DIVIDER,
                                             divider);
      if (result == ESY_OK) {
        ctx.divider = divider;
        memcpy(state, &ctx, sizeof(ctx));
      }
      return result;
    }
  }
  return ESY_ERR_NOT_SUPPORTED;
}

static esy_err_t attr_get(void *state, esy_sensor_channel_t channel,
                         esy_sensor_attribute_t attr, int64_t *micro) {
  sensor_mpu6050_ctx_t ctx;
  memcpy(&ctx, state, sizeof(ctx));
  if (attr == ESY_SENSOR_ATTR_SAMPLING_FREQUENCY) {
    *micro = k_output_microhertz / ((int64_t)ctx.divider + 1);
    return ESY_OK;
  }
  if (attr == ESY_SENSOR_ATTR_FULL_SCALE) {
    if (channel >= ESY_SENSOR_CHAN_ACCEL_X && channel <= ESY_SENSOR_CHAN_ACCEL_Z) {
      *micro = 2 * k_gravity_micro;
      return ESY_OK;
    }
    if (channel >= ESY_SENSOR_CHAN_GYRO_X && channel <= ESY_SENSOR_CHAN_GYRO_Z) {
      *micro = k_gyro_full_scale_micro;
      return ESY_OK;
    }
  }
  return ESY_ERR_NOT_SUPPORTED;
}

static const esy_sensor_driver_t k_driver = {
    .channels = MPU6050_CHANNELS, .fetch = fetch,
    .attr_set = attr_set, .attr_get = attr_get};

static esy_err_t configure(sensor_mpu6050_ctx_t *ctx) {
  uint8_t identity = 0u;
  esy_err_t result = read_registers(ctx, MPU6050_REG_WHO_AM_I, &identity, 1u);
  if (result != ESY_OK) {
    return result;
  }
  if (identity != k_device_identity) {
    return ESY_ERR_IO;
  }
  result = write_register(ctx, MPU6050_REG_POWER_1, k_internal_clock);
  if (result == ESY_OK) {
    result = write_register(ctx, MPU6050_REG_POWER_2, k_all_axes_active);
  }
  if (result != ESY_OK) {
    return result;
  }
  /* A synchronous HAL wait owns startup timing; no timestamp survives it. */
  esy_hal_system_delay_ms(k_startup_delay_ms);
  result = write_register(ctx, MPU6050_REG_POWER_1, k_x_gyro_pll);
  if (result == ESY_OK) {
    result = write_register(ctx, MPU6050_REG_CONFIG, k_low_pass_filter);
  }
  if (result == ESY_OK) {
    result = write_register(ctx, MPU6050_REG_GYRO_CONFIG, k_default_range);
  }
  if (result == ESY_OK) {
    result = write_register(ctx, MPU6050_REG_ACCEL_CONFIG, k_default_range);
  }
  if (result == ESY_OK) {
    result = write_register(ctx, MPU6050_REG_SAMPLE_DIVIDER, k_default_divider);
  }
  if (result == ESY_OK) {
    esy_hal_system_delay_ms(k_first_sample_delay_ms);
  }
  return result;
}

esy_err_t esy_sensor_mpu6050_init(esy_sensor_t *sensor,
                                 const esy_sensor_mpu6050_config_t *config) {
  if (sensor == NULL || config == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->busy) {
    return ESY_ERR_BUSY;
  }
  if (sensor->driver != NULL) {
    return ESY_ERR_STATE;
  }
  if (config->i2c == NULL || (config->address != 0x68u && config->address != 0x69u) ||
      config->timeout_ms == 0u || config->timeout_ms > INT32_MAX) {
    return ESY_ERR_INVALID_ARG;
  }
  sensor_mpu6050_ctx_t ctx = {
      .i2c = config->i2c, .timeout_ms = config->timeout_ms,
      .address = config->address, .divider = k_default_divider};
  const esy_err_t result = configure(&ctx);
  if (result != ESY_OK) {
    memset(sensor, 0, sizeof(*sensor));
    return result;
  }
  memcpy(sensor->storage.bytes, &ctx, sizeof(ctx));
  return esy_sensor_init(sensor, &k_driver, sensor->storage.bytes);
}
