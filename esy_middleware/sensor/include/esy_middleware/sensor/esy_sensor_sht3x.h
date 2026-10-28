/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SENSOR_SHT3X_H
#define ESY_SENSOR_SHT3X_H

#include "esy_middleware/sensor/esy_sensor.h"
#include "esy_hal/esy_hal_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Borrowed bus and finite transaction budget for an SHT30/31/35-DIS. */
typedef struct {
  esy_i2c_t *i2c; /**< Initialized master bus; caller serializes all bus access. */
  uint16_t address; /**< Raw 7-bit address, either 0x44 or 0x45. */
  uint32_t timeout_ms; /**< Per-transfer budget in milliseconds, 1..INT32_MAX. */
} esy_sensor_sht3x_config_t;

/**
 * @brief Attach a synchronous SHT3x-DIS temperature/humidity provider.
 * @param sensor Caller-owned {0} handle, kept at the same address until deinit.
 * @param config Copied configuration; the initialized bus is borrowed until deinit.
 * @return INVALID_ARG for invalid pointers/address/timeout, STATE for a live
 * handle, BUSY on reentry, otherwise OK. No hardware access occurs during init.
 *
 * Call only from the serialized main-loop/task owner, never ISR context, after
 * platform system initialization and sensor power-up. The owner must ensure the
 * device is idle in single-shot mode and not used by another driver. Fetch sends
 * high-repeatability command 0x2400, blocks for a HAL 16 ms delay, then reads two
 * CRC-protected words. The delay covers the 15.5 ms worst-case conversion at low
 * supply voltage; the HAL delay must not return early or run while its timebase
 * is suspended. Each fetch costs two finite bus budgets plus delay and scheduling
 * overhead. It contains no timestamp comparison or persistent timed state.
 * Temperature uses microdegrees Celsius; humidity uses micropercent, bounded to
 * 0..100000000. CRC failures return CRC_FAIL, short transfers return IO, and HAL
 * errors propagate. Sampling cadence belongs to the caller; attributes are not
 * supported. Generic deinit only detaches the borrowed bus, with no power-down
 * command and no bus lifecycle or IRQ operations. Failed init leaves no retained
 * resources; successful deinit permits initialization again.
 */
esy_err_t esy_sensor_sht3x_init(esy_sensor_t *sensor,
                                const esy_sensor_sht3x_config_t *config);

#ifdef __cplusplus
}
#endif
#endif /* ESY_SENSOR_SHT3X_H */
