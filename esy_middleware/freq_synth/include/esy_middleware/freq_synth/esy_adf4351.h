/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_adf4351.h
 * @brief ADF4351 frequency synthesizer driver (binds to @ref esy_freq_synth_t
 * abstraction).
 *
 * ## Overview
 * This driver controls the Analog Devices ADF4351 wideband PLL frequency
 * synthesizer.
 *
 * Key characteristics:
 * - Programs the device by writing 32-bit registers (R5..R0) via SPI.
 * - Uses GPIO to control pins such as LE/CE (latch enable / chip enable).
 * - After successful initialization, the driver binds itself to an @ref
 * esy_freq_synth_t handle:
 *   - fills the vtable (`synth->ops`)
 *   - sets `synth->ctx` to the backend context placed in `synth->backend.bytes`
 *   This allows upper layers to call the generic freq_synth APIs without
 * knowing the chip details.
 * - The driver does not require an external context buffer; it uses the backend
 * storage inside
 *   @ref esy_freq_synth_t.
 *
 * ## Notes
 * - Channel spacing of ADF4351 is: `spacing = PFD / MOD`.
 *   Therefore, @ref esy_adf4351_config_t::channel_step_hz is a *target* step
 * and the driver will try to approximate it during parameter solving.
 * - Some configuration items (e.g., CP current, MUXOUT, noise mode) are passed
 * through as raw register-field values. The caller must ensure these values are
 * valid per the datasheet.
 *
 * ## Thread safety
 * ADF4351 access is not internally synchronized. The caller must serialize all
 * operations on the same @ref esy_freq_synth_t handle, including init/deinit
 * and frequency/output/power updates.
 */

#ifndef ESY_ADF4351_H
#define ESY_ADF4351_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include "esy_middleware/freq_synth/esy_freq_synth.h"
#include "esy_hal/esy_hal_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Frequency solving strategy for ADF4351.
 *
 * ADF4351 output frequency is determined by solving parameters such as
 * INT/FRAC/MOD and reference path settings (R counter, doubler, /2, etc.). This
 * driver provides two solving strategies trading off precision/spur behavior
 * versus CPU cost.
 *
 * - PRECISE:
 *   Searches MOD (and corresponding FRAC) within a window to minimize frequency
 * error. Usually yields better approximation and potentially better spur
 * characteristics, at the cost of higher computation time.
 *
 * - FAST:
 *   Tries the ideal MOD (and optionally a small ±span neighborhood) for much
 * faster calculation, with potentially larger frequency error and/or less
 * optimal spurs.
 */
typedef enum {
  ESY_ADF4351_SOLVE_MODE_PRECISE =
      0, /**< Search MOD window (more accurate, slower). */
  ESY_ADF4351_SOLVE_MODE_FAST =
      1, /**< Try ideal MOD (optional ±span), faster with possible error. */
} esy_adf4351_solve_mode_t;

/**
 * @brief ADF4351 charge pump current register setting.
 *
 * The numeric value is the raw register encoding. Board-level RF design should
 * choose the setting from the datasheet table.
 */
typedef enum {
  ESY_ADF4351_CP_CURRENT_SETTING_0 = 0,
  ESY_ADF4351_CP_CURRENT_SETTING_1 = 1,
  ESY_ADF4351_CP_CURRENT_SETTING_2 = 2,
  ESY_ADF4351_CP_CURRENT_SETTING_3 = 3,
  ESY_ADF4351_CP_CURRENT_SETTING_4 = 4,
  ESY_ADF4351_CP_CURRENT_SETTING_5 = 5,
  ESY_ADF4351_CP_CURRENT_SETTING_6 = 6,
  ESY_ADF4351_CP_CURRENT_SETTING_7 = 7,
  ESY_ADF4351_CP_CURRENT_SETTING_8 = 8,
  ESY_ADF4351_CP_CURRENT_SETTING_9 = 9,
  ESY_ADF4351_CP_CURRENT_SETTING_10 = 10,
  ESY_ADF4351_CP_CURRENT_SETTING_11 = 11,
  ESY_ADF4351_CP_CURRENT_SETTING_12 = 12,
  ESY_ADF4351_CP_CURRENT_SETTING_13 = 13,
  ESY_ADF4351_CP_CURRENT_SETTING_14 = 14,
  ESY_ADF4351_CP_CURRENT_SETTING_15 = 15,
} esy_adf4351_cp_current_t;

/**
 * @brief ADF4351 MUXOUT register setting.
 */
typedef enum {
  ESY_ADF4351_MUXOUT_THREE_STATE = 0,
  ESY_ADF4351_MUXOUT_DVDD = 1,
  ESY_ADF4351_MUXOUT_DGND = 2,
  ESY_ADF4351_MUXOUT_R_COUNTER_OUTPUT = 3,
  ESY_ADF4351_MUXOUT_N_DIVIDER_OUTPUT = 4,
  ESY_ADF4351_MUXOUT_ANALOG_LOCK_DETECT = 5,
  ESY_ADF4351_MUXOUT_DIGITAL_LOCK_DETECT = 6,
} esy_adf4351_muxout_t;

