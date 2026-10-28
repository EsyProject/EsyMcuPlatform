/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_lmx2594.h"

#include <limits.h>
#include <string.h>

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_system.h"

#define LMX2594_REQUIRED_REGISTERS 79u
#define LMX2594_REGISTER_MAX 112u
#define LMX2594_REFERENCE_MIN_HZ 5000000u
#define LMX2594_REFERENCE_MAX_HZ 200000000u
#define LMX2594_PFD_MIN_HZ 5000000u
#define LMX2594_PFD_MAX_HZ 100000000u
#define LMX2594_SPI_MAX_HZ 5000000u
#define LMX2594_VCO_MIN_HZ 7500000000ULL
#define LMX2594_VCO_MAX_HZ 15000000000ULL
#define LMX2594_MULTISEGMENT_VCO_MAX_HZ 11500000000ULL
#define LMX2594_OUTPUT_MIN_HZ 10000000ULL
#define LMX2594_PLL_N_MAX 0x7FFFFu
#define LMX2594_LOCK_GUARD_US 50u
#define LMX2594_LOCK_POLL_US 5u
#define LMX2594_FCAL_EN 0x0008u
#define LMX2594_MUXOUT_LD 0x0004u
#define LMX2594_POWERDOWN 0x0001u
#define LMX2594_RESET 0x0002u
#define LMX2594_OUTA_PD 0x0040u
#define LMX2594_OUTB_PD 0x0080u
#define LMX2594_OUTA_POWER_MASK 0x3F00u
#define LMX2594_OUTB_POWER_MASK 0x003Fu
#define LMX2594_OUTA_MUX_VCO 0x0800u

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
  uint32_t cs_high_delay_us;
  uint16_t divider;
  uint16_t reg0;
  uint16_t reg44;
  uint16_t reg45;
  uint16_t reg46;
  uint8_t divider_code;
  bool guard_pending;
} lmx2594_ctx_t;

typedef struct {
  uint64_t vco_hz;
  uint32_t integer;
  uint32_t numerator;
  uint32_t denominator;
  uint8_t mash_order;
  uint8_t pfd_delay;
  uint8_t vco_start;
} lmx2594_plan_t;

_Static_assert(sizeof(lmx2594_ctx_t) <= ESY_FREQ_SYNTH_PRIV_BYTES,
               "LMX2594 context exceeds synthesizer backend storage");

/* SNAS696C Table 23: preserve every unnamed bit, including unused functions.
 * Named fields are patched below. R79..R106 are unnecessary with RAMP_EN=0;
 * R107..R112 are read-only. No-assist calibration follows Table 5. */
static const uint16_t k_register_defaults[LMX2594_REQUIRED_REGISTERS] = {
    0x2414u, 0x0808u, 0x0500u, 0x0642u, 0x1943u, 0x00C8u, 0xC802u,
    0x00B2u, 0x2000u, 0x0604u, 0x10D8u, 0x0018u, 0x5001u, 0x4000u,
    0x1E40u, 0x064Fu, 0x0080u, 0x012Cu, 0x0064u, 0x27B7u, 0xF848u,
    0x0401u, 0x0001u, 0x007Cu, 0x071Au, 0x0C2Bu, 0x0DB0u, 0x0002u,
    0x0488u, 0x318Cu, 0x318Cu, 0x43ECu, 0x0393u, 0x1E21u, 0x0000u,
    0x0004u, 0x0050u, 0x0304u, 0x0000u, 0x0001u, 0x0000u, 0x0000u,
    0x0000u, 0x0000u, 0x1FE0u, 0xC0DFu, 0x07FDu, 0x0300u, 0x0300u,
    0x4180u, 0x0000u, 0x0080u, 0x0820u, 0x0000u, 0x0000u, 0x0000u,
    0x0000u, 0x0020u, 0x8001u, 0x0001u, 0x03E8u, 0x00A8u, 0x0322u,
    0x0000u, 0x1388u, 0x0000u, 0x01F4u, 0x0000u, 0x03E8u, 0x0000u,
    0x0000u, 0x0081u, 0x0001u, 0x0000u, 0x0000u, 0x0840u, 0x000Cu,
    0x0000u, 0x016Fu,
};

