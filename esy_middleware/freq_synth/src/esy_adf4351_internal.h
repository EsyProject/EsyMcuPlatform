/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ADF4351_INTERNAL_H
#define ESY_ADF4351_INTERNAL_H

#include "esy_middleware/freq_synth/esy_adf4351.h"
#include "esy_common/device/esy_device.h"

typedef struct adf4351_ctx_s adf4351_ctx_t;

typedef struct {
  esy_err_t (*write_u32)(adf4351_ctx_t *ctx, uint32_t reg);
} adf4351_hw_ops_t;

/**
 * @brief ADF4351 driver-private context stored in the esy_freq_synth_t::backend
 * private area
 *
 * @details
 * Key points:
 * - The driver owns this context; upper layers must not access it directly.
 * - It caches the latest requested_hz and actual current_hz after quantization.
 * - It caches synthesis results such as PFD/MOD/FRAC/INT and rf_div_sel for
 * get_info and fast register updates.
 */
struct adf4351_ctx_s {
  esy_spi_t *spi;
  esy_gpio_t *le_gpio;
  esy_gpio_t *ce_gpio;
  const adf4351_hw_ops_t *hw_ops;
  const esy_device_t *pm_requester;

  uint64_t pfd_max_hz;
  /** Latest user-requested frequency before quantization */
  uint64_t requested_hz;
  /** Actual output frequency after INT/FRAC/MOD quantization */
  uint64_t current_hz;
  /** Actual PFD frequency */
  uint64_t pfd_hz;
  /** Actual channel spacing: spacing = PFD / MOD */
  uint64_t channel_spacing_hz;

  uint32_t ref_clk_hz;
  uint32_t channel_step_hz;
  uint32_t le_pulse_delay_us;
  esy_adf4351_cp_current_t cp_current;
  esy_adf4351_muxout_t muxout;
  esy_adf4351_noise_mode_t noise_mode;
  esy_adf4351_ld_pin_mode_t ld_pin_mode;
  esy_adf4351_solve_mode_t solve_mode;
  /** Register mirror cache used for diff-write to reduce SPI transfers */
  uint32_t r0, r1, r2, r3, r4, r5;

  uint16_t r_counter;
  uint16_t mod;
  uint16_t frac;
  uint16_t intn;
  uint8_t ref_div2;
  uint8_t ref_doubler;
  uint8_t band_sel_div;
  uint8_t fast_mod_span;
  /** Main output power level (0..3) */
  uint8_t main_power_level;
  /** Main output enable state */
  bool main_output_enabled;
  bool resume_main_output_enabled;
  bool suspended;
  /** RF output divider selection: 0..6 => /1,/2,/4,/8,/16,/32,/64 */
  uint8_t rf_div_sel;
};

/** @brief Borrow context only for this driver's immutable operations table. */
ESY_INTERNAL adf4351_ctx_t *esy_adf4351_get_context_internal(
    esy_freq_synth_t *synth);

/** @brief Driver output operation shared only with its device PM adapter. */
ESY_INTERNAL esy_err_t esy_adf4351_set_output_enable_internal(
    void *context, esy_freq_synth_port_t port, bool enable);

#endif /* ESY_ADF4351_INTERNAL_H */