/**
 * @brief ADF4351 noise/spur mode register setting.
 *
 * The numeric value is the raw register encoding. Use the datasheet table when
 * selecting a non-default mode.
 */
typedef enum {
  ESY_ADF4351_NOISE_MODE_0 = 0,
  ESY_ADF4351_NOISE_MODE_1 = 1,
  ESY_ADF4351_NOISE_MODE_2 = 2,
  ESY_ADF4351_NOISE_MODE_3 = 3,
} esy_adf4351_noise_mode_t;

/**
 * @brief ADF4351 lock-detect pin mode register setting.
 *
 * The numeric value is the raw register encoding.
 */
typedef enum {
  ESY_ADF4351_LD_PIN_MODE_0 = 0,
  ESY_ADF4351_LD_PIN_MODE_1 = 1,
  ESY_ADF4351_LD_PIN_MODE_2 = 2,
  ESY_ADF4351_LD_PIN_MODE_3 = 3,
} esy_adf4351_ld_pin_mode_t;

/**
 * @brief ADF4351 initialization configuration.
 *
 * ## Ownership and responsibilities
 * - `spi` must be initialized by the caller.
 * - `le_gpio` is required and `ce_gpio` is optional.
 * - Both GPIO resources remain caller-owned for the complete binding lifetime.
 *
 * ## Reference path
 * PFD frequency is determined by:
 * `PFD = ref_clk_hz * (ref_doubler ? 2 : 1) / (ref_div2 ? 2 : 1) / r_counter`
 *
 * The target channel step influences the solver via MOD selection:
 * `spacing = PFD / MOD`
 */
typedef struct {
  /**
   * @name SPI configuration
   * @{
   */

  /** @brief [in] SPI handle (must be initialized). */
  esy_spi_t *spi;

  /**
   * @brief [in] SPI bus speed in Hz.
   *
   * - 0: do not change the current SPI speed.
   * - Non-zero: the driver will attempt to set the bus speed if supported by
   * the HAL.
   */
  uint32_t spi_speed_hz;

  /** @} */

  /**
   * @name GPIO pins
   * @{
   */

  /**
   * @brief [in] LE pin (Latch Enable).
   *
   * - LE must stay low during SPI shifting.
   * - After a 32-bit write, pulse LE high to latch the register.
   */
  esy_gpio_t *le_gpio;

  /**
   * @brief [in] CE pin (Chip Enable), optional.
   *
   * - NULL: CE control is not used (e.g., CE is tied high in hardware).
   * - Non-NULL: the initialized resource is driven high during init.
   */
  esy_gpio_t *ce_gpio;

  /**
   * @brief [in] LE high-level pulse hold time in microseconds.
   *
   * - Used to meet latch pulse width requirements.
   * - 0: no explicit delay (rely on HAL timing / GPIO toggle latency).
   */
  uint32_t le_pulse_delay_us;

  /** @} */

  /**
   * @name Reference & channel plan
   * @{
   */

  /** @brief [in] Reference clock frequency in Hz (typical: 10/20/25 MHz). */
  uint32_t ref_clk_hz;

  /**
   * @brief [in] Target channel step in Hz.
   *
   * The actual step is `spacing = PFD / MOD`. The driver solver will attempt to
   * approximate this target. Must be greater than 0.
   */
  uint32_t channel_step_hz;

  /**
   * @brief [in] Initial output frequency in Hz programmed during
   * initialization.
   *
   * Must be within the ADF4351 output range.
   */
  uint64_t initial_freq_hz;

  /**
   * @brief [in] Whether MAIN RF output is enabled after initialization.
   *
   * Keep this false unless the board-level startup sequence is ready to emit
   * RF.
   */
  bool initial_output_enabled;

  /**
   * @brief [in] R counter (1..1023).
   *
   * Sets the reference divider. Typical default is 1.
   */
  uint16_t r_counter;

  /**
   * @brief [in] Reference /2 enable (0/1).
   *
   * - 1: divide reference clock by 2 before PFD.
   * - Default is 0.
   */
  uint8_t ref_div2;

  /**
   * @brief [in] Reference doubler enable (0/1).
   *
   * - 1: double reference clock before PFD.
   * - Default is 0.
   */
  uint8_t ref_doubler;

  /**
   * @brief [in] PFD maximum limit in Hz (validation constraint).
   *
   * Must be greater than 0 and is used as an upper bound during solving.
   */
  uint64_t pfd_max_hz;

  /** @} */

  /**
   * @name ADF4351 register field passthrough
   *
   * These fields are written directly into device register fields.
   * Value ranges are defined by the ADF4351 datasheet; the caller must provide
   * valid values.
   * @{
   */

  /** @brief [in] Charge pump current setting. */
  esy_adf4351_cp_current_t cp_current;

  /**
   * @brief [in] MUXOUT selection. Typical default is digital lock detect.
   *
   * A common choice is a digital lock detect output, but encoding depends on
   * the datasheet.
   */
  esy_adf4351_muxout_t muxout;

  /** @brief [in] Noise/spur mode. */
  esy_adf4351_noise_mode_t noise_mode;

  /** @brief [in] Band select clock divider, 1..255; zero is invalid.
   * Slow mode requires PFD/divider <=125 kHz. Otherwise the driver selects fast
   * mode, which requires a divider <=254 and PFD/divider <=500 kHz.
   * Typical default is 200.
   */
  uint8_t band_sel_div;

  /** @brief [in] LD (lock detect) pin mode. */
  esy_adf4351_ld_pin_mode_t ld_pin_mode;

  /** @} */

  /**
   * @name Solve mode
   * @{
   */

  /**
   * @brief [in] Frequency solving mode.
   *
   * - default_config typically selects FAST.
   * - Use PRECISE when you want smaller frequency error and/or better spur
   * behavior.
   */
  esy_adf4351_solve_mode_t solve_mode;

  /**
   * @brief [in] MOD probing span for FAST mode.
   *
   * - 0: try only ideal_mod.
   * - 1: try ideal_mod-1, ideal_mod, ideal_mod+1.
   * - N: try the window ideal_mod ± N (exact behavior is
   * implementation-defined).
   *
   * Larger span may reduce frequency error but increases computation time
   * (usually still much smaller than PRECISE mode).
   */
  uint8_t fast_mod_span;

  /** @} */
} esy_adf4351_config_t;

