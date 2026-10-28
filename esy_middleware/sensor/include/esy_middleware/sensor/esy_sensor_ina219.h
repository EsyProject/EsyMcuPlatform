/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SENSOR_INA219_H
#define ESY_SENSOR_INA219_H

#include "esy_middleware/sensor/esy_sensor.h"
#include "esy_hal/esy_hal_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Borrowed INA219 bus and caller-selected external shunt calibration. */
typedef struct {
  esy_i2c_t *i2c; /**< Initialized master bus; serialize every shared bus access. */
  uint16_t address; /**< Raw 7-bit address in 0x40..0x4f. */
  uint32_t timeout_ms; /**< Per-transfer budget in milliseconds, 1..INT32_MAX. */
  uint32_t shunt_microohm; /**< Positive shunt resistance, in microohms. */
  uint32_t current_lsb_microamp; /**< Positive nominal current LSB, in microamperes. */
} esy_sensor_ina219_config_t;

/**
 * @brief Configure a synchronous INA219 voltage/current/power provider.
 * @param sensor Caller-owned {0} handle, kept at the same address until deinit.
 * @param config Copied configuration; its initialized bus is borrowed until deinit.
 * @return INVALID_ARG for invalid pointers/ranges/calibration, STATE for a live
 * handle, BUSY on reentry, IO for a short transfer, a propagated HAL error, or OK.
 *
 * The serialized main-loop/task owner calls all operations, never ISR context.
 * Init writes fixed configuration 0x399f: 32 V register range, +/-320 mV shunt
 * range, 12-bit bus/shunt ADC, continuous conversion. The electrical bus limit
 * remains 26 V. Calibration is floor(40960000000 / (shunt_microohm *
 * current_lsb_microamp)), with bit zero discarded as required by the hardware;
 * the resulting value must be 2..65534. Returned current and power use the actual
 * quantized calibration and shunt resistance, with integer division truncating
 * toward zero in micro units. Select the nominal LSB for the expected current
 * range. No board-specific shunt default is assumed.
 * Init failure leaves no retained resources, but earlier hardware writes may
 * have succeeded; no register rollback is attempted. Deinit only detaches the
 * borrowed bus, does not power down the sensor, and permits initialization again.
 *
 * Fetch reads bus, shunt, current and power registers using four finite transfer
 * budgets, without retries or a readiness wait. A missing CNVR returns BUSY;
 * OVF returns IO. Power is read last because it clears CNVR. Continuous conversion
 * can advance between register reads, so the sample is not an atomic hardware
 * snapshot. Caller cadence controls reads; attributes are not supported. HAL
 * errors propagate; any acquisition error invalidates the generic cached sample.
 * No timestamp state, background work, IRQ registration or allocation is created.
 */
esy_err_t esy_sensor_ina219_init(esy_sensor_t *sensor,
                                 const esy_sensor_ina219_config_t *config);

#ifdef __cplusplus
}
#endif
#endif /* ESY_SENSOR_INA219_H */
