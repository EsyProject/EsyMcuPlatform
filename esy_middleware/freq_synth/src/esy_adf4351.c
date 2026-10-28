/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_adf4351.h"
#include "esy_adf4351_internal.h"

#include <stdbool.h>
#include <string.h>

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_spi_device.h"
#include "esy_hal/esy_hal_system.h" /* esy_hal_system_delay_us() */

/* -----------------------------
 * ADF4351 Register bit offsets
 * ----------------------------- */
#define ADF_R0_INT_SHIFT                15u
#define ADF_R0_FRAC_SHIFT               3u
#define ADF_R0_ID                       0u

#define ADF_R1_MOD_SHIFT                3u
#define ADF_R1_PHASE_SHIFT              15u
#define ADF_R1_PRESCALER_SHIFT          27u
#define ADF_R1_PHASE_ADJ_SHIFT          28u
#define ADF_R1_ID                       1u

#define ADF_R2_COUNTER_RESET_SHIFT      3u
#define ADF_R2_CP_THREESTATE_SHIFT      4u
#define ADF_R2_POWERDOWN_SHIFT          5u
#define ADF_R2_PD_POLARITY_SHIFT        6u
#define ADF_R2_LDP_SHIFT                7u
#define ADF_R2_LDF_SHIFT                8u
#define ADF_R2_CP_CURRENT_SHIFT         9u
#define ADF_R2_DOUBLE_BUF_SHIFT         13u
#define ADF_R2_R_COUNTER_SHIFT          14u
#define ADF_R2_REF_DIV2_SHIFT           24u
#define ADF_R2_REF_DOUBLER_SHIFT        25u
#define ADF_R2_MUXOUT_SHIFT             26u
#define ADF_R2_NOISE_MODE_SHIFT         29u
#define ADF_R2_ID                       2u

#define ADF_R3_CLKDIV_VALUE_SHIFT       3u
#define ADF_R3_CLKDIV_MODE_SHIFT        15u
#define ADF_R3_CSR_SHIFT                18u
#define ADF_R3_CHARGE_CANCEL_SHIFT      21u
#define ADF_R3_ABP_SHIFT                22u
#define ADF_R3_BAND_SEL_CLK_SHIFT       23u
#define ADF_R3_ID                       3u

#define ADF_R4_OUT_PWR_SHIFT            3u
#define ADF_R4_RF_OUT_EN_SHIFT          5u
#define ADF_R4_AUX_OUT_PWR_SHIFT        6u
#define ADF_R4_AUX_OUT_EN_SHIFT         8u
#define ADF_R4_AUX_OUT_SEL_SHIFT        9u
#define ADF_R4_MTLD_SHIFT               10u
#define ADF_R4_VCO_PWDN_SHIFT           11u
#define ADF_R4_BAND_SEL_DIV_SHIFT       12u
#define ADF_R4_RF_DIV_SEL_SHIFT         20u
#define ADF_R4_FB_SEL_SHIFT             23u
#define ADF_R4_ID                       4u

#define ADF_R5_LD_PIN_MODE_SHIFT        22u
#define ADF_R5_RESERVED_SHIFT           19u
#define ADF_R5_ID                       5u

/* -----------------------------
 * Limits (common constraints)
 * ----------------------------- */
#define ADF_OUT_MIN_HZ                  35000000ULL
#define ADF_OUT_MAX_HZ                  4400000000ULL
#define ADF_VCO_MIN_HZ                  2200000000ULL
#define ADF_VCO_MAX_HZ                  4400000000ULL
#define ADF_PFD_MAX_HZ                  32000000ULL

#define ADF_R_COUNTER_MIN               1u
#define ADF_R_COUNTER_MAX               1023u

#define ADF_MOD_MIN                     2u
#define ADF_MOD_MAX                     4095u

#define ADF_MOD_SEARCH_WINDOW           96u

#define ADF4351_DEFAULT_REF_CLK_HZ      20000000u
#define ADF4351_DEFAULT_STEP_HZ         25000u
#define ADF4351_DEFAULT_INITIAL_FREQ_HZ 2600000000ULL
#define ADF4351_DEFAULT_R_COUNTER       1u
#define ADF4351_DEFAULT_REF_DIV2        0u
#define ADF4351_DEFAULT_REF_DOUBLER     0u
#define ADF4351_DEFAULT_PFD_MAX_HZ      ADF_PFD_MAX_HZ

#define ADF4351_DEFAULT_CP_CURRENT      ESY_ADF4351_CP_CURRENT_SETTING_8
#define ADF4351_DEFAULT_MUXOUT          ESY_ADF4351_MUXOUT_DIGITAL_LOCK_DETECT
#define ADF4351_DEFAULT_NOISE_MODE      ESY_ADF4351_NOISE_MODE_0
#define ADF4351_DEFAULT_BAND_SEL_DIV    200u
#define ADF4351_DEFAULT_LD_PIN_MODE     ESY_ADF4351_LD_PIN_MODE_1

#define ADF4351_DEFAULT_SOLVE_MODE      ESY_ADF4351_SOLVE_MODE_FAST
#define ADF4351_DEFAULT_FAST_MOD_SPAN   0u

_Static_assert(ESY_FREQ_SYNTH_PRIV_BYTES >= sizeof(adf4351_ctx_t),
               "ESY_FREQ_SYNTH_PRIV_BYTES too small for adf4351_ctx_t");

static uint64_t adf4351_u64_abs_diff(uint64_t a, uint64_t b) {
  return (a >= b) ? (a - b) : (b - a);
}