static const uint16_t k_dividers[] = {
    2u, 4u, 6u, 8u, 12u, 16u, 24u, 32u, 48u,
    64u, 72u, 96u, 128u, 192u, 256u, 384u, 512u, 768u,
};

static bool lmx2594_divider_code(uint16_t divider, uint8_t *code) {
  if (divider == 1u) {
    *code = 0u;
    return true;
  }
  for (size_t i = 0u; i < sizeof(k_dividers) / sizeof(k_dividers[0]); i++) {
    if (k_dividers[i] == divider) {
      *code = (uint8_t)i;
      return true;
    }
  }
  return false;
}

static uint32_t lmx2594_gcd(uint32_t a, uint32_t b) {
  while (b != 0u) {
    const uint32_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

static esy_err_t lmx2594_plan(uint32_t pfd_hz, uint16_t divider,
                             uint64_t frequency_hz, lmx2594_plan_t *plan) {
  if (frequency_hz < LMX2594_OUTPUT_MIN_HZ ||
      frequency_hz > LMX2594_VCO_MAX_HZ / divider)
    return ESY_ERR_INVALID_ARG;
  const uint64_t vco_hz = frequency_hz * divider;
  if (vco_hz < LMX2594_VCO_MIN_HZ ||
      (divider >= 8u && vco_hz > LMX2594_MULTISEGMENT_VCO_MAX_HZ))
    return ESY_ERR_INVALID_ARG;
  const uint32_t integer = (uint32_t)(vco_hz / pfd_hz);
  const uint32_t remainder = (uint32_t)(vco_hz % pfd_hz);
  if (integer > LMX2594_PLL_N_MAX) return ESY_ERR_INVALID_ARG;
  memset(plan, 0, sizeof(*plan));
  plan->vco_hz = vco_hz;
  plan->integer = integer;
  plan->denominator = 1u;
  if (remainder != 0u) {
    const uint32_t gcd = lmx2594_gcd(remainder, pfd_hz);
    plan->numerator = remainder / gcd;
    plan->denominator = pfd_hz / gcd;
    plan->mash_order = 3u;
    plan->pfd_delay = vco_hz <= 10000000000ULL ? 3u : 4u;
    if (integer < (vco_hz <= 10000000000ULL ? 36u : 40u))
      return ESY_ERR_INVALID_ARG;
  } else {
    plan->pfd_delay = vco_hz <= 12500000000ULL ? 1u : 2u;
    if (integer < (vco_hz <= 12500000000ULL ? 28u : 32u))
      return ESY_ERR_INVALID_ARG;
  }
  /* TI explicitly requires VCO4 rather than VCO7 across this interval. */
  plan->vco_start = vco_hz >= 11900000000ULL && vco_hz <= 12100000000ULL
                        ? 4u : 7u;
  return ESY_OK;
}

static esy_err_t lmx2594_finish_frame(lmx2594_ctx_t *ctx, esy_err_t result) {
  const esy_err_t release = esy_hal_gpio_write(ctx->cs, true);
  if (ctx->cs_high_delay_us != 0u)
    esy_hal_system_delay_us(ctx->cs_high_delay_us);
  return result != ESY_OK ? result : release;
}

static esy_err_t lmx2594_write(lmx2594_ctx_t *ctx, uint8_t address,
                              uint16_t value) {
  const uint8_t frame[] = {address, (uint8_t)(value >> 8u), (uint8_t)value};
  esy_err_t err = esy_hal_gpio_write(ctx->cs, false);
  if (err != ESY_OK) return err;
  const int32_t written = esy_hal_spi_write_blocking(ctx->spi, frame,
                                                   sizeof(frame));
  return lmx2594_finish_frame(ctx, written == (int32_t)sizeof(frame)
                                     ? ESY_OK : ESY_ERR_IO);
}

static esy_err_t lmx2594_read(lmx2594_ctx_t *ctx, uint8_t address,
                             uint16_t *value) {
  const uint8_t frame[] = {(uint8_t)(0x80u | address), 0u, 0u};
  uint8_t received[sizeof(frame)] = {0};
  esy_err_t err = esy_hal_gpio_write(ctx->cs, false);
  if (err == ESY_OK) {
    const int32_t count = esy_hal_spi_transfer_blocking(
        ctx->spi, frame, received, sizeof(frame));
    err = lmx2594_finish_frame(ctx, count == (int32_t)sizeof(frame)
                                       ? ESY_OK : ESY_ERR_IO);
  }
  if (err == ESY_OK)
    *value = (uint16_t)(((uint16_t)received[1] << 8u) | received[2]);
  return err;
}

static uint16_t lmx2594_frequency_register(const lmx2594_ctx_t *ctx,
                                          const lmx2594_plan_t *plan,
                                          uint8_t address) {
  switch (address) {
    case 20u: return (uint16_t)(0xC048u | ((uint16_t)plan->vco_start << 11u));
    case 34u: return (uint16_t)(plan->integer >> 16u);
    case 36u: return (uint16_t)plan->integer;
    case 37u: return (uint16_t)(0x0004u | ((uint16_t)plan->pfd_delay << 8u));
    case 38u: return (uint16_t)(plan->denominator >> 16u);
    case 39u: return (uint16_t)plan->denominator;
    case 42u: return (uint16_t)(plan->numerator >> 16u);
    case 43u: return (uint16_t)plan->numerator;
    case 44u: return (uint16_t)((ctx->reg44 & ~0x0007u) | plan->mash_order);
    default: return k_register_defaults[address];
  }
}

static void lmx2594_commit_plan(lmx2594_ctx_t *ctx,
                                const lmx2594_plan_t *plan,
                                uint64_t frequency_hz) {
  ctx->requested_hz = frequency_hz;
  ctx->actual_hz = frequency_hz;
  ctx->integer = plan->integer;
  ctx->numerator = plan->numerator;
  ctx->denominator = plan->denominator;
  ctx->reg44 = lmx2594_frequency_register(ctx, plan, 44u);
  ctx->last_calibration_us = esy_hal_system_time_32_us();
  ctx->guard_pending = true;
}

static esy_err_t lmx2594_set_frequency(void *context, uint64_t frequency_hz) {
  lmx2594_ctx_t *ctx = context;
  lmx2594_plan_t plan;
  esy_err_t err = lmx2594_plan(ctx->pfd_hz, ctx->divider, frequency_hz, &plan);
  if (err != ESY_OK) return err;
  /* Write N first, then fractional fields and calibration starting core. */
  static const uint8_t k_tune_registers[] = {
      34u, 36u, 38u, 39u, 42u, 43u, 37u, 44u, 20u,
  };
  for (size_t i = 0u; i < sizeof(k_tune_registers); i++) {
    const uint8_t address = k_tune_registers[i];
    err = lmx2594_write(ctx, address,
                        lmx2594_frequency_register(ctx, &plan, address));
    if (err != ESY_OK) break;
  }
  if (err == ESY_OK)
    err = lmx2594_write(ctx, 0u, (uint16_t)(ctx->reg0 | LMX2594_FCAL_EN));
  if (err == ESY_OK) lmx2594_commit_plan(ctx, &plan, frequency_hz);
  else esy_freq_synth_runtime(ctx->owner)->faulted = true;
  return err;
}

static esy_err_t lmx2594_set_enable(void *context, esy_freq_synth_port_t port,
                                   bool enabled) {
  lmx2594_ctx_t *ctx = context;
  if (port != ESY_FREQ_SYNTH_PORT_MAIN && port != ESY_FREQ_SYNTH_PORT_AUX)
    return ESY_ERR_NOT_SUPPORTED;
  const uint16_t mask = port == ESY_FREQ_SYNTH_PORT_MAIN
                            ? LMX2594_OUTA_PD : LMX2594_OUTB_PD;
  const uint16_t next = enabled ? (uint16_t)(ctx->reg44 & ~mask)
                                : (uint16_t)(ctx->reg44 | mask);
  esy_err_t err = ESY_OK;
  if (port == ESY_FREQ_SYNTH_PORT_AUX && enabled) {
    /* Route B through the fixed divider only while it is in use. */
    const uint16_t reg46 = ctx->divider != 1u ? 0x07FCu : 0x07FDu;
    err = lmx2594_write(ctx, 46u, reg46);
    if (err == ESY_OK) ctx->reg46 = reg46;
  }
  if (err == ESY_OK) err = lmx2594_write(ctx, 44u, next);
  if (err == ESY_OK) ctx->reg44 = next;
  /* Power B down before changing its route, avoiding a live VCO transient. */
  if (err == ESY_OK && port == ESY_FREQ_SYNTH_PORT_AUX && !enabled) {
    err = lmx2594_write(ctx, 46u, 0x07FDu);
    if (err == ESY_OK) ctx->reg46 = 0x07FDu;
  }
  if (err != ESY_OK) esy_freq_synth_runtime(ctx->owner)->faulted = true;
  return err;
}

static esy_err_t lmx2594_set_power(void *context, esy_freq_synth_port_t port,
                                  uint8_t level) {
  lmx2594_ctx_t *ctx = context;
  if (level > 63u) return ESY_ERR_INVALID_ARG;
  if (port != ESY_FREQ_SYNTH_PORT_MAIN && port != ESY_FREQ_SYNTH_PORT_AUX)
    return ESY_ERR_NOT_SUPPORTED;
  const uint16_t next = port == ESY_FREQ_SYNTH_PORT_MAIN
      ? (uint16_t)((ctx->reg44 & ~LMX2594_OUTA_POWER_MASK) |
                   ((uint16_t)level << 8u))
      : (uint16_t)((ctx->reg45 & ~LMX2594_OUTB_POWER_MASK) | level);
  const esy_err_t err = lmx2594_write(ctx,
      port == ESY_FREQ_SYNTH_PORT_MAIN ? 44u : 45u, next);
  if (err == ESY_OK) {
    if (port == ESY_FREQ_SYNTH_PORT_MAIN) ctx->reg44 = next;
    else ctx->reg45 = next;
  } else esy_freq_synth_runtime(ctx->owner)->faulted = true;
  return err;
}

static esy_err_t lmx2594_get_info(void *context, esy_freq_synth_info_id_t id,
                                 esy_freq_synth_info_value_t *out) {
  const lmx2594_ctx_t *ctx = context;
  esy_freq_synth_info_value_t value = {0};
  switch (id) {
    case ESY_FREQ_SYNTH_INFO_ACTUAL_HZ: value.u64 = ctx->actual_hz; break;
    case ESY_FREQ_SYNTH_INFO_REQUESTED_HZ: value.u64 = ctx->requested_hz; break;
    case ESY_FREQ_SYNTH_INFO_PFD_HZ: value.u64 = ctx->pfd_hz; break;
    case ESY_FREQ_SYNTH_INFO_CHANNEL_SPACING_HZ:
      value.u64 = ctx->pfd_hz / ctx->denominator / ctx->divider; break;
    case ESY_FREQ_SYNTH_INFO_MOD: value.u32 = ctx->denominator; break;
    case ESY_FREQ_SYNTH_INFO_FRAC: value.u32 = ctx->numerator; break;
    case ESY_FREQ_SYNTH_INFO_INT: value.u32 = ctx->integer; break;
    case ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED:
      value.u8 = (ctx->reg44 & LMX2594_OUTA_PD) == 0u ? 1u : 0u; break;
    case ESY_FREQ_SYNTH_INFO_POWER_LEVEL:
      value.u8 = (uint8_t)((ctx->reg44 & LMX2594_OUTA_POWER_MASK) >> 8u); break;
    case ESY_FREQ_SYNTH_INFO_RF_DIV_SEL: value.u8 = ctx->divider_code; break;
    default: return ESY_ERR_NOT_SUPPORTED;
  }
  *out = value;
  return ESY_OK;
}


static const esy_freq_synth_vtable_t k_lmx2594_ops = {
    .set_frequency_hz = lmx2594_set_frequency,
    .set_output_enable = lmx2594_set_enable,
    .set_power_level = lmx2594_set_power,
    .get_info = lmx2594_get_info,
};

void esy_lmx2594_default_config(esy_lmx2594_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->spi_speed_hz = LMX2594_SPI_MAX_HZ;
  config->ref_clk_hz = 100000000u;
  config->r_divider = 1u;
  config->output_divider = 4u;
  config->initial_freq_hz = 2000000000ULL;
  config->mute_till_lock = true;
  config->charge_pump_gain = 4u;
  config->output_a_power = 31u;
  config->output_b_power = 31u;
}

esy_err_t esy_lmx2594_init(esy_freq_synth_t *synth,
                          const esy_lmx2594_config_t *config) {
  esy_gpio_config_t cs_config;
  esy_gpio_config_t muxout_config;
  lmx2594_plan_t plan;
  uint8_t divider_code;
  if (synth == NULL || config == NULL || config->spi == NULL ||
      config->cs_gpio == NULL || config->muxout_gpio == NULL ||
      esy_hal_gpio_get_config(config->cs_gpio, &cs_config) != ESY_OK ||
      esy_hal_gpio_get_config(config->muxout_gpio, &muxout_config) != ESY_OK ||
      config->cs_gpio == config->muxout_gpio ||
      cs_config.pin == muxout_config.pin ||
      cs_config.mode != ESY_GPIO_MODE_OUTPUT ||
      muxout_config.mode == ESY_GPIO_MODE_OUTPUT ||
      config->spi_speed_hz > LMX2594_SPI_MAX_HZ ||
      config->ref_clk_hz < LMX2594_REFERENCE_MIN_HZ ||
      config->ref_clk_hz > LMX2594_REFERENCE_MAX_HZ ||
      config->r_divider == 0u || config->r_divider > 255u ||
      config->ref_clk_hz % config->r_divider != 0u ||
      config->ref_clk_hz / config->r_divider < LMX2594_PFD_MIN_HZ ||
      config->ref_clk_hz / config->r_divider > LMX2594_PFD_MAX_HZ ||
      !lmx2594_divider_code(config->output_divider, &divider_code) ||
      config->output_a_power > 63u || config->output_b_power > 63u ||
      (config->charge_pump_gain != 1u && config->charge_pump_gain != 3u &&
       config->charge_pump_gain != 4u && config->charge_pump_gain != 5u &&
       config->charge_pump_gain != 7u)) return ESY_ERR_INVALID_ARG;
  const uint32_t pfd_hz = config->ref_clk_hz / config->r_divider;
  esy_err_t err = lmx2594_plan(pfd_hz, config->output_divider,
                              config->initial_freq_hz, &plan);
  if (err != ESY_OK) return err;
  if (synth->ops != NULL || synth->ctx != NULL ||
      esy_freq_synth_runtime(synth)->device != NULL ||
      esy_freq_synth_runtime(synth)->faulted)
    return ESY_ERR_STATE;
  memset(synth, 0, sizeof(*synth));
  lmx2594_ctx_t *ctx = (lmx2594_ctx_t *)synth->backend.bytes;
  ctx->owner = synth;
  ctx->spi = config->spi;
  ctx->cs = config->cs_gpio;
  ctx->muxout = config->muxout_gpio;
  ctx->pfd_hz = pfd_hz;
  ctx->divider = config->output_divider;
  ctx->divider_code = divider_code;
  ctx->cs_high_delay_us = config->cs_high_delay_us;
  ctx->reg0 = (uint16_t)(k_register_defaults[0] |
                         (config->mute_till_lock ? 0x0200u : 0u) |
                         (pfd_hz < 10000000u ? 0x0020u : 0u));
  ctx->reg44 = (uint16_t)(((uint16_t)config->output_a_power << 8u) |
                        LMX2594_OUTA_PD | LMX2594_OUTB_PD | 0x0020u);
  ctx->reg45 = (uint16_t)(0xC0C0u | config->output_b_power |
                        (ctx->divider == 1u ? LMX2594_OUTA_MUX_VCO : 0u));
  ctx->reg46 = 0x07FDu;
  synth->ctx = ctx;
  esy_freq_synth_runtime(synth)->faulted = true;
  synth->ops = &k_lmx2594_ops;
  err = esy_hal_gpio_write(ctx->cs, true);
  if (err == ESY_OK && config->spi_speed_hz != 0u)
    err = esy_hal_spi_set_speed(ctx->spi, config->spi_speed_hz);
  if (err == ESY_OK)
    err = lmx2594_write(ctx, 0u, (uint16_t)(ctx->reg0 | LMX2594_RESET));
  if (err == ESY_OK) err = lmx2594_write(ctx, 0u, ctx->reg0);
  for (uint8_t remaining = LMX2594_REQUIRED_REGISTERS;
       err == ESY_OK && remaining != 0u; remaining--) {
    const uint8_t address = (uint8_t)(remaining - 1u);
    uint16_t value = lmx2594_frequency_register(ctx, &plan, address);
    switch (address) {
      case 0u: value = ctx->reg0; break;
      case 7u: value |= config->mute_till_lock ? 0u : 0x4000u; break;
      case 11u: value = (uint16_t)(0x0008u | (config->r_divider << 4u)); break;
      case 14u: value = (uint16_t)(0x1E00u |
                                  ((uint16_t)config->charge_pump_gain << 4u));
                break;
      case 31u: value = (uint16_t)(0x03ECu |
                                  (ctx->divider > 2u ? 0x4000u : 0u)); break;
      case 45u: value = ctx->reg45; break;
      case 46u: value = ctx->reg46; break;
      case 75u: value = (uint16_t)(0x0800u |
                                  ((uint16_t)divider_code << 6u)); break;
      default: break;
    }
    err = lmx2594_write(ctx, address, value);
  }
  if (err == ESY_OK) {
    esy_hal_system_delay_ms(10u);
    err = lmx2594_write(ctx, 0u, (uint16_t)(ctx->reg0 | LMX2594_FCAL_EN));
  }
  if (err == ESY_OK) {
    lmx2594_commit_plan(ctx, &plan, config->initial_freq_hz);
    if (config->initial_output_enabled)
      err = lmx2594_set_enable(ctx, ESY_FREQ_SYNTH_PORT_MAIN, true);
  }
  if (err != ESY_OK) (void)esy_lmx2594_deinit(synth);
  else esy_freq_synth_runtime(synth)->faulted = false;
  return err;
}

esy_err_t esy_lmx2594_deinit(esy_freq_synth_t *synth) {
  if (synth == NULL) return ESY_ERR_INVALID_ARG;
  if (esy_freq_synth_runtime(synth)->device != NULL) return ESY_ERR_BUSY;
  if (synth->ctx == NULL) return ESY_OK;
  if (synth->ops != &k_lmx2594_ops)
    return ESY_ERR_STATE;
  lmx2594_ctx_t *ctx = synth->ctx;
  esy_err_t result = lmx2594_write(ctx, 44u,
      (uint16_t)(ctx->reg44 | LMX2594_OUTA_PD | LMX2594_OUTB_PD));
  esy_err_t err = lmx2594_write(ctx, 0u,
      (uint16_t)(ctx->reg0 | LMX2594_POWERDOWN));
  if (result == ESY_OK) result = err;
  err = esy_hal_gpio_write(ctx->cs, true);
  if (result == ESY_OK) result = err;
  esy_freq_synth_runtime(synth)->faulted = result != ESY_OK;
  if (result == ESY_OK) esy_freq_synth_detach(synth);
  return result;
}

static esy_err_t lmx2594_require(esy_freq_synth_t *synth,
                                lmx2594_ctx_t **ctx) {
  if (synth == NULL) return ESY_ERR_INVALID_ARG;
  if (synth->ctx == NULL || esy_freq_synth_runtime(synth)->faulted ||
      synth->ops != &k_lmx2594_ops)
    return ESY_ERR_STATE;
  *ctx = synth->ctx;
  return ESY_OK;
}

esy_err_t esy_lmx2594_read_register(esy_freq_synth_t *synth, uint8_t address,
                                   uint16_t *value) {
  lmx2594_ctx_t *ctx;
  if (value == NULL || address > LMX2594_REGISTER_MAX)
    return ESY_ERR_INVALID_ARG;
  esy_err_t err = lmx2594_require(synth, &ctx);
  if (err != ESY_OK) return err;
  uint16_t received;
  err = lmx2594_write(ctx, 0u, (uint16_t)(ctx->reg0 & ~LMX2594_MUXOUT_LD));
  if (err == ESY_OK) err = lmx2594_read(ctx, address, &received);
  const esy_err_t restore = lmx2594_write(ctx, 0u, ctx->reg0);
  if (restore != ESY_OK) {
    esy_freq_synth_runtime(synth)->faulted = true;
    return restore;
  }
  /* Every R0 write restarts amplitude calibration, even with FCAL_EN=0. */
  ctx->last_calibration_us = esy_hal_system_time_32_us();
  ctx->guard_pending = true;
  if (err == ESY_OK) *value = received;
  return err;
}

esy_err_t esy_lmx2594_is_locked(esy_freq_synth_t *synth, bool *locked) {
  lmx2594_ctx_t *ctx;
  if (locked == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t err = lmx2594_require(synth, &ctx);
  if (err != ESY_OK) return err;
  if (ctx->guard_pending) {
    if ((uint32_t)(esy_hal_system_time_32_us() - ctx->last_calibration_us) <
        LMX2594_LOCK_GUARD_US) {
      *locked = false;
      return ESY_OK;
    }
    /* Once observed complete, a 32-bit time-counter wrap must not rearm it. */
    ctx->guard_pending = false;
  }
  bool pin_high;
  err = esy_hal_gpio_read(ctx->muxout, &pin_high);
  if (err != ESY_OK) return err;
  *locked = pin_high;
  return ESY_OK;
}

esy_err_t esy_lmx2594_wait_for_lock(esy_freq_synth_t *synth,
                                  uint32_t timeout_us) {
  lmx2594_ctx_t *ctx;
  if (timeout_us > INT32_MAX) return ESY_ERR_INVALID_ARG;
  esy_err_t err = lmx2594_require(synth, &ctx);
  if (err != ESY_OK) return err;
  (void)ctx;
  const uint32_t started = esy_hal_system_time_32_us();
  for (;;) {
    bool locked;
    err = esy_lmx2594_is_locked(synth, &locked);
    if (err != ESY_OK || locked) return err;
    const uint32_t elapsed = esy_hal_system_time_32_us() - started;
    if (elapsed >= timeout_us) return ESY_ERR_TIMEOUT;
    const uint32_t remaining = timeout_us - elapsed;
    esy_hal_system_delay_us(remaining < LMX2594_LOCK_POLL_US
                                ? remaining : LMX2594_LOCK_POLL_US);
  }
}
