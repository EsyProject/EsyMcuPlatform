/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/sensor/esy_sensor_sht3x.h"
#include "esy_hal/esy_hal_system.h"

#include <limits.h>
#include <string.h>

#define SHT3X_RESPONSE_BYTES 6u
#define SHT3X_DATA_BYTES 2u
#define SHT3X_CHANNELS (ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_TEMPERATURE) | \
                       ESY_SENSOR_CHANNEL_BIT(ESY_SENSOR_CHAN_HUMIDITY))

typedef struct {
  esy_i2c_t *i2c;
  uint16_t address;
  uint32_t timeout_ms;
} sensor_sht3x_ctx_t;

_Static_assert(sizeof(sensor_sht3x_ctx_t) <= ESY_SENSOR_DRIVER_BYTES,
               "SHT3x context exceeds sensor storage");
_Static_assert(_Alignof(sensor_sht3x_ctx_t) <= _Alignof(max_align_t),
               "SHT3x context exceeds sensor storage alignment");

static const uint8_t k_measure_command[SHT3X_DATA_BYTES] = {0x24u, 0x00u};
static const uint32_t k_conversion_delay_ms = 16u;

static uint8_t sht3x_crc(const uint8_t *bytes) {
  uint8_t crc = 0xffu;
  for (size_t index = 0u; index < SHT3X_DATA_BYTES; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0u; bit < 8u; ++bit) {
      const uint32_t shifted = (uint32_t)crc << 1u;
      crc = (uint8_t)((crc & 0x80u) != 0u ? shifted ^ 0x31u : shifted);
    }
  }
  return crc;
}

static esy_err_t sht3x_fetch(void *arg, esy_sensor_sample_t *sample) {
  sensor_sht3x_ctx_t state;
  memcpy(&state, arg, sizeof(state));
  const sensor_sht3x_ctx_t *ctx = &state;
  uint8_t bytes[SHT3X_RESPONSE_BYTES] = {0};
  const esy_i2c_xfer_t command = {
      .addr = ctx->address,
      .tx = k_measure_command,
      .tx_len = sizeof(k_measure_command),
      .timeout_ms = ctx->timeout_ms,
  };
  int32_t result = esy_hal_i2c_transfer(ctx->i2c, &command);
  if (result < 0) {
    return (esy_err_t)result;
  }
  if (result != (int32_t)sizeof(k_measure_command)) {
    return ESY_ERR_IO;
  }

  esy_hal_system_delay_ms(k_conversion_delay_ms);
  const esy_i2c_xfer_t response = {
      .addr = ctx->address,
      .rx = bytes,
      .rx_len = sizeof(bytes),
      .timeout_ms = ctx->timeout_ms,
  };
  result = esy_hal_i2c_transfer(ctx->i2c, &response);
  if (result < 0) {
    return (esy_err_t)result;
  }
  if (result != (int32_t)sizeof(bytes)) {
    return ESY_ERR_IO;
  }
  if (sht3x_crc(bytes) != bytes[2] || sht3x_crc(&bytes[3]) != bytes[5]) {
    return ESY_ERR_CRC_FAIL;
  }

  const uint16_t temperature = (uint16_t)((uint16_t)bytes[0] << 8u) | bytes[1];
  const uint16_t humidity = (uint16_t)((uint16_t)bytes[3] << 8u) | bytes[4];
  const int64_t humidity_micro = INT64_C(100000000) * humidity / UINT16_MAX;
  sample->channels = SHT3X_CHANNELS;
  sample->micro[ESY_SENSOR_CHAN_TEMPERATURE] =
      INT64_C(-45000000) + INT64_C(175000000) * temperature / UINT16_MAX;
  sample->micro[ESY_SENSOR_CHAN_HUMIDITY] = humidity_micro;
  return ESY_OK;
}

static const esy_sensor_driver_t k_sht3x_driver = {
    .channels = SHT3X_CHANNELS,
    .fetch = sht3x_fetch,
};

esy_err_t esy_sensor_sht3x_init(esy_sensor_t *sensor,
                                const esy_sensor_sht3x_config_t *config) {
  if (sensor == NULL || config == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->busy) {
    return ESY_ERR_BUSY;
  }
  if (sensor->driver != NULL) {
    return ESY_ERR_STATE;
  }
  if (config->i2c == NULL ||
      (config->address != 0x44u && config->address != 0x45u) ||
      config->timeout_ms == 0u || config->timeout_ms > INT32_MAX) {
    return ESY_ERR_INVALID_ARG;
  }

  const sensor_sht3x_ctx_t ctx = {
      .i2c = config->i2c,
      .address = config->address,
      .timeout_ms = config->timeout_ms,
  };
  memcpy(sensor->storage.bytes, &ctx, sizeof(ctx));
  return esy_sensor_init(sensor, &k_sht3x_driver, sensor->storage.bytes);
}