static uint32_t adf4351_gcd_u32(uint32_t a, uint32_t b) {
  while (b != 0u) {
    uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

static uint16_t adf4351_clamp_u16(uint32_t v, uint16_t lo, uint16_t hi) {
  if (v < (uint32_t)lo) return lo;
  if (v > (uint32_t)hi) return hi;
  return (uint16_t)v;
}

/**
 * @brief Generate one LE latch pulse
 *
 * @note
 * - ADF4351 latches shifted SPI data into the selected register on the LE
 * rising edge or high pulse.
 * - This implementation drives LE high, optionally delays, then drives it low.
 * - Set le_pulse_delay_us to 0 when HAL GPIO toggling is already slow
 * enough.
 */
static esy_err_t adf4351_spi_write_u32(adf4351_ctx_t *ctx, uint32_t reg) {
  const uint8_t buf[] = {(uint8_t)(reg >> 24), (uint8_t)(reg >> 16),
                         (uint8_t)(reg >> 8), (uint8_t)reg};
  esy_err_t err = esy_hal_gpio_write(ctx->le_gpio, false);
  if (err != ESY_OK) return err;
  const int32_t written =
      ctx->pm_requester != NULL
          ? esy_hal_spi_write_blocking_for_pm(ctx->spi, ctx->pm_requester, buf, sizeof(buf))
          : esy_hal_spi_write_blocking(ctx->spi, buf, sizeof(buf));
  if (written != (int32_t)sizeof(buf)) return ESY_ERR_IO;
  err = esy_hal_gpio_write(ctx->le_gpio, true);
  if (ctx->le_pulse_delay_us != 0u)
    esy_hal_system_delay_us(ctx->le_pulse_delay_us);
  const esy_err_t release = esy_hal_gpio_write(ctx->le_gpio, false);
  return err != ESY_OK ? err : release;
}

static const adf4351_hw_ops_t k_adf4351_platform_hw_ops = {
    .write_u32 = adf4351_spi_write_u32,
};

static esy_err_t adf4351_write_reg(adf4351_ctx_t *ctx, uint32_t reg) {
  if ((ctx == NULL) || (ctx->hw_ops == NULL) ||
      (ctx->hw_ops->write_u32 == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  return ctx->hw_ops->write_u32(ctx, reg);
}

static esy_err_t adf4351_write_if_changed(adf4351_ctx_t *ctx, uint32_t new_reg,
                                          uint32_t *cached_reg) {
  if ((ctx == NULL) || (cached_reg == NULL)) return ESY_ERR_INVALID_ARG;
  if (new_reg == *cached_reg) return ESY_OK;

  const esy_err_t err = adf4351_write_reg(ctx, new_reg);
  if (err != ESY_OK) {
    return err;
  }

  *cached_reg = new_reg;
  return ESY_OK;
}

/**
 * @brief Only write changed registers while preserving order; R0 must be
 * written last
 *
 * @details
 * ADF4351 normally requires R0 to be written last to trigger a new frequency
 * update. This path diff-writes r5..r1 and writes r0 last.
 */
static esy_err_t adf4351_write_changed(adf4351_ctx_t *ctx, uint32_t new_r0,
                                       uint32_t new_r1, uint32_t new_r2,
                                       uint32_t new_r3, uint32_t new_r4,
                                       uint32_t new_r5) {
  esy_err_t err;

  const bool commit = new_r1 != ctx->r1 || new_r2 != ctx->r2;
  err = adf4351_write_if_changed(ctx, new_r5, &ctx->r5);
  if (err != ESY_OK) return err;
  err = adf4351_write_if_changed(ctx, new_r4, &ctx->r4);
  if (err != ESY_OK) return err;
  err = adf4351_write_if_changed(ctx, new_r3, &ctx->r3);
  if (err != ESY_OK) return err;
  err = adf4351_write_if_changed(ctx, new_r2, &ctx->r2);
  if (err != ESY_OK) return err;
  err = adf4351_write_if_changed(ctx, new_r1, &ctx->r1);
  if (err != ESY_OK) return err;
  err = commit ? adf4351_write_reg(ctx, new_r0)
               : adf4351_write_if_changed(ctx, new_r0, &ctx->r0);
  if (err == ESY_OK) ctx->r0 = new_r0;
  if (err != ESY_OK) return err;

  return ESY_OK;
}

/* pack_xxx functions are straightforward bit-field packing helpers. */

static uint32_t adf4351_pack_r0(uint16_t INT, uint16_t FRAC) {
  uint32_t r = 0;
  r |= ((uint32_t)(INT & 0xFFFFu) << ADF_R0_INT_SHIFT);
  r |= ((uint32_t)(FRAC & 0x0FFFu) << ADF_R0_FRAC_SHIFT);
  r |= ADF_R0_ID;
  return r;
}

static uint32_t adf4351_pack_r1(uint16_t MOD, uint16_t PHASE, uint8_t prescaler,
                                uint8_t phase_adj) {
  uint32_t r = 0;
  r |= ((uint32_t)(MOD & 0x0FFFu) << ADF_R1_MOD_SHIFT);
  r |= ((uint32_t)(PHASE & 0x0FFFu) << ADF_R1_PHASE_SHIFT);
  r |= ((uint32_t)(prescaler & 0x01u) << ADF_R1_PRESCALER_SHIFT);
  r |= ((uint32_t)(phase_adj & 0x01u) << ADF_R1_PHASE_ADJ_SHIFT);
  r |= ADF_R1_ID;
  return r;
}

static uint32_t adf4351_pack_r2(uint16_t r_counter, uint8_t ref_div2,
                                uint8_t ref_doubler, uint8_t cp_current,
                                uint8_t muxout, uint8_t noise_mode) {
  uint32_t r = 0;
  r |= (0u << ADF_R2_COUNTER_RESET_SHIFT);
  r |= (0u << ADF_R2_CP_THREESTATE_SHIFT);
  r |= (0u << ADF_R2_POWERDOWN_SHIFT);
  r |= (1u << ADF_R2_PD_POLARITY_SHIFT);
  r |= (0u << ADF_R2_LDP_SHIFT);
  r |= (0u << ADF_R2_LDF_SHIFT);
  r |= ((uint32_t)(cp_current & 0x0Fu) << ADF_R2_CP_CURRENT_SHIFT);
  r |= (0u << ADF_R2_DOUBLE_BUF_SHIFT);
  r |= ((uint32_t)(r_counter & 0x03FFu) << ADF_R2_R_COUNTER_SHIFT);
  r |= ((uint32_t)(ref_div2 & 0x01u) << ADF_R2_REF_DIV2_SHIFT);
  r |= ((uint32_t)(ref_doubler & 0x01u) << ADF_R2_REF_DOUBLER_SHIFT);
  r |= ((uint32_t)(muxout & 0x07u) << ADF_R2_MUXOUT_SHIFT);
  r |= ((uint32_t)(noise_mode & 0x03u) << ADF_R2_NOISE_MODE_SHIFT);
  r |= ADF_R2_ID;
  return r;
}

static uint32_t adf4351_pack_r3(uint16_t clkdiv_value, uint8_t band_sel_clk) {
  uint32_t r = 0;
  r |= ((uint32_t)(clkdiv_value & 0x0FFFu) << ADF_R3_CLKDIV_VALUE_SHIFT);
  r |= (0u << ADF_R3_CLKDIV_MODE_SHIFT);
  r |= (0u << ADF_R3_CSR_SHIFT);
  r |= (0u << ADF_R3_CHARGE_CANCEL_SHIFT);
  r |= (0u << ADF_R3_ABP_SHIFT);
  r |= ((uint32_t)(band_sel_clk & 0x01u) << ADF_R3_BAND_SEL_CLK_SHIFT);
  r |= ADF_R3_ID;
  return r;
}

static uint32_t adf4351_pack_r4(uint8_t out_pwr, uint8_t rf_out_en,
                                uint8_t aux_out_en, uint8_t rf_div_sel,
                                uint8_t fb_sel, uint8_t band_sel_div) {
  uint32_t r = 0;
  r |= ((uint32_t)(out_pwr & 0x03u) << ADF_R4_OUT_PWR_SHIFT);
  r |= ((uint32_t)(rf_out_en & 0x01u) << ADF_R4_RF_OUT_EN_SHIFT);
  r |= (0u << ADF_R4_AUX_OUT_PWR_SHIFT);
  r |= ((uint32_t)(aux_out_en & 0x01u) << ADF_R4_AUX_OUT_EN_SHIFT);
  r |= (0u << ADF_R4_AUX_OUT_SEL_SHIFT);
  r |= (0u << ADF_R4_MTLD_SHIFT);
  r |= (0u << ADF_R4_VCO_PWDN_SHIFT);
  r |= ((uint32_t)(band_sel_div & 0xFFu) << ADF_R4_BAND_SEL_DIV_SHIFT);
  r |= ((uint32_t)(rf_div_sel & 0x07u) << ADF_R4_RF_DIV_SEL_SHIFT);
  r |= ((uint32_t)(fb_sel & 0x01u) << ADF_R4_FB_SEL_SHIFT);
  r |= ADF_R4_ID;
  return r;
}

static uint32_t adf4351_pack_r5(uint8_t ld_pin_mode) {
  uint32_t r = 0;
  r |= (3u << ADF_R5_RESERVED_SHIFT); /* ADF4351 requires reserved=0b11 */
  r |= ((uint32_t)(ld_pin_mode & 0x03u) << ADF_R5_LD_PIN_MODE_SHIFT);
  r |= ADF_R5_ID;
  return r;
}

/**
 * @brief Calculate the PFD frequency and apply basic validity and limit checks
 *
 * @details
 * PFD = ref_clk * (ref_doubler?2:1) / (r_counter * (ref_div2?2:1))
 * - Return 0 when the calculated value exceeds pfd_max_hz.
 */
static uint64_t adf4351_calc_pfd_hz(const adf4351_ctx_t *ctx) {
  if (ctx == NULL) return 0u;
  if (ctx->ref_clk_hz == 0u) return 0u;
  if ((ctx->r_counter < ADF_R_COUNTER_MIN) ||
      (ctx->r_counter > ADF_R_COUNTER_MAX))
    return 0u;
  if ((ctx->ref_div2 > 1u) || (ctx->ref_doubler > 1u)) return 0u;

  const uint64_t mul = (ctx->ref_doubler != 0u) ? 2u : 1u;
  const uint64_t div2 = (ctx->ref_div2 != 0u) ? 2u : 1u;

  const uint64_t pfd =
      ((uint64_t)ctx->ref_clk_hz * mul) / ((uint64_t)ctx->r_counter * div2);
  if (pfd == 0u) return 0u;

  if (pfd > ctx->pfd_max_hz) {
    return 0u;
  }
  return pfd;
}

/**
 * @brief Frequency solver result
 *
 * @details
 * - error_hz is relative to the VCO target because output still passes through
 * the RF divider.
 * - reduced_mod favors a more reducible FRAC/MOD pair, which is empirically
 * better for spurs.
 */
typedef struct {
  uint16_t mod;
  uint16_t frac;
  uint16_t intn;
  uint64_t vco_actual_hz;
  uint64_t out_actual_hz;
  uint64_t error_hz;
  uint32_t reduced_mod;
  bool integer_n;
} adf4351_solution_t;

/**
 * @brief Frequency configuration plan
 *
 * @details
 * This structure describes what to write and what to cache after a successful
 * write; it does not access hardware directly. This keeps frequency solving,
 * register packing, and SPI/GPIO writes independently verifiable.
 */
typedef struct {
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r4;
  uint32_t r5;

  uint64_t pfd_hz;
  uint64_t current_hz;
  uint64_t channel_spacing_hz;

  uint16_t mod;
  uint16_t frac;
  uint16_t intn;

  uint8_t rf_div_sel;
  uint8_t main_power_level;
} adf4351_plan_t;

/**
 * @brief Try a given MOD, calculate INT/FRAC, and produce the actual frequency
 *
 * @details
 * - Use rounding: FRAC = round(rem*MOD/PFD)
 * - If FRAC overflows to MOD, carry into INT and set FRAC to 0
 */
static bool adf4351_try_solution(uint64_t pfd_num, uint32_t pfd_den,
                                 uint64_t vco_target_hz,
                                 uint8_t rf_div_sel, uint16_t mod,
                                 adf4351_solution_t *out_sol) {
  if ((pfd_num == 0u) || (pfd_den == 0u) || (out_sol == NULL)) return false;
  if ((mod < ADF_MOD_MIN) || (mod > ADF_MOD_MAX)) return false;

  const uint64_t target_num = vco_target_hz * pfd_den;
  uint64_t int64 = target_num / pfd_num;
  if (int64 > 65535u) return false;

  const uint64_t rem = target_num % pfd_num;

  uint64_t frac64 = (rem * (uint64_t)mod + (pfd_num / 2u)) / pfd_num;

  uint16_t intn = (uint16_t)int64;
  uint16_t frac = 0u;

  if (frac64 >= (uint64_t)mod) {
    if (int64 == 65535u) return false;
    intn = (uint16_t)(int64 + 1u);
    frac = 0u;
  } else {
    frac = (uint16_t)frac64;
  }

  if (frac > 4095u) return false;

  const uint64_t vco_actual_hz =
      (((uint64_t)intn * pfd_num) +
       (((uint64_t)frac * pfd_num) / (uint64_t)mod)) / pfd_den;
  const uint64_t out_actual_hz = vco_actual_hz >> rf_div_sel;
  const uint64_t error_hz = adf4351_u64_abs_diff(vco_actual_hz, vco_target_hz);

  uint32_t g = adf4351_gcd_u32((uint32_t)frac, (uint32_t)mod);
  if (g == 0u) g = (uint32_t)mod;
  const uint32_t reduced_mod = (uint32_t)mod / g;

  out_sol->mod = mod;
  out_sol->frac = frac;
  out_sol->intn = intn;
  out_sol->vco_actual_hz = vco_actual_hz;
  out_sol->out_actual_hz = out_actual_hz;
  out_sol->error_hz = error_hz;
  out_sol->reduced_mod = reduced_mod;
  out_sol->integer_n = (frac == 0u);
  return true;
}

/**
 * @brief PRECISE mode: search near ideal_mod and choose the best solution
 *
 * @details Selection priority, from highest to lowest:
 * 1) Smaller frequency error.
 * 2) For equal error, prefer integer-N (FRAC == 0).
 * 3) Then prefer smaller reduced_mod, which means a smaller denominator and is
 * empirically better for spurs. 4) Finally prefer smaller MOD.
 */
static bool adf4351_choose_best_solution(uint64_t pfd_num, uint32_t pfd_den,
                                         uint64_t vco_target_hz,
                                         uint8_t rf_div_sel,
                                         uint32_t desired_step_hz,
                                         adf4351_solution_t *out_best) {
  if ((pfd_num == 0u) || (pfd_den == 0u) || (desired_step_hz == 0u) || (out_best == NULL))
    return false;

  const uint64_t step_num = (uint64_t)desired_step_hz * pfd_den;
  uint64_t ideal_mod64 = (pfd_num + step_num / 2u) / step_num;
  uint16_t ideal_mod =
      adf4351_clamp_u16((uint32_t)ideal_mod64, ADF_MOD_MIN, ADF_MOD_MAX);

  adf4351_solution_t best = (adf4351_solution_t){0};
  bool has_best = false;

  const uint32_t win = ADF_MOD_SEARCH_WINDOW;
  uint32_t start =
      (ideal_mod > win) ? (uint32_t)ideal_mod - win : (uint32_t)ADF_MOD_MIN;
  uint32_t end = (uint32_t)ideal_mod + win;
  if (end > ADF_MOD_MAX) end = ADF_MOD_MAX;
  if (start < ADF_MOD_MIN) start = ADF_MOD_MIN;

  for (uint32_t m = start; m <= end; m++) {
    adf4351_solution_t sol;
    if (!adf4351_try_solution(pfd_num, pfd_den, vco_target_hz, rf_div_sel, (uint16_t)m,
                              &sol)) {
      continue;
    }

    if (!has_best) {
      best = sol;
      has_best = true;
      continue;
    }

    if (sol.error_hz < best.error_hz) {
      best = sol;
      continue;
    }

    if (sol.error_hz == best.error_hz) {
      if (sol.integer_n && !best.integer_n) {
        best = sol;
        continue;
      }
      if ((sol.integer_n == best.integer_n) &&
          (sol.reduced_mod < best.reduced_mod)) {
        best = sol;
        continue;
      }
      if ((sol.integer_n == best.integer_n) &&
          (sol.reduced_mod == best.reduced_mod) && (sol.mod < best.mod)) {
        best = sol;
        continue;
      }
    }
  }

  if (!has_best) return false;
  *out_best = best;
  return true;
}

/**
 * @brief FAST mode: only try ideal_mod, optionally within ±span, to reduce CPU
 * work
 *
 * @note
 * span=0 tries only ideal_mod and is fastest; larger span can improve the
 * chance of lower error.
 */
static bool adf4351_choose_fast_solution(uint64_t pfd_num, uint32_t pfd_den,
                                         uint64_t vco_target_hz,
                                         uint8_t rf_div_sel,
                                         uint32_t desired_step_hz, uint8_t span,
                                         adf4351_solution_t *out_best) {
  if ((pfd_num == 0u) || (pfd_den == 0u) || (desired_step_hz == 0u) || (out_best == NULL))
    return false;

  const uint64_t step_num = (uint64_t)desired_step_hz * pfd_den;
  uint64_t ideal_mod64 = (pfd_num + step_num / 2u) / step_num;
  uint16_t ideal_mod =
      adf4351_clamp_u16((uint32_t)ideal_mod64, ADF_MOD_MIN, ADF_MOD_MAX);

  uint32_t start =
      (ideal_mod > span) ? ((uint32_t)ideal_mod - span) : (uint32_t)ADF_MOD_MIN;
  uint32_t end = (uint32_t)ideal_mod + span;

  if (start < ADF_MOD_MIN) start = ADF_MOD_MIN;
  if (end > ADF_MOD_MAX) end = ADF_MOD_MAX;

  adf4351_solution_t best = (adf4351_solution_t){0};
  bool has_best = false;

  for (uint32_t m = start; m <= end; m++) {
    adf4351_solution_t sol;
    if (!adf4351_try_solution(pfd_num, pfd_den, vco_target_hz, rf_div_sel, (uint16_t)m,
                              &sol)) {
      continue;
    }
    if (!has_best || (sol.error_hz < best.error_hz)) {
      best = sol;
      has_best = true;
      if (sol.error_hz == 0u) {
        break;
      }
    }
  }

  if (!has_best) return false;
  *out_best = best;
  return true;
}

static esy_err_t adf4351_build_plan(const adf4351_ctx_t *ctx,
                                    uint64_t req_out_hz, uint8_t out_pwr,
                                    adf4351_plan_t *plan) {
  if ((ctx == NULL) || (plan == NULL)) return ESY_ERR_INVALID_ARG;

  const uint64_t out_hz = req_out_hz;
  if (out_hz < ADF_OUT_MIN_HZ || out_hz > ADF_OUT_MAX_HZ)
    return ESY_ERR_INVALID_ARG;

  const uint32_t desired_step_hz = ctx->channel_step_hz;

  const uint64_t pfd_num = (uint64_t)ctx->ref_clk_hz *
                           (ctx->ref_doubler ? 2u : 1u);
  const uint32_t pfd_den = ctx->r_counter * (ctx->ref_div2 ? 2u : 1u);
  const uint64_t pfd_hz = adf4351_calc_pfd_hz(ctx);
  if (pfd_hz == 0u) return ESY_ERR_INVALID_ARG;

  uint8_t rf_div_sel = 0;
  uint64_t vco_target_hz = out_hz;
  while ((vco_target_hz < ADF_VCO_MIN_HZ) && (rf_div_sel < 6u)) {
    rf_div_sel++;
    vco_target_hz <<= 1;
  }
  if ((vco_target_hz < ADF_VCO_MIN_HZ) || (vco_target_hz > ADF_VCO_MAX_HZ)) {
    return ESY_ERR_FAIL;
  }

  adf4351_solution_t best;
  if (ctx->solve_mode == ESY_ADF4351_SOLVE_MODE_FAST) {
    const uint8_t span = ctx->fast_mod_span;
    if (!adf4351_choose_fast_solution(pfd_num, pfd_den, vco_target_hz, rf_div_sel,
                                      desired_step_hz, span, &best)) {
      return ESY_ERR_FAIL;
    }
  } else {
    if (!adf4351_choose_best_solution(pfd_num, pfd_den, vco_target_hz, rf_div_sel,
                                      desired_step_hz, &best)) {
      return ESY_ERR_FAIL;
    }
  }

  const uint16_t INT = best.intn;
  const uint16_t FRAC = best.frac;
  const uint16_t MOD = best.mod;

  /* ADF4351 prescaler and INT have constraints; avoid invalid combinations */
  uint8_t prescaler = (vco_target_hz > 3600000000ULL) ? 1u : 0u;
  if ((prescaler == 0u && INT < 23u) || (prescaler == 1u && INT < 75u)) {
    prescaler = 1u;
    if (INT < 75u) {
      return ESY_ERR_FAIL;
    }
  }

  if ((best.vco_actual_hz < ADF_VCO_MIN_HZ) ||
      (best.vco_actual_hz > ADF_VCO_MAX_HZ)) {
    return ESY_ERR_FAIL;
  }
  if ((best.out_actual_hz < ADF_OUT_MIN_HZ) ||
      (best.out_actual_hz > ADF_OUT_MAX_HZ)) {
    return ESY_ERR_FAIL;
  }

  memset(plan, 0, sizeof(*plan));
  plan->r0 = adf4351_pack_r0(INT, FRAC);
  plan->r1 = adf4351_pack_r1(MOD, 1u, prescaler, 0u);
  plan->r2 = adf4351_pack_r2(ctx->r_counter, ctx->ref_div2, ctx->ref_doubler,
                             ctx->cp_current, ctx->muxout, ctx->noise_mode);
  const bool fast_band = pfd_num > (uint64_t)pfd_den * ctx->band_sel_div * 125000u;
  if (fast_band && (ctx->band_sel_div > 254u ||
      pfd_num > (uint64_t)pfd_den * ctx->band_sel_div * 500000u))
    return ESY_ERR_INVALID_ARG;
  plan->r3 = adf4351_pack_r3(150u, fast_band ? 1u : 0u);
  plan->r4 = adf4351_pack_r4(out_pwr, ctx->main_output_enabled ? 1u : 0u, 0u,
                             rf_div_sel, 1u, ctx->band_sel_div);
  plan->r5 = adf4351_pack_r5(ctx->ld_pin_mode);

  plan->pfd_hz = pfd_hz;
  plan->mod = MOD;
  plan->frac = FRAC;
  plan->intn = INT;
  plan->channel_spacing_hz = (MOD != 0u) ? (pfd_hz / (uint64_t)MOD) : 0u;
  plan->rf_div_sel = rf_div_sel;
  plan->main_power_level = out_pwr;
  plan->current_hz = best.out_actual_hz;

  return ESY_OK;
}

static esy_err_t adf4351_apply_plan(adf4351_ctx_t *ctx,
                                    const adf4351_plan_t *plan) {
  if ((ctx == NULL) || (plan == NULL)) return ESY_ERR_INVALID_ARG;

  {
    const esy_err_t err = adf4351_write_changed(
        ctx, plan->r0, plan->r1, plan->r2, plan->r3, plan->r4, plan->r5);
    if (err != ESY_OK) {
      ctx->r0 = ctx->r1 = ctx->r2 = ctx->r3 = ctx->r4 = ctx->r5 = UINT32_MAX;
      return err;
    }
  }

  ctx->pfd_hz = plan->pfd_hz;
  ctx->mod = plan->mod;
  ctx->frac = plan->frac;
  ctx->intn = plan->intn;
  ctx->channel_spacing_hz = plan->channel_spacing_hz;

  ctx->rf_div_sel = plan->rf_div_sel;
  ctx->main_power_level = plan->main_power_level;

  ctx->current_hz = plan->current_hz;
  return ESY_OK;
}

/**
 * @brief Calculate and apply frequency configuration; set_frequency and
 * set_power_level both use this core path
 */
static esy_err_t adf4351_calc_and_apply(adf4351_ctx_t *ctx, uint64_t req_out_hz,
                                        uint8_t out_pwr) {
  adf4351_plan_t plan;

  esy_err_t err = adf4351_build_plan(ctx, req_out_hz, out_pwr, &plan);
  if (err != ESY_OK) {
    return err;
  }

  err = adf4351_apply_plan(ctx, &plan);
  if (err != ESY_OK) {
    return err;
  }

  return ESY_OK;
}

/* -----------------------------
 * freq_synth vtable impl
 * ----------------------------- */

/**
 * @brief Set frequency through the abstraction entry point
 *
 * @note
 * - Record requested_hz, the user target, so later set_power_level recalculates
 * from the same target frequency.
 * - Actual output can differ from requested_hz because of INT/FRAC/MOD
 * quantization and the RF divider; get_info can query it.
 */
static esy_err_t adf4351_set_frequency_hz(void *c, uint64_t freq_hz) {
  adf4351_ctx_t *ctx = (adf4351_ctx_t *)c;
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;

  const esy_err_t err = adf4351_calc_and_apply(ctx, freq_hz, ctx->main_power_level);
  if (err == ESY_OK) ctx->requested_hz = freq_hz;
  return err;
}

/**
 * @brief Enable or disable output; only the MAIN port is supported
 *
 * @note
 * Only the RF_OUT_EN bit in R4 changes; frequency-related registers are left
 * untouched.
 */
ESY_INTERNAL esy_err_t esy_adf4351_set_output_enable_internal(void *c, esy_freq_synth_port_t port,
                                           bool enable) {
  adf4351_ctx_t *ctx = (adf4351_ctx_t *)c;
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;

  if (port == ESY_FREQ_SYNTH_PORT_MAIN) {
    const uint8_t out_pwr = ctx->main_power_level;
    const uint32_t new_r4 = adf4351_pack_r4(
        out_pwr, enable ? 1u : 0u, 0u, ctx->rf_div_sel, 1u, ctx->band_sel_div);
    const esy_err_t err = adf4351_write_reg(ctx, new_r4);
    if (err != ESY_OK) {
      ctx->r4 = UINT32_MAX;
      return err;
    }

    ctx->r4 = new_r4;
    ctx->main_output_enabled = enable;
    return ESY_OK;
  }
  return ESY_ERR_NOT_SUPPORTED;
}

/**
 * @brief Set output power level (0..3, MAIN port only)
 *
 * @note
 * Power changes affect R4, and this path recalculates and applies from
 * requested_hz. This keeps frequency and current power configuration
 * synchronized while preserving the R0-last update rule.
 */
static esy_err_t adf4351_set_power_level(void *c, esy_freq_synth_port_t port,
                                         uint8_t level) {
  adf4351_ctx_t *ctx = (adf4351_ctx_t *)c;
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if (port != ESY_FREQ_SYNTH_PORT_MAIN) return ESY_ERR_NOT_SUPPORTED;
  if (level > 3u) return ESY_ERR_INVALID_ARG;

  return adf4351_calc_and_apply(ctx, ctx->requested_hz, level);
}

/**
 * @brief Query driver internals for diagnostics or upper-layer display
 *
 * @note
 * - ACTUAL_HZ is the quantized real output.
 * - REQUESTED_HZ is the user-configured target.
 * - PFD/spacing/MOD/FRAC/INT diagnose frequency error and channel planning.
 */
static esy_err_t adf4351_get_info(void *c, esy_freq_synth_info_id_t id,
                                  esy_freq_synth_info_value_t *out_value) {
  adf4351_ctx_t *ctx = (adf4351_ctx_t *)c;
  if ((ctx == NULL) || (out_value == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  memset(out_value, 0, sizeof(*out_value));

  switch (id) {
    case ESY_FREQ_SYNTH_INFO_ACTUAL_HZ:
      out_value->u64 = ctx->current_hz;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_REQUESTED_HZ:
      out_value->u64 = ctx->requested_hz;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_PFD_HZ:
      if (ctx->pfd_hz == 0u) return ESY_ERR_NOT_SUPPORTED;
      out_value->u64 = ctx->pfd_hz;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_CHANNEL_SPACING_HZ:
      if (ctx->channel_spacing_hz == 0u) return ESY_ERR_NOT_SUPPORTED;
      out_value->u64 = ctx->channel_spacing_hz;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_MOD:
      if (ctx->mod == 0u) return ESY_ERR_NOT_SUPPORTED;
      out_value->u16 = ctx->mod;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_FRAC:
      out_value->u16 = ctx->frac;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_INT:
      out_value->u16 = ctx->intn;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED:
      out_value->u8 = ctx->main_output_enabled ? 1u : 0u;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_POWER_LEVEL:
      out_value->u8 = ctx->main_power_level;
      return ESY_OK;

    case ESY_FREQ_SYNTH_INFO_RF_DIV_SEL:
      out_value->u8 = ctx->rf_div_sel;
      return ESY_OK;

    default:
      return ESY_ERR_NOT_SUPPORTED;
  }
}

/* -----------------------------
 * Public API
 * ----------------------------- */

/**
 * @brief Fill the default configuration with conservative common values
 *
 * @note
 * - solve_mode defaults to FAST.
 * - fast_mod_span defaults to 0, trying only ideal_mod for the fastest path.
 */
void esy_adf4351_default_config(esy_adf4351_config_t *config) {
  if (config == NULL) return;

  memset(config, 0, sizeof(*config));
  config->spi = NULL;
  config->spi_speed_hz = 0;

  config->le_gpio = NULL;
  config->ce_gpio = NULL;

  config->ref_clk_hz = ADF4351_DEFAULT_REF_CLK_HZ;
  config->channel_step_hz = ADF4351_DEFAULT_STEP_HZ;
  config->initial_freq_hz = ADF4351_DEFAULT_INITIAL_FREQ_HZ;
  config->initial_output_enabled = false;

  config->r_counter = ADF4351_DEFAULT_R_COUNTER;
  config->ref_div2 = ADF4351_DEFAULT_REF_DIV2;
  config->ref_doubler = ADF4351_DEFAULT_REF_DOUBLER;

  config->pfd_max_hz = ADF4351_DEFAULT_PFD_MAX_HZ;

  config->le_pulse_delay_us = 0u;

  config->cp_current = ADF4351_DEFAULT_CP_CURRENT;
  config->muxout = ADF4351_DEFAULT_MUXOUT;
  config->noise_mode = ADF4351_DEFAULT_NOISE_MODE;
  config->band_sel_div = ADF4351_DEFAULT_BAND_SEL_DIV;
  config->ld_pin_mode = ADF4351_DEFAULT_LD_PIN_MODE;

  config->solve_mode = ADF4351_DEFAULT_SOLVE_MODE;
  config->fast_mod_span = ADF4351_DEFAULT_FAST_MOD_SPAN;
}

/**
 * @brief Validate configuration fields that would make calculation, registers,
 * or hardware semantics invalid
 */
static bool adf4351_validate_config(const esy_adf4351_config_t *cfg) {
  if ((cfg->ref_clk_hz == 0u) || (cfg->channel_step_hz == 0u)) return false;
  if ((cfg->pfd_max_hz == 0u) || (cfg->pfd_max_hz > ADF_PFD_MAX_HZ))
    return false;

  if ((cfg->r_counter < ADF_R_COUNTER_MIN) ||
      (cfg->r_counter > ADF_R_COUNTER_MAX))
    return false;
  if ((cfg->ref_div2 > 1u) || (cfg->ref_doubler > 1u)) return false;
  if ((cfg->initial_freq_hz < ADF_OUT_MIN_HZ) ||
      (cfg->initial_freq_hz > ADF_OUT_MAX_HZ)) {
    return false;
  }

  const uint64_t ref_mul = (cfg->ref_doubler != 0u) ? 2u : 1u;
  const uint64_t ref_div =
      (uint64_t)cfg->r_counter * ((cfg->ref_div2 != 0u) ? 2u : 1u);
  const uint64_t pfd_hz = ((uint64_t)cfg->ref_clk_hz * ref_mul) / ref_div;
  if ((pfd_hz == 0u) || ((uint64_t)cfg->ref_clk_hz * ref_mul >
                          cfg->pfd_max_hz * ref_div)) return false;

  if ((uint32_t)cfg->cp_current > (uint32_t)ESY_ADF4351_CP_CURRENT_SETTING_15)
    return false;
  if ((uint32_t)cfg->muxout > (uint32_t)ESY_ADF4351_MUXOUT_DIGITAL_LOCK_DETECT)
    return false;
  if ((uint32_t)cfg->noise_mode > (uint32_t)ESY_ADF4351_NOISE_MODE_3)
    return false;
  if (cfg->band_sel_div == 0u) return false;
  if ((uint32_t)cfg->ld_pin_mode > (uint32_t)ESY_ADF4351_LD_PIN_MODE_3)
    return false;

  if (!((cfg->solve_mode == ESY_ADF4351_SOLVE_MODE_PRECISE) ||
        (cfg->solve_mode == ESY_ADF4351_SOLVE_MODE_FAST))) {
    return false;
  }

  return true;
}

/**
 * @brief Initialize and bind to @ref esy_freq_synth_t
 *
 * @details
 * - Initialize GPIO, with LE required and CE optional.
 * - Bind the vtable.
 * - Write the configured initial frequency so the chip enters a usable state.
 */

static const esy_freq_synth_vtable_t k_adf4351_ops = {
    .set_frequency_hz = adf4351_set_frequency_hz,
    .set_output_enable = esy_adf4351_set_output_enable_internal,
    .set_power_level = adf4351_set_power_level,
    .get_info = adf4351_get_info,
};

ESY_INTERNAL adf4351_ctx_t *esy_adf4351_get_context_internal(
    esy_freq_synth_t *synth) {
  if (synth == NULL || synth->ctx == NULL ||
      synth->ops != &k_adf4351_ops) return NULL;
  return synth->ctx;
}

esy_err_t esy_adf4351_init(esy_freq_synth_t *synth,
                           const esy_adf4351_config_t *config) {
  if ((synth == NULL) || (config == NULL) || (config->spi == NULL) ||
      (config->le_gpio == NULL) || !esy_hal_gpio_is_initialized(config->le_gpio) ||
      ((config->ce_gpio != NULL) && !esy_hal_gpio_is_initialized(config->ce_gpio))) {
    return ESY_ERR_INVALID_ARG;
  }

  const esy_adf4351_config_t cfg = *config;

  if (!adf4351_validate_config(&cfg)) {
    return ESY_ERR_INVALID_ARG;
  }

  if (synth->ops != NULL || synth->ctx != NULL ||
      esy_freq_synth_runtime(synth)->device != NULL ||
      esy_freq_synth_runtime(synth)->faulted)
    return ESY_ERR_STATE;
  memset(synth, 0, sizeof(*synth));

  adf4351_ctx_t *ctx = (adf4351_ctx_t *)synth->backend.bytes;
  memset(ctx, 0, sizeof(*ctx));

  ctx->spi = cfg.spi;
  ctx->le_gpio = cfg.le_gpio;
  ctx->ce_gpio = cfg.ce_gpio;
  ctx->hw_ops = &k_adf4351_platform_hw_ops;

  ctx->ref_clk_hz = cfg.ref_clk_hz;
  ctx->channel_step_hz = cfg.channel_step_hz;

  ctx->r_counter = cfg.r_counter;
  ctx->ref_div2 = cfg.ref_div2;
  ctx->ref_doubler = cfg.ref_doubler;

  ctx->pfd_max_hz = cfg.pfd_max_hz;

  ctx->le_pulse_delay_us = cfg.le_pulse_delay_us;

  ctx->cp_current = cfg.cp_current;
  ctx->muxout = cfg.muxout;
  ctx->noise_mode = cfg.noise_mode;
  ctx->band_sel_div = cfg.band_sel_div;
  ctx->ld_pin_mode = cfg.ld_pin_mode;

  ctx->solve_mode = cfg.solve_mode;
  ctx->fast_mod_span = cfg.fast_mod_span;

  synth->ctx = ctx;
  esy_freq_synth_runtime(synth)->faulted = true;
  synth->ops = &k_adf4351_ops;

  if (esy_hal_gpio_write(ctx->le_gpio, false) != ESY_OK) {
    (void)esy_adf4351_deinit(synth);
    return ESY_ERR_FAIL;
  }

  if (ctx->ce_gpio != NULL) {
    if (esy_hal_gpio_write(ctx->ce_gpio, true) != ESY_OK) {
      (void)esy_adf4351_deinit(synth);
      return ESY_ERR_FAIL;
    }
  }

  if (cfg.spi_speed_hz != 0u) {
    (void)esy_hal_spi_set_speed(ctx->spi, cfg.spi_speed_hz);
  }

  ctx->main_power_level = 0u;
  ctx->main_output_enabled = cfg.initial_output_enabled;
  ctx->requested_hz = cfg.initial_freq_hz;

  {
    const esy_err_t err =
        adf4351_calc_and_apply(ctx, ctx->requested_hz, ctx->main_power_level);
    if (err != ESY_OK) {
      esy_adf4351_deinit(synth);
      return err;
    }
  }

  esy_freq_synth_runtime(synth)->faulted = false;
  return ESY_OK;
}

/**
 * @brief Deinitialize: disable output, release GPIO, and detach the abstraction
 */
esy_err_t esy_adf4351_deinit(esy_freq_synth_t *synth) {
  if (synth == NULL) return ESY_ERR_INVALID_ARG;
  if (esy_freq_synth_runtime(synth)->device != NULL) return ESY_ERR_BUSY;
  if (synth->ctx == NULL) return ESY_OK;
  if (synth->ops != &k_adf4351_ops)
    return ESY_ERR_STATE;
  adf4351_ctx_t *ctx = synth->ctx;
  esy_err_t ret = esy_adf4351_set_output_enable_internal(ctx, ESY_FREQ_SYNTH_PORT_MAIN, false);
  esy_err_t err;
  if (ctx->ce_gpio != NULL) {
    err = esy_hal_gpio_write(ctx->ce_gpio, false);
    if (ret == ESY_OK) ret = err;
  }
  err = esy_hal_gpio_write(ctx->le_gpio, false);
  if (ret == ESY_OK) ret = err;
  esy_freq_synth_runtime(synth)->faulted = ret != ESY_OK;
  if (ret == ESY_OK) esy_freq_synth_detach(synth);
  return ret;
}
