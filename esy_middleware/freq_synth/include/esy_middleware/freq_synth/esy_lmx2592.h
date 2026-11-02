/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_lmx2592.h
 * @brief Synchronous, caller-owned LMX2592 synthesizer backend.
 *
 * Calls require task/startup context and external serialization of the entire
 * operation, including SPI and MUXout. No OS, heap or device/PM adapter is used.
 * Supply initialized mode-0, eight-bit, MSB-first, manual-CS SPI; SDI is MOSI
 * and MUXout is both MISO and a GPIO input. The board must hold CE high and
 * provide a valid reference before init. Failed tuning/output programming faults
 * the handle: retain all borrowed resources until deinit succeeds.
 */
#ifndef ESY_LMX2592_H
#define ESY_LMX2592_H

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
  uint32_t spi_speed_hz; /**< Requested SCK, 1..5000000 Hz; zero retains speed.
                             Speed-setting errors abort init; independently
                             verify the actual bus speed. */
  esy_gpio_t *cs_gpio; /**< Initialized active-low CSB output. */
  esy_gpio_t *muxout_gpio; /**< Initialized MUXout input, also SPI MISO. */
  uint32_t cs_high_delay_us; /**< Additional CSB inactive hold per frame. */
  uint32_t ref_clk_hz; /**< OSCin, 5..200 MHz; present before init. */
  uint16_t r_divider; /**< PLL_R, 1..255; integral-Hz PFD of 5..100 MHz. */
  uint16_t output_divider; /**< Zero selects the lowest legal divisor per tune;
                               1 selects VCO directly. Fixed divisors supported:
                               2,3,4,6,8,12,16,24,32,36,48,64,96,128,192,
                               within SNAS646G Table 7-4 frequency limits.
                               Automatic mode selects direct VCO at >=3.55 GHz.
                               Changing route briefly disables both outputs. */
  uint64_t initial_freq_hz; /**< Initial output A frequency, 20 MHz..7.1 GHz. */
  bool initial_output_enabled; /**< Enable output A after initialization. */
  uint8_t charge_pump_current; /**< Equal CP_IUP/CP_IDN bitmask, 1..31.
                                  Bit currents: 4=1.25, 3=2.5, 2=0.625,
                                  1=0.312, 0=0.156 mA before multiplier. */
  uint8_t charge_pump_multiplier; /**< CP_ICOARSE: 0 (x1) or 2 (x1.5),
                                     TI's recommended lock-detect settings. */
  uint8_t output_a_power; /**< OUTA_POW: 0..31 or 48..63; characterize pullups. */
  uint8_t output_b_power; /**< OUTB_POW: 0..31 or 48..63; B initially disabled. */
} esy_lmx2592_config_t;

/**
 * @brief Copyable, caller-owned automatic-calibration result for one frequency.
 *
 * Keep snapshots private to the physical device and initialization lifetime.
 * Invalidate them after reset, power/reference changes or hardware faults;
 * validity across temperature, supply and analog changes is caller policy.
 * Editing fields, interpolation and nearby-frequency reuse are unsupported.
 */
typedef struct {
  uint64_t frequency_hz; /**< Exact MAIN frequency used during capture. */
  uint32_t ref_clk_hz; /**< Initialization OSCin frequency. */
  uint32_t pfd_hz; /**< Initialization phase-detector frequency. */
  uint16_t output_divider; /**< Actual selected divisor, including auto mode. */
  uint16_t daciset; /**< Captured VCO_IDAC, 250..450 per SNAS646G. */
  uint8_t vco_core; /**< Captured VCO core, 1..7. */
  uint8_t capctrl; /**< Captured capacitor-bank code, 0..183. */
} esy_lmx2592_vco_calibration_t;

/**
 * @brief Fill defaults: 100 MHz reference, automatic divider, 2 GHz, RF off.
 *
 * Charge-pump bitmask 12 with multiplier 0 selects nominal 3.125 mA.
 * @param[out] config Destination; NULL is ignored.
 */
void esy_lmx2592_default_config(esy_lmx2592_config_t *config);

