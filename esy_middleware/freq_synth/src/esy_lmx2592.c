/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_lmx2592.h"

#include <limits.h>
#include <string.h>

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_system.h"

#define LMX2592_REGISTER_MAX 70u
#define LMX2592_REFERENCE_MIN_HZ 5000000u
#define LMX2592_REFERENCE_MAX_HZ 200000000u
#define LMX2592_PFD_MIN_HZ 5000000u
#define LMX2592_PFD_MAX_HZ 100000000u
#define LMX2592_SPI_MAX_HZ 5000000u
#define LMX2592_VCO_MIN_HZ 3550000000ULL
#define LMX2592_VCO_MAX_HZ 7100000000ULL
#define LMX2592_OUTPUT_MIN_HZ 20000000ULL
#define LMX2592_PLL_N_MAX 4095u
#define LMX2592_N_PRESCALER 2u
#define LMX2592_LOCK_POLL_US 5u
#define LMX2592_CAL_GUARD_100MHZ_US 800u
#define LMX2592_FCAL_EN 0x0008u
#define LMX2592_MUXOUT_LD 0x0004u
#define LMX2592_POWERDOWN 0x0001u
#define LMX2592_RESET 0x0002u
#define LMX2592_OUTA_PD 0x0040u
#define LMX2592_OUTB_PD 0x0080u
#define LMX2592_OUTA_POWER_MASK 0x3F00u
#define LMX2592_OUTB_POWER_MASK 0x003Fu
#define LMX2592_OUTA_MUX_VCO 0x0800u
#define LMX2592_OUTB_MUX_VCO 0x0001u

typedef struct {
  esy_freq_synth_t *owner;
  esy_spi_t *spi;
  esy_gpio_t *cs;
  esy_gpio_t *muxout;
  uint64_t requested_hz;
  uint64_t actual_hz;
  uint32_t pfd_hz;
  uint32_t numerator;
  uint32_t denominator;
  uint32_t integer;
  uint32_t last_calibration_us;
  uint32_t lock_guard_us;
  uint32_t cs_high_delay_us;
  uint16_t configured_divider;
  uint16_t divider;
  uint16_t reg0;
  uint16_t reg46;
  uint16_t reg47;
  bool guard_pending;
} lmx2592_ctx_t;

typedef struct {
  uint16_t divider;
  uint16_t output_min_mhz;
  uint16_t output_max_mhz;
  uint16_t vco_min_mhz;
  uint16_t vco_max_mhz;
  uint8_t segment1;
  uint8_t segment2;
  uint8_t segment3;
  uint8_t segment_select;
} lmx2592_divider_t;

typedef struct {
  uint32_t integer;
  uint32_t numerator;
  uint32_t denominator;
  uint16_t reg30;
  uint16_t reg31;
  uint16_t reg34;
  uint16_t reg35;
  uint16_t reg36;
  uint16_t reg47;
  uint16_t reg48;
  uint16_t divider;
  uint8_t mash_order;
  uint8_t pfd_delay;
} lmx2592_plan_t;

typedef struct {
  uint8_t address;
  uint16_t value;
} lmx2592_register_t;

_Static_assert(sizeof(lmx2592_ctx_t) <= ESY_FREQ_SYNTH_PRIV_BYTES,
               "LMX2592 context exceeds synthesizer backend storage");

/* SNAS646G Figure 7-5: only listed R/W addresses may be programmed. Preserve
 * every unnamed bit. Named configuration/frequency fields are patched below;
 * R68..R70 are read-only. Default amplitude and frequency calibration is used. */