/**
 * @brief Fill a conservative default configuration for ADF4351.
 *
 * The function writes a commonly useful default set (e.g., r_counter=1,
 * initial_freq_hz=2.6 GHz, initial_output_enabled=false, solve_mode=FAST,
 * pfd_max_hz=32 MHz, etc.).
 * The caller may override fields afterward.
 *
 * @param[out] config Configuration object (must not be NULL).
 */
void esy_adf4351_default_config(esy_adf4351_config_t *config);

/**
 * @brief Initialize ADF4351 and bind it to a frequency synthesizer abstraction
 * handle.
 *
 * Behavior:
 * - Uses caller-owned initialized SPI/GPIO resources from @p config.
 * - Places the driver backend context into @p synth->backend.bytes and binds
 * the vtable.
 * - If `ce_gpio != NULL`, the driver drives CE high to enable the chip.
 * - Writes required register defaults to bring the device into an operational
 * state (exact sequence is implementation-defined).
 *
 *
 * The first use requires `esy_freq_synth_t synth = {0}`. A live or faulted
 * handle returns ESY_ERR_STATE without modification. Invalid configuration
 * leaves the handle unchanged. I/O failure attempts cleanup; if cleanup also
 * fails, the handle remains attached and faulted until deinit succeeds.
 * These operations are task-only and require external serialization.

 * @param[in,out] synth  Frequency synthesizer handle (must not be NULL).
 * @param[in]     config Initialization configuration (must not be NULL).
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments.
 * @retval ESY_ERR_STATE       Handle is already live or faulted.
 * @retval ESY_ERR_FAIL        Initial GPIO setup failed.
 * @return Register programming errors, including ESY_ERR_IO for SPI failure,
 *         are propagated. Failed cleanup retains an attached, faulted handle
 *         as described above; the returned error describes initialization.
 */
esy_err_t esy_adf4351_init(esy_freq_synth_t *synth,
                           const esy_adf4351_config_t *config);

/**
 * @brief Disable outputs and detach only after successful cleanup.
 *
 * Task context only; externally serialize with every operation on this handle
 * and the shared SPI bus. Unregister the logical device first. Caller-owned
 * SPI/GPIO resources must remain valid until this function succeeds.
 * On failure, the handle remains attached and faulted; only unregister and
 * matching backend deinit may be retried. Do not clear or reuse its storage.
 * Success leaves a zeroed handle; repeated deinit of that handle succeeds.
 * @param[in,out] synth Zero-initialized or initialized backend handle.
 * @retval ESY_OK Cleanup complete, including an already empty handle.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_BUSY Logical device still registered or cleanup pending.
 * @retval ESY_ERR_STATE Handle belongs to a different backend.
 * @return SPI/GPIO error when cleanup cannot establish the safe state.
 */
esy_err_t esy_adf4351_deinit(esy_freq_synth_t *synth);

#ifdef __cplusplus
}
#endif

#endif /* ESY_ADF4351_H */
