/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SENSOR_MPU6050_H
#define ESY_SENSOR_MPU6050_H

#include "esy_hal/esy_hal_i2c.h"
#include "esy_middleware/sensor/esy_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Borrowed MPU-6050 I2C connection; initialize the bus separately. */
typedef struct {
  esy_i2c_t *i2c; /**< Borrowed initialized master bus; serialize all bus access. */
  uint16_t address; /**< Raw, unshifted 7-bit address: 0x68 or 0x69. */
  uint32_t timeout_ms; /**< Per-transfer total budget, 1..INT32_MAX milliseconds. */
} esy_sensor_mpu6050_config_t;

/**
 * @brief Identify and configure an MPU-6050 in caller-owned sensor storage.
 *
 * Initialize sensor with {0} before its first use. One main-loop/task owner
 * serializes this blocking operation, all sensor operations and the borrowed
 * bus. No operation is ISR-safe. The bus must support repeated-start transfers.
 * No bus initialization, IRQ registration, heap allocation or task is performed.
 * WHO_AM_I must equal 0x68, including when AD0 selects bus address 0x69.
 *
 * The driver wakes all axes, waits 30 ms using the initialized system HAL,
 * selects the X gyro PLL, sets DLPF_CFG=3, +/-2 g and +/-250 degrees/second,
 * and starts at 100 Hz. It then waits one 10 ms sample period before returning.
 * Fetch reads one coherent 14-byte burst and reports
 * temperature (die temperature), acceleration X/Y/Z and angular velocity X/Y/Z
 * in the framework's micro units. Conversion truncates toward zero and applies
 * nominal datasheet sensitivity, without offset or mounting calibration.
 *
 * SAMPLING_FREQUENCY applies to all seven channels. Accepted values are
 * floor(1,000,000,000 / n) microhertz for integer n=1..256; queries return that
 * same representation of the hardware rate 1000/n Hz. The caller independently
 * controls fetch cadence. FULL_SCALE is read-only for acceleration and gyro
 * channels; every range-setting request returns NOT_SUPPORTED without I/O.
 * Software rate state changes only after a complete successful write; failed
 * writes may nevertheless change hardware, so retry configuration before use.
 *
 * Repeated init returns STATE, or BUSY during a callback, before touching the
 * device. Failed init retains no software references and permits retry, but
 * hardware may remain partially configured. Successful esy_sensor_deinit only
 * detaches the borrowed bus: it leaves the bus and sensor hardware running.
 * Keep the bus alive until that detach succeeds.
 *
 * @param sensor Caller-owned zero or detached sensor handle.
 * @param config Copied bus settings; the config itself is not retained.
 * @return INVALID_ARG for pointers, address or timeout; STATE for a live handle;
 * BUSY for reentry; IO for a different device identity or short
 * transfers; a HAL error on transport failure; otherwise OK.
 */
esy_err_t esy_sensor_mpu6050_init(esy_sensor_t *sensor,
                                 const esy_sensor_mpu6050_config_t *config);

#ifdef __cplusplus
}
#endif
#endif /* ESY_SENSOR_MPU6050_H */