/**
 * @brief Reset, program the documented R/W registers, delay 10 ms and calibrate.
 *
 * First use requires `esy_freq_synth_t synth = {0}`. Invalid inputs leave it
 * unchanged; repeated init returns STATE. Failed I/O attempts deinit. Failed
 * cleanup retains a faulted binding and borrowed resources for deinit retry.
 * Success does not establish lock: call wait_for_lock before relying on RF.
 * The reference path bypasses the doubler/multiplier, PLL_N_PRE is two, and
 * frequency planning uses integer or the highest legal MASH order up to three.
 * Generic tuning always triggers automatic calibration and leaves full assist;
 * explicit exact-frequency full assist uses the APIs below. VCO doubling, phase
 * synchronization, FCAL_FAST/ACAL_FAST algorithms and PM are unsupported.
 * This chip has no LMX2594-style OUT_MUTE: RF during calibration
 * and settling must be managed by the consumer or an external interlock.
 * @param[in,out] synth Stable caller-owned, initially zeroed handle.
 * @param[in] config Configuration copied during the call.
 * @retval ESY_OK Registers programmed and calibration triggered.
 * @retval ESY_ERR_INVALID_ARG Invalid resource/configuration/frequency.
 * @retval ESY_ERR_STATE Existing binding.
 * @return Propagated SPI/GPIO failure.
 */
esy_err_t esy_lmx2592_init(esy_freq_synth_t *synth,
                          const esy_lmx2592_config_t *config);

/**
 * @brief Disable both outputs, power down and release the binding.
 *
 * Borrowed resources must remain valid through success. Failed cleanup retains
 * a faulted handle; only deinit may be retried. Never clear a retained handle.
 * Success zeroes the handle, and repeated deinit succeeds.
 * @param[in,out] synth Zeroed or initialized LMX2592 handle.
 * @retval ESY_OK Fully detached, including an already empty handle.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_BUSY Logical device still registered.
 * @retval ESY_ERR_STATE Another backend owns the handle.
 * @return SPI/GPIO failure on incomplete cleanup.
 */
esy_err_t esy_lmx2592_deinit(esy_freq_synth_t *synth);

/**
 * @brief Read a register through MUXout and restore lock detect with FCAL clear.
 *
 * MUXout is not lock detect during this synchronous operation. Destination is
 * unchanged on error. Restoration is attempted even after I/O failure; failed
 * restoration faults the handle. Because readback rewrites R0, the driver
 * conservatively restarts its software lock guard: establish lock and analog
 * settling again before measuring RF. R68..R70 contain read-only diagnostics;
 * unlisted register addresses within the accepted range have no defined fields.
 * @param[in,out] synth Initialized, non-faulted LMX2592 handle.
 * @param[in] address Register number, 0..70.
 * @param[out] value Destination register value.
 * @return OK, INVALID_ARG, STATE or propagated SPI/GPIO failure.
 */
esy_err_t esy_lmx2592_read_register(esy_freq_synth_t *synth, uint8_t address,
                                   uint16_t *value);

/**
 * @brief Capture one exact-frequency snapshot in automatic-calibration mode.
 *
 * Task context only; externally serialize the handle, SPI and MUXout. Holds any
 * registered device reference until return. Establishes lock, selects MUXout
 * readback once, waits the reference-scaled calibration guard, checks Vtune,
 * and reads R68..R70. The destination is unchanged on failure. Lock-detect
 * restoration is attempted on every exit after switching, including I/O errors.
 * Failed restoration returns STATE and faults the handle; only deinit recovers.
 * Successful restoration rearms the calibration guard even on recoverable read
 * errors/timeouts. Reestablish lock and analog settling before measuring RF.
 *
 * Uses the advancing HAL monotonic 32-bit microsecond clock, with observations
 * less than 2^31 us apart. The timeout includes initial lock, guard and reads;
 * synchronous I/O and mandatory restoration may extend elapsed time. A zero
 * timeout probes initial lock, then returns TIMEOUT without switching MUXout.
 * No allocation, snapshot cache or background worker is retained.
 * @param[in,out] synth Initialized, non-faulted LMX2592 handle.
 * @param[in] timeout_us Budget, 0..INT32_MAX microseconds.
 * @param[out] out Caller-owned snapshot, unchanged on failure.
 * @return OK, INVALID_ARG, STATE (including full-assist mode), TIMEOUT or I/O
 * failure. Invalid captured core, CAPCTRL or IDAC returns a recoverable I/O error.
 */
