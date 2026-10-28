/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/sensor/esy_sensor_ina219.h"

#include <limits.h>
#include <string.h>

#define INA219_WORD_BYTES 2u
#define INA219_WRITE_BYTES 3u
#define INA219_CHANNELS (ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_VOLTAGE) | \
                        ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_SHUNT_VOLTAGE) | \
                        ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_CURRENT) | \
                        ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_POWER))

enum {
  INA219_REG_CONFIG = 0x00,
  INA219_REG_SHUNT = 0x01,
  INA219_REG_BUS = 0x02,
  INA219_REG_POWER = 0x03,
  INA219_REG_CURRENT = 0x04,
  INA219_REG_CALIBRATION = 0x05
};

typedef struct {
  esy_i2c_t *i2c;
  uint16_t address;
  uint32_t timeout_ms;
  uint64_t calibration_resistance;
} sensor_ina219_ctx_t;

_Static_assert(sizeof(sensor_ina219_ctx_t) <= ESY_SENSOR_DRIVER_BYTES,
               "INA219 context exceeds sensor storage");
_Static_assert(_Alignof(sensor_ina219_ctx_t) <= _Alignof(max_align_t),
               "INA219 context exceeds sensor storage alignment");

static const uint16_t k_continuous_config = 0x399fu;
static const uint16_t k_conversion_ready = 0x0002u;
static const uint16_t k_math_overflow = 0x0001u;
static const uint64_t k_calibration_numerator = UINT64_C(40960000000);

static esy_err_t ina219_write(const sensor_ina219_ctx_t *ctx, uint8_t reg,
                              uint16_t value) {
  const uint8_t bytes[INA219_WRITE_BYTES] = {reg, (uint8_t)(value >> 8u),
                                           (uint8_t)value};
  const esy_i2c_xfer_t transfer = {
      .addr = ctx->address,
      .tx = bytes,
      .tx_len = sizeof(bytes),
      .timeout_ms = ctx->timeout_ms,
  };
  const int32_t result = esy_hal_i2c_transfer(ctx->i2c, &transfer);
  if (result < 0) {
    return (esy_err_t)result;
  }
  return result == (int32_t)sizeof(bytes) ? ESY_OK : ESY_ERR_IO;
}

static esy_err_t ina219_read(const sensor_ina219_ctx_t *ctx, uint8_t reg,
                             uint16_t *value) {
  uint8_t bytes[INA219_WORD_BYTES] = {0};
  const esy_i2c_xfer_t transfer = {
      .addr = ctx->address,
      .tx = &reg,
      .tx_len = sizeof(reg),
      .rx = bytes,
      .rx_len = sizeof(bytes),
      .repeated_start = true,
      .timeout_ms = ctx->timeout_ms,
  };
  const int32_t result = esy_hal_i2c_transfer(ctx->i2c, &transfer);
  if (result < 0) {
    return (esy_err_t)result;
  }
  if (result != (int32_t)(sizeof(reg) + sizeof(bytes))) {
    return ESY_ERR_IO;
  }
  *value = (uint16_t)((uint16_t)bytes[0] << 8u) | bytes[1];
  return ESY_OK;
}

static int32_t ina219_signed_word(uint16_t word) {
  return (word & 0x8000u) != 0u ? (int32_t)word - INT32_C(65536) : (int32_t)word;
}

static esy_err_t ina219_fetch(void *arg, esy_sensor_sample_t *sample) {
  sensor_ina219_ctx_t state;
  memcpy(&state, arg, sizeof(state));
  const sensor_ina219_ctx_t *ctx = &state;
  uint16_t bus = 0u;
  uint16_t shunt = 0u;
  uint16_t current = 0u;
  uint16_t power = 0u;
  esy_err_t result = ina219_read(ctx, INA219_REG_BUS, &bus);
  if (result != ESY_OK) {
    return result;
  }
  if ((bus & k_math_overflow) != 0u) {
    return ESY_ERR_IO;
  }
  if ((bus & k_conversion_ready) == 0u) {
    return ESY_ERR_BUSY;
  }
  result = ina219_read(ctx, INA219_REG_SHUNT, &shunt);
  if (result != ESY_OK) {
    return result;
  }
  result = ina219_read(ctx, INA219_REG_CURRENT, &current);
  if (result != ESY_OK) {
    return result;
  }
  result = ina219_read(ctx, INA219_REG_POWER, &power);
  if (result != ESY_OK) {
    return result;
  }
  sample->channels = INA219_CHANNELS;
  sample->micro[ESY_SENSOR_CHAN_VOLTAGE] = (int64_t)(bus >> 3u) * INT64_C(4000);
  sample->micro[ESY_SENSOR_CHAN_SHUNT_VOLTAGE] =
      (int64_t)ina219_signed_word(shunt) * INT64_C(10);
  sample->micro[ESY_SENSOR_CHAN_CURRENT] =
      (int64_t)ina219_signed_word(current) * (int64_t)k_calibration_numerator /
      (int64_t)ctx->calibration_resistance;
  sample->micro[ESY_SENSOR_CHAN_POWER] =
      (int64_t)power * INT64_C(20) * (int64_t)k_calibration_numerator /
      (int64_t)ctx->calibration_resistance;
  return ESY_OK;
}

static const esy_sensor_driver_t k_ina219_driver = {
    .channels = INA219_CHANNELS,
    .fetch = ina219_fetch,
};

esy_err_t esy_sensor_ina219_init(esy_sensor_t *sensor,
                                 const esy_sensor_ina219_config_t *config) {
  if (sensor == NULL || config == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->busy) {
    return ESY_ERR_BUSY;
  }
  if (sensor->driver != NULL) {
    return ESY_ERR_STATE;
  }
  if (config->i2c == NULL || config->address < 0x40u || config->address > 0x4fu ||
      config->timeout_ms == 0u || config->timeout_ms > INT32_MAX ||
      config->shunt_microohm == 0u || config->current_lsb_microamp == 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Two uint32_t inputs multiply exactly into uint64_t. */
  const uint64_t denominator =
      (uint64_t)config->shunt_microohm * config->current_lsb_microamp;
  const uint64_t calibration = k_calibration_numerator / denominator;
  if (calibration < 2u || calibration > UINT16_MAX) {
    return ESY_ERR_INVALID_ARG;
  }
  const uint16_t effective_calibration = (uint16_t)calibration & 0xfffeu;
  const sensor_ina219_ctx_t ctx = {
      .i2c = config->i2c,
      .address = config->address,
      .timeout_ms = config->timeout_ms,
      .calibration_resistance =
          (uint64_t)effective_calibration * config->shunt_microohm,
  };
  esy_err_t result = ina219_write(&ctx, INA219_REG_CONFIG, k_continuous_config);
  if (result == ESY_OK) {
    result = ina219_write(&ctx, INA219_REG_CALIBRATION, effective_calibration);
  }
  if (result != ESY_OK) {
    return result;
  }
  memcpy(sensor->storage.bytes, &ctx, sizeof(ctx));
  return esy_sensor_init(sensor, &k_ina219_driver, sensor->storage.bytes);
}