static const lmx2592_register_t k_register_defaults[] = {
    {0u, 0x2214u}, {1u, 0x080Bu}, {2u, 0x0500u}, {4u, 0x1943u},
    {7u, 0x28B2u}, {8u, 0x1084u}, {9u, 0x0302u}, {10u, 0x10D8u},
    {11u, 0x0018u}, {12u, 0x7001u}, {13u, 0x4000u}, {14u, 0x018Du},
    {19u, 0x0965u}, {20u, 0x012Cu}, {22u, 0x2300u}, {23u, 0x8842u},
    {24u, 0x0509u}, {25u, 0x0000u}, {28u, 0x2924u}, {29u, 0x0084u},
    {30u, 0x0034u}, {31u, 0x0401u}, {32u, 0x210Au}, {33u, 0x2A0Au},
    {34u, 0xC3EAu}, {35u, 0x021Du}, {36u, 0x0411u}, {37u, 0x4000u},
    {38u, 0x0036u}, {39u, 0x8204u}, {40u, 0x0000u}, {41u, 0x03E8u},
    {42u, 0x0000u}, {43u, 0x0000u}, {44u, 0x0000u}, {45u, 0x0000u},
    {46u, 0x0FA3u}, {47u, 0x00C0u}, {48u, 0x03FCu}, {59u, 0x0000u},
    {61u, 0x0001u}, {62u, 0x0000u}, {64u, 0x007Fu},
};

/* Table 7-4: output and VCO intervals, plus encoded channel-divider segments.
 * Segment 2/3 codes are 1=divide 2, 2=divide 4, 4=divide 6, 8=divide 8. */
static const lmx2592_divider_t k_dividers[] = {
    {2u, 1775u, 3550u, 3550u, 7100u, 0u, 0u, 0u, 1u},
    {3u, 1184u, 2200u, 3552u, 6600u, 1u, 0u, 0u, 1u},
    {4u, 888u, 1184u, 3552u, 4736u, 0u, 1u, 0u, 2u},
    {6u, 592u, 888u, 3552u, 5328u, 1u, 1u, 0u, 2u},
    {8u, 444u, 592u, 3552u, 4736u, 0u, 2u, 0u, 2u},
    {12u, 296u, 444u, 3552u, 5328u, 0u, 4u, 0u, 2u},
    {16u, 222u, 296u, 3552u, 4736u, 0u, 8u, 0u, 2u},
    {24u, 148u, 222u, 3552u, 5328u, 1u, 8u, 0u, 2u},
    {32u, 111u, 148u, 3552u, 4736u, 0u, 8u, 1u, 4u},
    {36u, 99u, 111u, 3564u, 3996u, 1u, 4u, 1u, 4u},
    {48u, 74u, 99u, 3552u, 4752u, 1u, 8u, 1u, 4u},
    {64u, 56u, 74u, 3584u, 4736u, 0u, 8u, 2u, 4u},
    {96u, 37u, 56u, 3552u, 5376u, 0u, 8u, 4u, 4u},
    {128u, 28u, 37u, 3584u, 4736u, 0u, 8u, 8u, 4u},
    {192u, 20u, 28u, 3840u, 5376u, 1u, 8u, 8u, 4u},
};

static bool lmx2592_power_valid(uint8_t value) {
  return value <= 31u || (value >= 48u && value <= 63u);
}

static bool lmx2592_divider_valid(uint16_t divider) {
  if (divider <= 1u) return true;
  for (size_t i = 0u; i < sizeof(k_dividers) / sizeof(k_dividers[0]); i++)
    if (divider == k_dividers[i].divider) return true;
  return false;
}

