/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_lmx2594.h
 * @brief Synchronous, caller-owned LMX2594 synthesizer backend.
 *
 * All calls require task/startup context and external serialization of the
 * complete operation, including the SPI bus and MUXout. No OS or heap is used.
 * SPI must already provide mode 0, eight-bit, MSB-first, manual-CS transfers;
 * SDI is MOSI and the separate MUXout pin is MISO. CE must be held high by the
 * board. This backend does not provide a logical device/PM adapter.
 * Any failed mutating I/O operation faults the handle because the register
 * sequence may be partial; preserve borrowed resources and retry deinit.
 */
#ifndef ESY_LMX2594_H
#define ESY_LMX2594_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include "esy_hal/esy_hal_spi.h"
#include "esy_middleware/freq_synth/esy_freq_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Immutable initialization inputs; hardware handles remain borrowed. */
typedef struct {
  esy_spi_t *spi; /**< Initialized SPI; retained until successful deinit. */
  uint32_t spi_speed_hz; /**< Requested SCK, 1..5000000 Hz; zero preserves speed.
                             Speed-setting errors abort init; the caller
                             verifies the actual bus speed independently. */
  esy_gpio_t *cs_gpio; /**< Initialized active-low CSB output. */
  esy_gpio_t *muxout_gpio; /**< Initialized MUXout input, also SPI MISO. */
  uint32_t cs_high_delay_us; /**< Additional CSB inactive hold per frame. */
  uint32_t ref_clk_hz; /**< OSCin, 5..200 MHz; must be present during init. */
  uint16_t r_divider; /**< PLL_R, 1..255; integral PFD of 5..100 MHz required. */
  uint16_t output_divider; /**< Fixed divisor: 1 (VCO), 2,4,6,8,12,16,24,32,48,
                               64,72,96,128,192,256,384,512,768. VCO must be
                               7.5..15 GHz, or <=11.5 GHz for divisor >=8. */
  uint64_t initial_freq_hz; /**< Initial output A frequency, at least 10 MHz. */
  bool initial_output_enabled; /**< Enable output A after initialization. */
  bool mute_till_lock; /**< Request OUT_MUTE for calibration muting. This is
                           not a substitute for measured analog settling or
                           an external RF safety interlock. */
  uint8_t charge_pump_gain; /**< CPG code: 1,3,4,5,7; default 4 is 3 mA. */
  uint8_t output_a_power; /**< OUTA_PWR code, 0..63. */
  uint8_t output_b_power; /**< OUTB_PWR code, 0..63; B initially disabled. */
} esy_lmx2594_config_t;

/**
 * @brief Fill defaults: 100 MHz reference, divide-by-four, 2 GHz, RF disabled.
 * @param[out] config Destination; NULL is ignored.
 */
void esy_lmx2594_default_config(esy_lmx2594_config_t *config);

/**
 * @brief Reset and program all required registers, wait 10 ms, then calibrate.
 *
 * First use requires `esy_freq_synth_t synth = {0}`. Invalid inputs leave it
 * unchanged; repeated init returns STATE. Failed I/O attempts deinit. Failed
 * cleanup retains a faulted binding and borrowed resources for deinit retry.
 * Success does not promise PLL lock: call wait_for_lock before relying on RF.
 * Frequency changes use automatic VCO calibration; hardware ramping, SYNC,
 * SysRef, reference multiplication and calibration bypass are not supported.
 * @param[in,out] synth Stable, caller-owned handle.
 * @param[in] config Configuration copied during the call.
 * @retval ESY_OK Registers programmed and calibration triggered.
 * @retval ESY_ERR_INVALID_ARG Invalid resource/configuration/frequency.
 * @retval ESY_ERR_STATE Existing binding.
 * @return SPI/GPIO error on initialization failure.
 */
esy_err_t esy_lmx2594_init(esy_freq_synth_t *synth,
                          const esy_lmx2594_config_t *config);

/**
 * @brief Disable both RF outputs, power down the device and release the binding.
 *
 * Borrowed SPI/GPIO remain valid through success. Failed cleanup retains a
 * faulted binding; only deinit may be retried. Never clear a retained handle.
 * Success leaves the handle zeroed and repeated deinit succeeds.
 * @param[in,out] synth Zeroed or initialized LMX2594 handle.
 * @retval ESY_OK Fully detached, including an already empty handle.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_BUSY A logical device remains registered.
 * @retval ESY_ERR_STATE Another backend owns the handle.
 * @return SPI/GPIO error on incomplete cleanup.
 */
esy_err_t esy_lmx2594_deinit(esy_freq_synth_t *synth);

/**
 * @brief Read one register and restore MUXout lock-detect mode without FCAL.
 *
 * Do not interpret MUXout as lock detect during this synchronous operation.
 * The destination is unchanged on error. Restore is attempted even after I/O
 * failure; failed restoration faults the handle and requires deinit. Any R0
 * write triggers amplitude calibration even with FCAL cleared. Therefore a
 * readback restarts the 50 us lock guard; wait for lock and analog settling
 * again before measuring RF.
 * @param[in,out] synth Initialized, non-faulted LMX2594 handle.
 * @param[in] address Register number, 0..112.
 * @param[out] value Destination register value.
 * @return OK, INVALID_ARG, STATE or propagated SPI/GPIO failure.
 */
esy_err_t esy_lmx2594_read_register(esy_freq_synth_t *synth, uint8_t address,
                                   uint16_t *value);

/**
 * @brief Check combined calibration/indirect-Vtune lock on MUXout.
 *
 * Returns false for the first 50 us following FCAL to reject the previous
 * frequency's status. Thereafter MUXout must be high with LD_TYPE=1. The call
 * does not access SPI or disturb calibration.
 * This conservative software guard is not a measured lock-time guarantee.
 * Timebase resolution and analog settling must be verified by the consumer.
 * @param[in,out] synth Initialized, non-faulted LMX2594 handle.
 * @param[out] locked Destination, unchanged on failure.
 * @return OK (including unlocked), INVALID_ARG, STATE or I/O failure.
 */
esy_err_t esy_lmx2594_is_locked(esy_freq_synth_t *synth, bool *locked);

/**
 * @brief Poll lock with 5 us delays until success or timeout.
 *
 * Uses the HAL monotonic microsecond counter; scheduling and synchronous I/O
 * may extend elapsed time. A zero timeout performs one status query. Preserve
 * ownership and externally serialize for the entire potentially blocking call.
 * @param[in,out] synth Initialized LMX2594 handle.
 * @param[in] timeout_us Deadline interval, 0..INT32_MAX microseconds.
 * @return OK, TIMEOUT, INVALID_ARG, STATE or I/O failure.
 */
esy_err_t esy_lmx2594_wait_for_lock(esy_freq_synth_t *synth,
                                  uint32_t timeout_us);

#ifdef __cplusplus
}
#endif
#endif /* ESY_LMX2594_H */