esy_err_t esy_lmx2592_read_vco_calibration(
    esy_freq_synth_t *synth, uint32_t timeout_us,
    esy_lmx2592_vco_calibration_t *out);

/**
 * @brief Revisit an exact captured frequency with manual VCO calibration values.
 *
 * Task context only; externally serialize the handle and complete SPI operation.
 * Holds any registered device reference until return. Snapshot frequency, OSCin,
 * PFD and actual output divisor must match the planned target exactly. Programs
 * changed frequency fields plus R19 IDAC, R22 CAPCTRL, R23 forced core and R8
 * amplitude/cap overrides. Does not write R0 or trigger FCAL. Output enables and
 * power are preserved; divider-route changes briefly mute both outputs as for
 * normal tuning. This is full assist, not the FCAL_FAST/ACAL_FAST algorithms.
 *
 * Every success rearms the existing reference-scaled calibration guard as a
 * conservative stale-status policy; no faster lock-time bound is promised.
 * Wait for lock and analog settling before measuring RF. Invalid snapshots
 * cause no SPI/GPIO I/O. Partial mutating I/O faults the binding; only deinit
 * may recover. Generic tuning clears overrides and runs automatic calibration.
 * @param[in,out] synth Initialized, non-faulted LMX2592 handle.
 * @param[in] frequency_hz Exact captured MAIN output frequency in Hz.
 * @param[in] calibration Caller-owned snapshot borrowed only during this call.
 * @return OK, INVALID_ARG, STATE or propagated I/O error.
 */
esy_err_t esy_lmx2592_set_frequency_fast(
    esy_freq_synth_t *synth, uint64_t frequency_hz,
    const esy_lmx2592_vco_calibration_t *calibration);

/**
 * @brief Check MUXout Vtune lock detect without accessing SPI.
 *
 * Returns false during a software guard scaled from 800 us at a 100 MHz
 * calibration clock (the data sheet's maximum at its stated test conditions).
 * Calibration clock is OSCin / 2^CAL_CLK_DIV and is kept below 200 MHz. This
 * scaled wait rejects immediate stale status; it is not a guaranteed lock or
 * settling time for every frequency, reference, supply and loop filter.
 * Full-assist requests conservatively retain this same guard, despite bypassing
 * automatic calibration. Thereafter LD_EN=1 and LD_TYPE=1 provide the GPIO lock
 * indication. Consumers
 * must characterize both locking and analog settling on their hardware.
 * @param[in,out] synth Initialized, non-faulted LMX2592 handle.
 * @param[out] locked Destination, unchanged on failure.
 * @return OK (including unlocked), INVALID_ARG, STATE or GPIO failure.
 */
esy_err_t esy_lmx2592_is_locked(esy_freq_synth_t *synth, bool *locked);

/**
 * @brief Poll lock with 5 us delays until success or timeout.
 *
 * Uses HAL monotonic microsecond time; scheduling and synchronous I/O can
 * extend elapsed time. Zero timeout performs one status query. Serialize the
 * handle and retain resources throughout this potentially blocking operation.
 * @param[in,out] synth Initialized LMX2592 handle.
 * @param[in] timeout_us Deadline interval, 0..INT32_MAX microseconds.
 * @return OK, TIMEOUT, INVALID_ARG, STATE or GPIO failure.
 */
esy_err_t esy_lmx2592_wait_for_lock(esy_freq_synth_t *synth,
                                  uint32_t timeout_us);

#ifdef __cplusplus
}
#endif
#endif /* ESY_LMX2592_H */