static uint32_t lmx2592_gcd(uint32_t a, uint32_t b) {
  while (b != 0u) {
    const uint32_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

static esy_err_t lmx2592_plan(const lmx2592_ctx_t *ctx, uint64_t frequency_hz,
                             lmx2592_plan_t *plan) {
  if (frequency_hz < LMX2592_OUTPUT_MIN_HZ ||
      frequency_hz > LMX2592_VCO_MAX_HZ) return ESY_ERR_INVALID_ARG;
  memset(plan, 0, sizeof(*plan));
  uint64_t vco_hz = frequency_hz;
  if (ctx->configured_divider == 1u ||
      (ctx->configured_divider == 0u && frequency_hz >= LMX2592_VCO_MIN_HZ)) {
    if (frequency_hz < LMX2592_VCO_MIN_HZ) return ESY_ERR_INVALID_ARG;
    plan->divider = 1u;
    plan->reg31 = 0x0081u;
    plan->reg34 = 0xC3CAu;
    plan->reg35 = 0x0019u;
    plan->reg47 = (uint16_t)(ctx->reg47 | LMX2592_OUTA_MUX_VCO);
    plan->reg48 = 0x03FDu;
    if ((ctx->reg46 & LMX2592_OUTB_PD) != 0u) plan->reg31 |= 0x0400u;
  } else {
    const lmx2592_divider_t *selected = NULL;
    for (size_t i = 0u; i < sizeof(k_dividers) / sizeof(k_dividers[0]); i++) {
      const lmx2592_divider_t *candidate = &k_dividers[i];
      if (ctx->configured_divider != 0u &&
          ctx->configured_divider != candidate->divider) continue;
      const uint64_t candidate_vco = frequency_hz * candidate->divider;
      if (frequency_hz >= (uint64_t)candidate->output_min_mhz * 1000000u &&
          frequency_hz <= (uint64_t)candidate->output_max_mhz * 1000000u &&
          candidate_vco >= (uint64_t)candidate->vco_min_mhz * 1000000u &&
          candidate_vco <= (uint64_t)candidate->vco_max_mhz * 1000000u) {
        selected = candidate;
        vco_hz = candidate_vco;
        break;
      }
    }
    if (selected == NULL) return ESY_ERR_INVALID_ARG;
    plan->divider = selected->divider;
    plan->reg31 = 0x0601u;
    plan->reg34 = 0xC3EAu;
    plan->reg35 = (uint16_t)(0x001Bu | ((uint16_t)selected->segment1 << 2u) |
                             ((uint16_t)selected->segment2 << 9u) |
                             (selected->segment2 != 0u ? 0x0080u : 0u) |
                             (selected->segment3 != 0u ? 0x0100u : 0u));
    plan->reg36 = (uint16_t)(0x0400u |
                             ((uint16_t)selected->segment_select << 4u) |
                             selected->segment3);
    plan->reg47 = (uint16_t)(ctx->reg47 & ~LMX2592_OUTA_MUX_VCO);
    plan->reg48 = 0x03FDu;
    if ((ctx->reg46 & LMX2592_OUTB_PD) == 0u) {
      plan->reg36 |= 0x0800u;
      plan->reg48 = 0x03FCu;
    }
  }
  const uint32_t effective_pfd = ctx->pfd_hz * LMX2592_N_PRESCALER;
  plan->integer = (uint32_t)(vco_hz / effective_pfd);
  const uint32_t remainder = (uint32_t)(vco_hz % effective_pfd);
  if (plan->integer > LMX2592_PLL_N_MAX) return ESY_ERR_INVALID_ARG;
  plan->denominator = 1u;
  if (remainder != 0u) {
    const uint32_t gcd = lmx2592_gcd(remainder, effective_pfd);
    plan->numerator = remainder / gcd;
    plan->denominator = effective_pfd / gcd;
    if (plan->integer >= 18u) plan->mash_order = 3u;
    else if (plan->integer >= 16u) plan->mash_order = 2u;
    else if (plan->integer >= 11u) plan->mash_order = 1u;
    else return ESY_ERR_INVALID_ARG;
    plan->pfd_delay = plan->mash_order == 1u ? 1u : 2u;
  } else {
    if (plan->integer < 9u) return ESY_ERR_INVALID_ARG;
    plan->pfd_delay = 1u;
  }
  plan->reg30 = vco_hz >= 6500000000ULL ? 0x00F4u : 0x0034u;
  return ESY_OK;
}

static esy_err_t lmx2592_finish_frame(lmx2592_ctx_t *ctx, esy_err_t result) {
  const esy_err_t release = esy_hal_gpio_write(ctx->cs, true);
  if (ctx->cs_high_delay_us != 0u)
    esy_hal_system_delay_us(ctx->cs_high_delay_us);
  return result != ESY_OK ? result : release;
}

static esy_err_t lmx2592_write(lmx2592_ctx_t *ctx, uint8_t address,
                              uint16_t value) {
  const uint8_t frame[] = {address, (uint8_t)(value >> 8u), (uint8_t)value};
  esy_err_t err = esy_hal_gpio_write(ctx->cs, false);
  if (err != ESY_OK) return err;
  const int32_t written = esy_hal_spi_write_blocking(ctx->spi, frame,
                                                   sizeof(frame));
  return lmx2592_finish_frame(ctx, written == (int32_t)sizeof(frame)
                                     ? ESY_OK : ESY_ERR_IO);
}

static esy_err_t lmx2592_read(lmx2592_ctx_t *ctx, uint8_t address,
                             uint16_t *value) {
  const uint8_t frame[] = {(uint8_t)(0x80u | address), 0u, 0u};
  uint8_t received[sizeof(frame)] = {0};
  esy_err_t err = esy_hal_gpio_write(ctx->cs, false);
  if (err == ESY_OK) {
    const int32_t count = esy_hal_spi_transfer_blocking(
        ctx->spi, frame, received, sizeof(frame));
    err = lmx2592_finish_frame(ctx, count == (int32_t)sizeof(frame)
                                       ? ESY_OK : ESY_ERR_IO);
  }
  if (err == ESY_OK)
    *value = (uint16_t)(((uint16_t)received[1] << 8u) | received[2]);
  return err;
}

static uint16_t lmx2592_frequency_register(const lmx2592_ctx_t *ctx,
                                          const lmx2592_plan_t *plan,
                                          uint8_t address, uint16_t fallback) {
  switch (address) {
    case 30u: return plan->reg30;
    case 31u: return plan->reg31;
    case 34u: return plan->reg34;
    case 35u: return plan->reg35;
    case 36u: return plan->reg36;
    case 38u: return (uint16_t)(plan->integer << 1u);
    case 39u: return (uint16_t)(0x8004u | ((uint16_t)plan->pfd_delay << 8u));
    case 40u: return (uint16_t)(plan->denominator >> 16u);
    case 41u: return (uint16_t)plan->denominator;
    case 44u: return (uint16_t)(plan->numerator >> 16u);
    case 45u: return (uint16_t)plan->numerator;
    case 46u: return (uint16_t)((ctx->reg46 & ~0x0007u) | plan->mash_order);
    case 47u: return plan->reg47;
    case 48u: return plan->reg48;
    default: return fallback;
  }
}

static void lmx2592_commit_plan(lmx2592_ctx_t *ctx,
                                const lmx2592_plan_t *plan,
                                uint64_t frequency_hz) {
  ctx->requested_hz = frequency_hz;
  ctx->actual_hz = frequency_hz;
  ctx->integer = plan->integer;
  ctx->numerator = plan->numerator;
  ctx->denominator = plan->denominator;
  ctx->divider = plan->divider;
  ctx->reg46 = lmx2592_frequency_register(ctx, plan, 46u, 0u);
  ctx->reg47 = plan->reg47;
  ctx->last_calibration_us = esy_hal_system_time_32_us();
  ctx->guard_pending = true;
}

static esy_err_t lmx2592_set_frequency(void *context, uint64_t frequency_hz) {
  lmx2592_ctx_t *ctx = context;
  lmx2592_plan_t plan;
  esy_err_t err = lmx2592_plan(ctx, frequency_hz, &plan);
  if (err != ESY_OK) return err;
  if (plan.divider != ctx->divider) {
    /* Mute the buffers before switching between VCO and divider routes. This
     * does not promise muting throughout subsequent calibration/settling. */
    err = lmx2592_write(ctx, 46u,
        (uint16_t)(ctx->reg46 | LMX2592_OUTA_PD | LMX2592_OUTB_PD));
    static const uint8_t k_route_registers[] = {31u, 34u, 35u, 36u, 47u, 48u};
    for (size_t i = 0u; err == ESY_OK && i < sizeof(k_route_registers); i++) {
      const uint8_t address = k_route_registers[i];
      err = lmx2592_write(ctx, address,
                          lmx2592_frequency_register(ctx, &plan, address, 0u));
    }
  }
  /* Table 7-2 and section 7.5.2: N, fractional fields, then FCAL. */
  static const uint8_t k_tune_registers[] = {
      38u, 40u, 41u, 44u, 45u, 39u, 30u, 46u,
  };
  for (size_t i = 0u; err == ESY_OK && i < sizeof(k_tune_registers); i++) {
    const uint8_t address = k_tune_registers[i];
    err = lmx2592_write(ctx, address,
                        lmx2592_frequency_register(ctx, &plan, address, 0u));
  }
  if (err == ESY_OK)
    err = lmx2592_write(ctx, 0u, (uint16_t)(ctx->reg0 | LMX2592_FCAL_EN));
  if (err == ESY_OK) lmx2592_commit_plan(ctx, &plan, frequency_hz);
  else esy_freq_synth_runtime(ctx->owner)->faulted = true;
  return err;
}

static esy_err_t lmx2592_set_enable(void *context, esy_freq_synth_port_t port,
                                   bool enabled) {
  lmx2592_ctx_t *ctx = context;
  if (port != ESY_FREQ_SYNTH_PORT_MAIN && port != ESY_FREQ_SYNTH_PORT_AUX)
    return ESY_ERR_NOT_SUPPORTED;
  const uint16_t mask = port == ESY_FREQ_SYNTH_PORT_MAIN
                            ? LMX2592_OUTA_PD : LMX2592_OUTB_PD;
  const uint16_t previous = ctx->reg46;
  const uint16_t next = enabled ? (uint16_t)(previous & ~mask)
                                : (uint16_t)(previous | mask);
  esy_err_t err = ESY_OK;
  if (port == ESY_FREQ_SYNTH_PORT_AUX) {
    if (!enabled) err = lmx2592_write(ctx, 46u, next);
    /* Keep B muted until its distribution path is ready. */
    if (err == ESY_OK) {
      ctx->reg46 = next;
      lmx2592_plan_t plan;
      err = lmx2592_plan(ctx, ctx->actual_hz, &plan);
      ctx->reg46 = previous;
      static const uint8_t k_b_route_registers[] = {31u, 36u, 48u};
      for (size_t i = 0u; err == ESY_OK && i < sizeof(k_b_route_registers); i++) {
        const uint8_t address = k_b_route_registers[i];
        err = lmx2592_write(ctx, address,
                            lmx2592_frequency_register(ctx, &plan, address, 0u));
      }
    }
  }
  if (err == ESY_OK && (port != ESY_FREQ_SYNTH_PORT_AUX || enabled))
    err = lmx2592_write(ctx, 46u, next);
  if (err == ESY_OK) ctx->reg46 = next;
  else esy_freq_synth_runtime(ctx->owner)->faulted = true;
  return err;
}

static esy_err_t lmx2592_set_power(void *context, esy_freq_synth_port_t port,
                                  uint8_t level) {
  lmx2592_ctx_t *ctx = context;
  if (!lmx2592_power_valid(level)) return ESY_ERR_INVALID_ARG;
  if (port != ESY_FREQ_SYNTH_PORT_MAIN && port != ESY_FREQ_SYNTH_PORT_AUX)
    return ESY_ERR_NOT_SUPPORTED;
  const uint16_t next = port == ESY_FREQ_SYNTH_PORT_MAIN
      ? (uint16_t)((ctx->reg46 & ~LMX2592_OUTA_POWER_MASK) |
                   ((uint16_t)level << 8u))
      : (uint16_t)((ctx->reg47 & ~LMX2592_OUTB_POWER_MASK) | level);
  const esy_err_t err = lmx2592_write(ctx,
      port == ESY_FREQ_SYNTH_PORT_MAIN ? 46u : 47u, next);
  if (err == ESY_OK) {
    if (port == ESY_FREQ_SYNTH_PORT_MAIN) ctx->reg46 = next;
    else ctx->reg47 = next;
  } else esy_freq_synth_runtime(ctx->owner)->faulted = true;
  return err;
}

static esy_err_t lmx2592_get_info(void *context, esy_freq_synth_info_id_t id,
                                 esy_freq_synth_info_value_t *out) {
  const lmx2592_ctx_t *ctx = context;
  esy_freq_synth_info_value_t value = {0};
  switch (id) {
    case ESY_FREQ_SYNTH_INFO_ACTUAL_HZ: value.u64 = ctx->actual_hz; break;
    case ESY_FREQ_SYNTH_INFO_REQUESTED_HZ: value.u64 = ctx->requested_hz; break;
    case ESY_FREQ_SYNTH_INFO_PFD_HZ: value.u64 = ctx->pfd_hz; break;
    case ESY_FREQ_SYNTH_INFO_CHANNEL_SPACING_HZ:
      value.u64 = (uint64_t)ctx->pfd_hz * LMX2592_N_PRESCALER /
                  ctx->denominator / ctx->divider; break;
    case ESY_FREQ_SYNTH_INFO_MOD: value.u32 = ctx->denominator; break;
    case ESY_FREQ_SYNTH_INFO_FRAC: value.u32 = ctx->numerator; break;
    case ESY_FREQ_SYNTH_INFO_INT: value.u32 = ctx->integer; break;
    case ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED:
      value.u8 = (ctx->reg46 & LMX2592_OUTA_PD) == 0u ? 1u : 0u; break;
    case ESY_FREQ_SYNTH_INFO_POWER_LEVEL:
      value.u8 = (uint8_t)((ctx->reg46 & LMX2592_OUTA_POWER_MASK) >> 8u); break;
    case ESY_FREQ_SYNTH_INFO_RF_DIV_SEL: value.u8 = (uint8_t)ctx->divider; break;
    default: return ESY_ERR_NOT_SUPPORTED;
  }
  *out = value;
  return ESY_OK;
}


static const esy_freq_synth_vtable_t k_lmx2592_ops = {
    .set_frequency_hz = lmx2592_set_frequency,
    .set_output_enable = lmx2592_set_enable,
    .set_power_level = lmx2592_set_power,
    .get_info = lmx2592_get_info,
};

void esy_lmx2592_default_config(esy_lmx2592_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->spi_speed_hz = LMX2592_SPI_MAX_HZ;
  config->ref_clk_hz = 100000000u;
  config->r_divider = 1u;
  config->initial_freq_hz = 2000000000ULL;
  config->charge_pump_current = 12u;
  config->output_a_power = 31u;
  config->output_b_power = 31u;
}

esy_err_t esy_lmx2592_init(esy_freq_synth_t *synth,
                          const esy_lmx2592_config_t *config) {
  esy_gpio_config_t cs_config;
  esy_gpio_config_t muxout_config;
  if (synth == NULL || config == NULL || config->spi == NULL ||
      config->cs_gpio == NULL || config->muxout_gpio == NULL ||
      esy_hal_gpio_get_config(config->cs_gpio, &cs_config) != ESY_OK ||
      esy_hal_gpio_get_config(config->muxout_gpio, &muxout_config) != ESY_OK ||
      config->cs_gpio == config->muxout_gpio ||
      cs_config.pin == muxout_config.pin ||
      cs_config.mode != ESY_GPIO_MODE_OUTPUT ||
      muxout_config.mode == ESY_GPIO_MODE_OUTPUT ||
      config->spi_speed_hz > LMX2592_SPI_MAX_HZ ||
      config->ref_clk_hz < LMX2592_REFERENCE_MIN_HZ ||
      config->ref_clk_hz > LMX2592_REFERENCE_MAX_HZ ||
      config->r_divider == 0u || config->r_divider > 255u ||
      config->ref_clk_hz % config->r_divider != 0u ||
      config->ref_clk_hz / config->r_divider < LMX2592_PFD_MIN_HZ ||
      config->ref_clk_hz / config->r_divider > LMX2592_PFD_MAX_HZ ||
      !lmx2592_divider_valid(config->output_divider) ||
      !lmx2592_power_valid(config->output_a_power) ||
      !lmx2592_power_valid(config->output_b_power) ||
      config->charge_pump_current == 0u || config->charge_pump_current > 31u ||
      (config->charge_pump_multiplier != 0u &&
       config->charge_pump_multiplier != 2u)) return ESY_ERR_INVALID_ARG;
  lmx2592_ctx_t initial = {0};
  initial.pfd_hz = config->ref_clk_hz / config->r_divider;
  initial.configured_divider = config->output_divider;
  initial.reg46 = (uint16_t)(((uint16_t)config->output_a_power << 8u) |
                            LMX2592_OUTA_PD | LMX2592_OUTB_PD | 0x0020u);
  initial.reg47 = (uint16_t)(0x00C0u | config->output_b_power);
  lmx2592_plan_t plan;
  esy_err_t err = lmx2592_plan(&initial, config->initial_freq_hz, &plan);
  if (err != ESY_OK) return err;
  if (synth->ops != NULL || synth->ctx != NULL ||
      esy_freq_synth_runtime(synth)->device != NULL ||
      esy_freq_synth_runtime(synth)->faulted)
    return ESY_ERR_STATE;
  memset(synth, 0, sizeof(*synth));
  lmx2592_ctx_t *ctx = (lmx2592_ctx_t *)synth->backend.bytes;
  *ctx = initial;
  ctx->owner = synth;
  ctx->spi = config->spi;
  ctx->cs = config->cs_gpio;
  ctx->muxout = config->muxout_gpio;
  ctx->cs_high_delay_us = config->cs_high_delay_us;
  ctx->reg0 = (uint16_t)(0x2214u | (ctx->pfd_hz < 10000000u ? 0x0020u : 0u));
  const uint8_t calibration_divider = config->ref_clk_hz >= 200000000u ? 1u : 0u;
  const uint64_t guard_scale = (uint64_t)LMX2592_CAL_GUARD_100MHZ_US *
                              100000000u * (1u << calibration_divider);
  ctx->lock_guard_us = (uint32_t)((guard_scale + config->ref_clk_hz - 1u) /
                                  config->ref_clk_hz);
  synth->ctx = ctx;
  esy_freq_synth_runtime(synth)->faulted = true;
  synth->ops = &k_lmx2592_ops;
  err = esy_hal_gpio_write(ctx->cs, true);
  if (err == ESY_OK && config->spi_speed_hz != 0u)
    err = esy_hal_spi_set_speed(ctx->spi, config->spi_speed_hz);
  if (err == ESY_OK)
    err = lmx2592_write(ctx, 0u, (uint16_t)(ctx->reg0 | LMX2592_RESET));
  if (err == ESY_OK) err = lmx2592_write(ctx, 0u, ctx->reg0);
  for (size_t remaining = sizeof(k_register_defaults) /
                          sizeof(k_register_defaults[0]);
       err == ESY_OK && remaining != 0u; remaining--) {
    const lmx2592_register_t *entry = &k_register_defaults[remaining - 1u];
    uint16_t value = lmx2592_frequency_register(ctx, &plan, entry->address,
                                              entry->value);
    switch (entry->address) {
      case 0u: value = ctx->reg0; break;
      case 1u: value = (uint16_t)(0x0808u | calibration_divider); break;
      case 11u: value = (uint16_t)(0x0008u | (config->r_divider << 4u)); break;
      case 14u: value = (uint16_t)(((uint16_t)config->charge_pump_current << 7u) |
                                  ((uint16_t)config->charge_pump_current << 2u) |
                                  config->charge_pump_multiplier); break;
      default: break;
    }
    err = lmx2592_write(ctx, entry->address, value);
  }
  if (err == ESY_OK) {
    /* Conservative power-on margin; section 7.5.1 does not mandate 10 ms. */
    esy_hal_system_delay_ms(10u);
    err = lmx2592_write(ctx, 0u, (uint16_t)(ctx->reg0 | LMX2592_FCAL_EN));
  }
  if (err == ESY_OK) {
    lmx2592_commit_plan(ctx, &plan, config->initial_freq_hz);
    if (config->initial_output_enabled)
      err = lmx2592_set_enable(ctx, ESY_FREQ_SYNTH_PORT_MAIN, true);
  }
  if (err != ESY_OK) (void)esy_lmx2592_deinit(synth);
  else esy_freq_synth_runtime(synth)->faulted = false;
  return err;
}

esy_err_t esy_lmx2592_deinit(esy_freq_synth_t *synth) {
  if (synth == NULL) return ESY_ERR_INVALID_ARG;
  if (esy_freq_synth_runtime(synth)->device != NULL) return ESY_ERR_BUSY;
  if (synth->ctx == NULL) return ESY_OK;
  if (synth->ops != &k_lmx2592_ops)
    return ESY_ERR_STATE;
  lmx2592_ctx_t *ctx = synth->ctx;
  esy_err_t result = lmx2592_write(ctx, 46u,
      (uint16_t)(ctx->reg46 | LMX2592_OUTA_PD | LMX2592_OUTB_PD));
  esy_err_t err = lmx2592_write(ctx, 0u,
      (uint16_t)(ctx->reg0 | LMX2592_POWERDOWN));
  if (result == ESY_OK) result = err;
  err = esy_hal_gpio_write(ctx->cs, true);
  if (result == ESY_OK) result = err;
  esy_freq_synth_runtime(synth)->faulted = result != ESY_OK;
  if (result == ESY_OK) esy_freq_synth_detach(synth);
  return result;
}

static esy_err_t lmx2592_require(esy_freq_synth_t *synth,
                                lmx2592_ctx_t **ctx) {
  if (synth == NULL) return ESY_ERR_INVALID_ARG;
  if (synth->ctx == NULL || esy_freq_synth_runtime(synth)->faulted ||
      synth->ops != &k_lmx2592_ops)
    return ESY_ERR_STATE;
  *ctx = synth->ctx;
  return ESY_OK;
}

esy_err_t esy_lmx2592_read_register(esy_freq_synth_t *synth, uint8_t address,
                                   uint16_t *value) {
  lmx2592_ctx_t *ctx;
  if (value == NULL || address > LMX2592_REGISTER_MAX)
    return ESY_ERR_INVALID_ARG;
  esy_err_t err = lmx2592_require(synth, &ctx);
  if (err != ESY_OK) return err;
  uint16_t received;
  err = lmx2592_write(ctx, 0u, (uint16_t)(ctx->reg0 & ~LMX2592_MUXOUT_LD));
  if (err == ESY_OK) err = lmx2592_read(ctx, address, &received);
  const esy_err_t restore = lmx2592_write(ctx, 0u, ctx->reg0);
  if (restore != ESY_OK) {
    esy_freq_synth_runtime(synth)->faulted = true;
    return restore;
  }
  /* Diagnostic R0 writes conservatively invalidate the software lock guard. */
  ctx->last_calibration_us = esy_hal_system_time_32_us();
  ctx->guard_pending = true;
  if (err == ESY_OK) *value = received;
  return err;
}

esy_err_t esy_lmx2592_is_locked(esy_freq_synth_t *synth, bool *locked) {
  lmx2592_ctx_t *ctx;
  if (locked == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t err = lmx2592_require(synth, &ctx);
  if (err != ESY_OK) return err;
  if (ctx->guard_pending) {
    if ((uint32_t)(esy_hal_system_time_32_us() - ctx->last_calibration_us) <
        ctx->lock_guard_us) {
      *locked = false;
      return ESY_OK;
    }
    /* Once observed complete, a 32-bit time-counter wrap must not rearm it. */
    ctx->guard_pending = false;
  }
  bool pin_high;
  err = esy_hal_gpio_read(ctx->muxout, &pin_high);
  if (err == ESY_OK) *locked = pin_high;
  return err;
}

esy_err_t esy_lmx2592_wait_for_lock(esy_freq_synth_t *synth,
                                  uint32_t timeout_us) {
  lmx2592_ctx_t *ctx;
  if (timeout_us > INT32_MAX) return ESY_ERR_INVALID_ARG;
  esy_err_t err = lmx2592_require(synth, &ctx);
  if (err != ESY_OK) return err;
  (void)ctx;
  const uint32_t started = esy_hal_system_time_32_us();
  for (;;) {
    bool locked;
    err = esy_lmx2592_is_locked(synth, &locked);
    if (err != ESY_OK || locked) return err;
    const uint32_t elapsed = esy_hal_system_time_32_us() - started;
    if (elapsed >= timeout_us) return ESY_ERR_TIMEOUT;
    const uint32_t remaining = timeout_us - elapsed;
    esy_hal_system_delay_us(remaining < LMX2592_LOCK_POLL_US
                                ? remaining : LMX2592_LOCK_POLL_US);
  }
}
