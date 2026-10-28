/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_adf4372.h"
#include "esy_adf4372_internal.h"

#include <string.h>

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_spi_device.h"
#include "esy_hal/esy_hal_system.h"

#define ADF4372_RESET         0x00u
#define ADF4372_DEVICE_INFO   0x06u
#define ADF4372_INT_L         0x10u
#define ADF4372_INT_H         0x11u
#define ADF4372_AUTOCAL       0x12u
#define ADF4372_R_COUNTER     0x1Fu
#define ADF4372_CP            0x1Eu
#define ADF4372_MUXOUT        0x20u
#define ADF4372_REFIN         0x22u
#define ADF4372_RF_DIV        0x24u
#define ADF4372_RF8           0x25u
#define ADF4372_CP_BLEED      0x26u
#define ADF4372_INT_MODE      0x2Bu
#define ADF4372_ALC_VCO1      0x2Cu
#define ADF4372_ALC_VCO2      0x2Du
#define ADF4372_ALC_VCO3      0x2Eu
#define ADF4372_ALC_VCO4      0x2Fu
#define ADF4372_VCO_FSM       0x33u
#define ADF4372_VCO_OVERRIDE  0x34u
#define ADF4372_VCO_BAND      0x37u
#define ADF4372_VCO_CORE_BIAS 0x38u
#define ADF4372_AUX           0x72u
#define ADF4372_VCO_DATA      0x6Eu
#define ADF4372_VCO_CORE      0x6Fu
#define ADF4372_VCO_FSM_TEST_MODE_MASK            0xE0u
#define ADF4372_VCO_FSM_TEST_MODE_MANUAL_OVERRIDE 0x20u
#define ADF4372_VCO_ALC_TIMEOUT_MASK              0x1Fu
#define ADF4372_MODULUS1      33554432ULL
#define ADF4372_MODULUS2_MAX  16383u
#define ADF4372_PFD_MAX_HZ    125000000ULL
#define ADF4372_REFERENCE_MIN_HZ 10000000u
#define ADF4372_REFERENCE_SINGLE_MAX_HZ 500000000u
#define ADF4372_REFERENCE_DIFF_MAX_HZ 600000000u
#define ADF4372_INT_MIN 23u
#define ADF4372_INT_MAX 32767u
#define ADF4372_VCO_MIN_HZ    4000000000ULL
#define ADF4372_VCO_MAX_HZ    8000000000ULL
#define ADF4372_RF8_MIN_HZ    62500000ULL
#define ADF4372_RF16_MIN_HZ   8000000000ULL
#define ADF4372_RF16_MAX_HZ   16000000000ULL
#define ADF4372_REVISION_MASK 0x0Fu
#define ADF4372_REVISION_9    0x09u
#define ADF4372_REVISION_A    0x0Au
#define ADF4372_FREQUENCY_FIELD_COUNT 10u

enum {
  ADF4372_FAST_SHADOW_R_COUNTER = ADF4372_FREQUENCY_FIELD_COUNT,
  ADF4372_FAST_SHADOW_RF_DIV = 11,
  ADF4372_FAST_SHADOW_CP_BLEED = 12,
  ADF4372_FAST_SHADOW_INT_MODE = 13,
  ADF4372_FAST_SHADOW_VCO_OVERRIDE = 14,
  ADF4372_FAST_SHADOW_VCO_BAND = 15,
  ADF4372_FAST_SHADOW_VCO_CORE_BIAS = 16,
};

typedef struct {
  uint8_t address;
  uint8_t value;
} adf4372_reg_t;

typedef struct {
  uint8_t reg2c;
  uint8_t reg2d;
  uint8_t reg2e;
  uint8_t reg2f;
} adf4372_alc_profile_t;

typedef struct {
  uint64_t actual_hz;
  uint32_t fract1;
  uint16_t integer;
  uint16_t fract2;
  uint16_t mod2;
  uint8_t rf_divider;
} adf4372_plan_t;

_Static_assert(ESY_FREQ_SYNTH_PRIV_BYTES >= sizeof(adf4372_ctx_t),
               "ESY_FREQ_SYNTH_PRIV_BYTES is too small for adf4372_ctx_t");
_Static_assert(ADF4372_FAST_SHADOW_COUNT <= 32u,
               "ADF4372 fast register shadow mask is too small");

static const adf4372_reg_t k_default_registers[] = {
    {0x73u, 0x00u}, {0x72u, 0x32u}, {0x71u, 0x60u}, {0x70u, 0x03u},
    {0x52u, 0xF4u}, {0x47u, 0xC0u}, {0x41u, 0x28u}, {0x40u, 0x50u},
    {0x3Fu, 0x80u}, {0x3Eu, 0x0Cu}, {0x3Au, 0x55u}, {0x39u, 0x07u},
    {0x36u, 0x30u}, {0x35u, 0xFAu}, {0x32u, 0x04u}, {0x2Fu, 0x92u},
    {0x2Eu, 0x10u}, {0x2Du, 0x11u}, {0x2Cu, 0x44u}, {0x28u, 0x83u},
    {0x27u, 0xC5u}, {0x25u, 0x03u}, {0x24u, 0x80u}, {0x23u, 0x00u},
    {0x22u, 0x00u}, {0x20u, 0x14u}, {0x1Eu, 0x48u}, {0x12u, 0x40u},
};

static const adf4372_alc_profile_t k_alc_profiles[2][2] = {
    {
        {0x24u, 0x09u, 0x0Au, 0x0Au},
        {0x44u, 0x11u, 0x12u, 0x94u},
    },
    {
        {0x24u, 0x0Au, 0x0Au, 0x0Au},
        {0x44u, 0x11u, 0x10u, 0x92u},
    },
};

static void adf4372_invalidate_fast_register_shadow(adf4372_ctx_t *ctx) {
  if (ctx != NULL) {
    ctx->fast_register_shadow_valid_mask = 0u;
  }
}

static bool adf4372_fast_register_shadow_index(uint8_t address,
                                               uint8_t *index) {
  if (index == NULL) return false;
  if ((address >= ADF4372_INT_H) &&
      (address < (ADF4372_INT_H + ADF4372_FREQUENCY_FIELD_COUNT))) {
    *index = (uint8_t)(address - ADF4372_INT_H);
    return true;
  }
  switch (address) {
    case ADF4372_R_COUNTER:
      *index = ADF4372_FAST_SHADOW_R_COUNTER;
      return true;
    case ADF4372_RF_DIV:
      *index = ADF4372_FAST_SHADOW_RF_DIV;
      return true;
    case ADF4372_CP_BLEED:
      *index = ADF4372_FAST_SHADOW_CP_BLEED;
      return true;
    case ADF4372_INT_MODE:
      *index = ADF4372_FAST_SHADOW_INT_MODE;
      return true;
    case ADF4372_VCO_OVERRIDE:
      *index = ADF4372_FAST_SHADOW_VCO_OVERRIDE;
      return true;
    case ADF4372_VCO_BAND:
      *index = ADF4372_FAST_SHADOW_VCO_BAND;
      return true;
    case ADF4372_VCO_CORE_BIAS:
      *index = ADF4372_FAST_SHADOW_VCO_CORE_BIAS;
      return true;
    default:
      return false;
  }
}

static void adf4372_record_fast_register_write(adf4372_ctx_t *ctx,
                                               uint8_t address,
                                               uint8_t value) {
  uint8_t index;
  if (!adf4372_fast_register_shadow_index(address, &index)) return;
  ctx->fast_register_shadow[index] = value;
  ctx->fast_register_shadow_valid_mask |= (uint32_t)1u << index;
}

static esy_err_t adf4372_write(adf4372_ctx_t *ctx, uint8_t address,
                               uint8_t value) {
  const uint8_t frame[] = {0u, address, value};
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if (address == ADF4372_RESET) {
    adf4372_invalidate_fast_register_shadow(ctx);
  }
  if (ctx->spi == NULL) {
    adf4372_invalidate_fast_register_shadow(ctx);
    return ESY_ERR_INVALID_ARG;
  }
  esy_err_t ret = esy_hal_gpio_write(ctx->cs_gpio, false);
  if (ret != ESY_OK) {
    adf4372_invalidate_fast_register_shadow(ctx);
    return ret;
  }
  const int32_t written =
      ctx->pm_requester != NULL
          ? esy_hal_spi_write_blocking_for_pm(ctx->spi, ctx->pm_requester, frame, sizeof(frame))
          : esy_hal_spi_write_blocking(ctx->spi, frame, sizeof(frame));
  if (written != (int32_t)sizeof(frame)) {
    (void)esy_hal_gpio_write(ctx->cs_gpio, true);
    adf4372_invalidate_fast_register_shadow(ctx);
    return ESY_ERR_IO;
  }
  ret = esy_hal_gpio_write(ctx->cs_gpio, true);
  if (ret != ESY_OK) {
    adf4372_invalidate_fast_register_shadow(ctx);
    return ret;
  }
  if (ctx->cs_high_delay_us != 0u)
    esy_hal_system_delay_us(ctx->cs_high_delay_us);
  adf4372_record_fast_register_write(ctx, address, value);
  return ESY_OK;
}

static esy_err_t adf4372_write_if_changed(adf4372_ctx_t *ctx,
                                          uint8_t address, uint8_t value) {
  uint8_t index;
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if (adf4372_fast_register_shadow_index(address, &index) &&
      ((ctx->fast_register_shadow_valid_mask & ((uint32_t)1u << index)) !=
       0u) &&
      (ctx->fast_register_shadow[index] == value)) {
    return ESY_OK;
  }
  return adf4372_write(ctx, address, value);
}

static esy_err_t adf4372_read(const adf4372_ctx_t *ctx, uint8_t address,
                              uint8_t *value) {
  const uint8_t frame[] = {0x80u, address};
  if ((ctx == NULL) || (ctx->spi == NULL) || (value == NULL))
    return ESY_ERR_INVALID_ARG;
  esy_err_t ret = esy_hal_gpio_write(ctx->cs_gpio, false);
  if (ret != ESY_OK) return ret;
  if ((ctx->spi_three_wire &&
       (esy_hal_spi_write_then_read_blocking(
            ctx->spi, frame, sizeof(frame), value, 1u) != ESY_OK)) ||
      (!ctx->spi_three_wire &&
       ((esy_hal_spi_write_blocking(ctx->spi, frame, sizeof(frame)) !=
         (int32_t)sizeof(frame)) ||
        (esy_hal_spi_read_blocking(ctx->spi, value, 1u) != 1))))
    ret = ESY_ERR_IO;
  const esy_err_t release = esy_hal_gpio_write(ctx->cs_gpio, true);
  if (ctx->cs_high_delay_us != 0u)
    esy_hal_system_delay_us(ctx->cs_high_delay_us);
  return ret != ESY_OK ? ret : release;
}

static esy_err_t adf4372_read_revision(const adf4372_ctx_t *ctx,
                                       uint8_t *revision) {
  uint8_t value;
  esy_err_t err;
  if (revision == NULL) return ESY_ERR_INVALID_ARG;
  err = adf4372_read(ctx, ADF4372_DEVICE_INFO, &value);
  if (err != ESY_OK) return err;
  value &= ADF4372_REVISION_MASK;
  if ((value != ADF4372_REVISION_9) && (value != ADF4372_REVISION_A))
    return ESY_ERR_IO;
  *revision = value;
  return ESY_OK;
}

static const adf4372_alc_profile_t *adf4372_select_alc_profile(
    uint8_t revision, esy_adf4372_vco_supply_t supply) {
  const uint8_t revision_index = (revision == ADF4372_REVISION_9) ? 0u : 1u;
  return &k_alc_profiles[revision_index][(uint8_t)supply];
}

static uint8_t adf4372_apply_alc_profile(const adf4372_alc_profile_t *profile,
                                         uint8_t address,
                                         uint8_t default_value) {
  if (address == ADF4372_ALC_VCO1) return profile->reg2c;
  if (address == ADF4372_ALC_VCO2) return profile->reg2d;
  if (address == ADF4372_ALC_VCO3) return profile->reg2e;
  if (address == ADF4372_ALC_VCO4) return profile->reg2f;
  return default_value;
}

static uint32_t adf4372_gcd(uint32_t a, uint32_t b) {
  while (b != 0u) {
    const uint32_t r = a % b;
    a = b;
    b = r;
  }
  return a;
}

static bool adf4372_is_main_channel(esy_adf4372_channel_t channel) {
  return (channel == ESY_ADF4372_CHANNEL_RF8) ||
         (channel == ESY_ADF4372_CHANNEL_RF16);
}

static esy_err_t adf4372_build_plan(const adf4372_ctx_t *ctx,
                                    uint64_t frequency_hz,
                                    esy_adf4372_channel_t channel,
                                    adf4372_plan_t *plan) {
  uint64_t vco_hz = frequency_hz;
  uint64_t rem;
  uint64_t scaled;
  uint64_t actual_vco;
  uint32_t fract2;
  uint32_t mod2;
  uint32_t divisor = 1u;
  uint8_t rf_divider = 0u;
  if ((ctx == NULL) || (plan == NULL) || (ctx->ref_clk_hz == 0u) || (ctx->r_counter == 0u))
    return ESY_ERR_INVALID_ARG;
  if (channel == ESY_ADF4372_CHANNEL_RF16) {
    if ((frequency_hz < ADF4372_RF16_MIN_HZ) ||
        (frequency_hz > ADF4372_RF16_MAX_HZ))
      return ESY_ERR_INVALID_ARG;
    vco_hz = frequency_hz / 2u;
  } else if ((channel == ESY_ADF4372_CHANNEL_AUX_RF8) && ctx->aux_vco) {
    if ((frequency_hz < ADF4372_VCO_MIN_HZ) ||
        (frequency_hz > ADF4372_VCO_MAX_HZ))
      return ESY_ERR_INVALID_ARG;
  } else {
    if ((frequency_hz < ADF4372_RF8_MIN_HZ) ||
        (frequency_hz > ADF4372_VCO_MAX_HZ))
      return ESY_ERR_INVALID_ARG;
    while ((vco_hz < ADF4372_VCO_MIN_HZ) && (rf_divider < 6u)) {
      vco_hz <<= 1u;
      divisor <<= 1u;
      rf_divider++;
    }
  }
  if ((vco_hz < ADF4372_VCO_MIN_HZ) || (vco_hz > ADF4372_VCO_MAX_HZ))
    return ESY_ERR_INVALID_ARG;
  memset(plan, 0, sizeof(*plan));
  const uint64_t target_num = vco_hz * ctx->r_counter;
  const uint64_t integer = target_num / ctx->ref_clk_hz;
  if (integer < ADF4372_INT_MIN || integer > ADF4372_INT_MAX)
    return ESY_ERR_INVALID_ARG;
  plan->integer = (uint16_t)integer;
  rem = target_num % ctx->ref_clk_hz;
  scaled = rem * ADF4372_MODULUS1;
  plan->fract1 = (uint32_t)(scaled / ctx->ref_clk_hz);
  fract2 = (uint32_t)(scaled % ctx->ref_clk_hz);
  mod2 = (uint32_t)ctx->ref_clk_hz;
  while (mod2 > ADF4372_MODULUS2_MAX) {
    mod2 >>= 1u;
    fract2 >>= 1u;
  }
  {
    const uint32_t g = adf4372_gcd(fract2, mod2);
    if (g != 0u) {
      fract2 /= g;
      mod2 /= g;
    }
  }
  /* Reducing the secondary ratio can round it to one. Carry before encoding
   * MOD2's minimum value, including the FRAC1-to-INT boundary. */
  if (fract2 >= mod2) {
    fract2 = 0u;
    plan->fract1++;
    if (plan->fract1 == ADF4372_MODULUS1) {
      plan->fract1 = 0u;
      if (plan->integer == ADF4372_INT_MAX) return ESY_ERR_INVALID_ARG;
      plan->integer++;
    }
  }
  if (mod2 < 2u) mod2 = 2u;
  const uint64_t primary = (uint64_t)plan->fract1 * ctx->ref_clk_hz;
  const uint64_t residual = (primary % ADF4372_MODULUS1) * mod2 +
                             (uint64_t)fract2 * ctx->ref_clk_hz;
  actual_vco = ((uint64_t)plan->integer * ctx->ref_clk_hz +
                primary / ADF4372_MODULUS1 +
                residual / (ADF4372_MODULUS1 * mod2)) / ctx->r_counter;
  plan->actual_hz = (channel == ESY_ADF4372_CHANNEL_RF16)
                        ? actual_vco * 2u
                        : actual_vco / divisor;
  plan->fract2 = (uint16_t)fract2;
  plan->mod2 = (uint16_t)mod2;
  plan->rf_divider = rf_divider;
  return ESY_OK;
}

static esy_err_t adf4372_write_frequency_fields(adf4372_ctx_t *ctx,
                                                const adf4372_plan_t *plan,
                                                bool autocal,
                                                bool write_changed_only) {
  uint8_t values[ADF4372_FREQUENCY_FIELD_COUNT];
  uint32_t bleed;
  esy_err_t err;
  values[0] = (uint8_t)(plan->integer >> 8u);
  values[1] = autocal ? 0x40u : 0u;
  values[2] = 0u;
  values[3] = (uint8_t)plan->fract1;
  values[4] = (uint8_t)(plan->fract1 >> 8u);
  values[5] = (uint8_t)(plan->fract1 >> 16u);
  values[6] = (uint8_t)(((plan->fract2 & 0x7Fu) << 1u) | (plan->fract1 >> 24u));
  values[7] = (uint8_t)(plan->fract2 >> 7u);
  values[8] = (uint8_t)plan->mod2;
  values[9] = (uint8_t)(plan->mod2 >> 8u);
  for (uint8_t i = 0u; i < ADF4372_FREQUENCY_FIELD_COUNT; i++) {
    const uint8_t address = (uint8_t)(ADF4372_INT_H + i);
    err = write_changed_only ? adf4372_write_if_changed(ctx, address, values[i])
                             : adf4372_write(ctx, address, values[i]);
    if (err != ESY_OK) return err;
  }
  err = write_changed_only
            ? adf4372_write_if_changed(ctx, ADF4372_R_COUNTER, (uint8_t)(ctx->r_counter & 0x1Fu))
            : adf4372_write(ctx, ADF4372_R_COUNTER, (uint8_t)(ctx->r_counter & 0x1Fu));
  if (err != ESY_OK) return err;
  ctx->reg24 =
      (uint8_t)((ctx->reg24 & 0x8Fu) | ((plan->rf_divider & 0x07u) << 4u));
  err = write_changed_only
            ? adf4372_write_if_changed(ctx, ADF4372_RF_DIV, ctx->reg24)
            : adf4372_write(ctx, ADF4372_RF_DIV, ctx->reg24);
  if (err != ESY_OK) return err;
  bleed = ((400u * ctx->cp_ua) + ((uint32_t)plan->integer * 375u) - 1u) /
          ((uint32_t)plan->integer * 375u);
  if (bleed == 0u) {
    bleed = 1u;
  }
  if (bleed > 255u) {
    bleed = 255u;
  }
  err = write_changed_only
            ? adf4372_write_if_changed(ctx, ADF4372_CP_BLEED, (uint8_t)bleed)
            : adf4372_write(ctx, ADF4372_CP_BLEED, (uint8_t)bleed);
  if (err != ESY_OK) return err;
  const uint8_t int_mode = plan->fract2 != 0u ? 0x10u :
      (plan->fract1 == 0u ? 1u : 0u);
  return write_changed_only
             ? adf4372_write_if_changed(ctx, ADF4372_INT_MODE, int_mode)
             : adf4372_write(ctx, ADF4372_INT_MODE, int_mode);
}

static esy_err_t adf4372_trigger(adf4372_ctx_t *ctx, const adf4372_plan_t *plan,
                                 uint64_t requested_hz) {
  const esy_err_t err =
      adf4372_write(ctx, ADF4372_INT_L, (uint8_t)plan->integer);
  if (err != ESY_OK) return err;
  ctx->requested_hz = requested_hz;
  ctx->actual_hz = plan->actual_hz;
  ctx->integer = plan->integer;
  ctx->fract1 = plan->fract1;
  ctx->fract2 = plan->fract2;
  ctx->mod2 = plan->mod2;
  ctx->rf_divider = plan->rf_divider;
  return ESY_OK;
}

static esy_err_t adf4372_set_vco_fsm_test_mode(adf4372_ctx_t *ctx,
                                               uint8_t test_mode,
                                               bool write_changed_only) {
  const uint8_t value = (uint8_t)((ctx->reg34 & ADF4372_VCO_ALC_TIMEOUT_MASK) |
                                  (test_mode & ADF4372_VCO_FSM_TEST_MODE_MASK));
  const esy_err_t err =
      write_changed_only
          ? adf4372_write_if_changed(ctx, ADF4372_VCO_OVERRIDE, value)
          : adf4372_write(ctx, ADF4372_VCO_OVERRIDE, value);

  if (err == ESY_OK) {
    ctx->reg34 = value;
  }
  return err;
}

static esy_err_t adf4372_set_frequency(adf4372_ctx_t *ctx,
                                       uint64_t frequency_hz) {
  adf4372_plan_t plan;
  esy_err_t err = adf4372_build_plan(
      ctx, frequency_hz, (esy_adf4372_channel_t)ctx->main_channel, &plan);
  if (err != ESY_OK) return err;
  err = adf4372_write_frequency_fields(ctx, &plan, true, false);
  if (err != ESY_OK) return err;
  err = adf4372_set_vco_fsm_test_mode(ctx, 0u, false);
  return (err == ESY_OK) ? adf4372_trigger(ctx, &plan, frequency_hz) : err;
}

static esy_err_t adf4372_enable_channel(adf4372_ctx_t *ctx,
                                        esy_adf4372_channel_t channel,
                                        bool enable) {
  uint8_t *cache;
  uint8_t address;
  uint8_t bit;
  if (channel == ESY_ADF4372_CHANNEL_RF8) {
    cache = &ctx->reg25;
    address = ADF4372_RF8;
    bit = 2u;
  } else if (channel == ESY_ADF4372_CHANNEL_AUX_RF8) {
    cache = &ctx->reg72;
    address = ADF4372_AUX;
    bit = 3u;
  } else if (channel == ESY_ADF4372_CHANNEL_RF16) {
    cache = &ctx->reg25;
    address = ADF4372_RF8;
    bit = 3u;
  } else
    return ESY_ERR_INVALID_ARG;
  const uint8_t value = enable ? (uint8_t)(*cache | (1u << bit))
                               : (uint8_t)(*cache & ~(1u << bit));
  const esy_err_t err = adf4372_write(ctx, address, value);
  if (err == ESY_OK) *cache = value;
  return err;
}

static esy_err_t adf4372_set_frequency_hz(void *context,
                                          uint64_t frequency_hz) {
  return adf4372_set_frequency((adf4372_ctx_t *)context, frequency_hz);
}
ESY_INTERNAL esy_err_t esy_adf4372_set_output_enable_internal(void *context, esy_freq_synth_port_t port,
                                    bool enable) {
  adf4372_ctx_t *ctx = context;
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if (port == ESY_FREQ_SYNTH_PORT_MAIN) {
    const esy_err_t err = adf4372_enable_channel(
        ctx, (esy_adf4372_channel_t)ctx->main_channel, enable);
    if (err == ESY_OK) ctx->main_enabled = enable;
    return err;
  }
  if (port == ESY_FREQ_SYNTH_PORT_AUX) {
    const esy_err_t err =
        adf4372_enable_channel(ctx, ESY_ADF4372_CHANNEL_AUX_RF8, enable);
    if (err == ESY_OK) ctx->aux_enabled = enable;
    return err;
  }
  return ESY_ERR_NOT_SUPPORTED;
}
static esy_err_t adf4372_set_power(void *context, esy_freq_synth_port_t port,
                                   uint8_t level) {
  adf4372_ctx_t *ctx = context;
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if ((port != ESY_FREQ_SYNTH_PORT_MAIN) ||
      (ctx->main_channel != ESY_ADF4372_CHANNEL_RF8))
    return ESY_ERR_NOT_SUPPORTED;
  if (level > 3u) return ESY_ERR_INVALID_ARG;
  const uint8_t value = (uint8_t)((ctx->reg25 & 0xFCu) | level);
  const esy_err_t err = adf4372_write(ctx, ADF4372_RF8, value);
  if (err == ESY_OK) {
    ctx->reg25 = value;
    ctx->main_power_level = level;
  }
  return err;
}
static esy_err_t adf4372_get_info(void *context, esy_freq_synth_info_id_t id,
                                  esy_freq_synth_info_value_t *value) {
  adf4372_ctx_t *ctx = context;
  if ((ctx == NULL) || (value == NULL)) return ESY_ERR_INVALID_ARG;
  switch (id) {
    case ESY_FREQ_SYNTH_INFO_ACTUAL_HZ:
      value->u64 = ctx->actual_hz;
      return ESY_OK;
    case ESY_FREQ_SYNTH_INFO_REQUESTED_HZ:
      value->u64 = ctx->requested_hz;
      return ESY_OK;
    case ESY_FREQ_SYNTH_INFO_PFD_HZ:
      value->u64 = ctx->ref_clk_hz / ctx->r_counter;
      return ESY_OK;
    case ESY_FREQ_SYNTH_INFO_MOD:
      value->u16 = ctx->mod2;
      return ESY_OK;
    case ESY_FREQ_SYNTH_INFO_INT:
      value->u16 = ctx->integer;
      return ESY_OK;
    case ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED:
      value->u8 = ctx->main_enabled ? 1u : 0u;
      return ESY_OK;
    case ESY_FREQ_SYNTH_INFO_POWER_LEVEL:
      if (ctx->main_channel != ESY_ADF4372_CHANNEL_RF8)
        return ESY_ERR_NOT_SUPPORTED;
      value->u8 = ctx->main_power_level;
      return ESY_OK;
    case ESY_FREQ_SYNTH_INFO_RF_DIV_SEL:
      value->u8 = ctx->rf_divider;
      return ESY_OK;
    default:
      return ESY_ERR_NOT_SUPPORTED;
  }
}

static esy_err_t adf4372_program_defaults(adf4372_ctx_t *ctx,
                                          const esy_adf4372_config_t *config) {
  const adf4372_alc_profile_t *alc_profile;
  uint8_t device_revision;
  uint32_t timeout = 2u;
  uint32_t synth_timeout = 2u;
  uint32_t alc_timeout = 2u;
  uint32_t adc_clock_divider = 0u;
  const uint64_t pfd_hz = (ctx->ref_clk_hz + ctx->r_counter - 1u) / ctx->r_counter;
  const uint32_t band_divider = (uint32_t)((pfd_hz + 2399999u) / 2400000u);
  esy_err_t err = adf4372_write(ctx, ADF4372_RESET, 0x81u);
  if (err != ESY_OK) return err;
  err =
      adf4372_write(ctx, ADF4372_RESET, config->spi_three_wire ? 0x00u : 0x18u);
  if (err != ESY_OK) return err;
  err = adf4372_write(ctx, ADF4372_MUXOUT, config->muxout_3v3 ? 0x04u : 0x00u);
  if (err != ESY_OK) return err;
  err = adf4372_read_revision(ctx, &device_revision);
  if (err != ESY_OK) return err;
  alc_profile = adf4372_select_alc_profile(device_revision, config->vco_supply);
  for (uint8_t i = 0u;
       i < sizeof(k_default_registers) / sizeof(k_default_registers[0]); i++) {
    const uint8_t value =
        adf4372_apply_alc_profile(alc_profile, k_default_registers[i].address,
                                  k_default_registers[i].value);
    err = adf4372_write(ctx, k_default_registers[i].address, value);
    if (err != ESY_OK) return err;
  }
  err = adf4372_write(ctx, ADF4372_CP,
                      (uint8_t)((config->charge_pump_current << 4u) | 0x08u));
  if (err != ESY_OK) return err;
  err = adf4372_write(ctx, ADF4372_REFIN,
                      config->differential_ref_clk ? 0x40u : 0u);
  if (err != ESY_OK) return err;
  err = adf4372_write(ctx, ADF4372_MUXOUT,
                      (uint8_t)((config->muxout_mode << 4u) |
                                (config->muxout_enabled ? 0x08u : 0u) |
                                (config->muxout_3v3 ? 0x04u : 0u)));
  if (err != ESY_OK) return err;
  ctx->reg25 = (uint8_t)(config->rf8_power_level |
                         (config->mute_till_lock ? 0x80u : 0u));
  ctx->reg72 = config->auxiliary_uses_vco ? 0x72u : 0x32u;
  ctx->reg24 = 0x80u;
  err = adf4372_write(ctx, ADF4372_RF8, ctx->reg25);
  if (err != ESY_OK) return err;
  err = adf4372_write(ctx, ADF4372_AUX, ctx->reg72);
  if (err != ESY_OK) return err;
  while (((uint64_t)synth_timeout * 1024u + timeout) <= (20u * pfd_hz + 999999u) / 1000000u) {
    timeout++;
    if (timeout > 1023u) {
      timeout = 2u;
      synth_timeout++;
    }
  }
  while (((uint64_t)alc_timeout * 1024u) + timeout <
         (50u * pfd_hz + 999999u) / 1000000u)
    alc_timeout++;
  err = adf4372_write(ctx, 0x30u, (uint8_t)band_divider);
  if (err != ESY_OK) return err;
  err = adf4372_write(ctx, 0x31u, (uint8_t)timeout);
  if (err != ESY_OK) return err;
  err = adf4372_write(ctx, 0x32u, (uint8_t)(((timeout >> 8u) & 0x03u) | 0x04u));
  if (err != ESY_OK) return err;
  err = adf4372_write(ctx, 0x33u, (uint8_t)synth_timeout);
  if (err != ESY_OK) return err;
  ctx->reg34 = (uint8_t)(alc_timeout & ADF4372_VCO_ALC_TIMEOUT_MASK);
  err = adf4372_write(ctx, ADF4372_VCO_OVERRIDE, ctx->reg34);
  if (err != ESY_OK) return err;
  if (pfd_hz > 200000u) {
    adc_clock_divider = (uint32_t)((((pfd_hz / 100000u) - 2u) + 3u) / 4u);
    if (adc_clock_divider > 255u) adc_clock_divider = 255u;
  }
  return adf4372_write(ctx, 0x35u, (uint8_t)adc_clock_divider);
}

void esy_adf4372_default_config(esy_adf4372_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->cs_gpio = NULL;
  config->ce_gpio = NULL;
  config->ref_clk_hz = 100000000u;
  config->r_counter = 1u;
  config->vco_supply = ESY_ADF4372_VCO_SUPPLY_5V;
  config->initial_freq_hz = 5000000000ULL;
  config->main_channel = ESY_ADF4372_CHANNEL_RF8;
  config->charge_pump_current = 4u;
  config->charge_pump_current_ua = 1750u;
  config->muxout_mode = 1u;
  config->muxout_enabled = false;
  config->rf8_power_level = 3u;
}


static const esy_freq_synth_vtable_t k_adf4372_ops = {
    .set_frequency_hz = adf4372_set_frequency_hz,
    .set_output_enable = esy_adf4372_set_output_enable_internal,
    .set_power_level = adf4372_set_power,
    .get_info = adf4372_get_info,
};

ESY_INTERNAL adf4372_ctx_t *esy_adf4372_get_context_internal(
    esy_freq_synth_t *synth) {
  if (synth == NULL || synth->ctx == NULL ||
      synth->ops != &k_adf4372_ops) return NULL;
  return synth->ctx;
}

esy_err_t esy_adf4372_init(esy_freq_synth_t *synth,
                           const esy_adf4372_config_t *config) {
  adf4372_ctx_t *ctx;
  esy_err_t err;
  if ((synth == NULL) || (config == NULL) || (config->spi == NULL) ||
      (config->cs_gpio == NULL) || !esy_hal_gpio_is_initialized(config->cs_gpio) ||
      ((config->ce_gpio != NULL) && !esy_hal_gpio_is_initialized(config->ce_gpio)) ||
      !adf4372_is_main_channel(config->main_channel) ||
      (config->r_counter == 0u) || (config->r_counter > 32u) ||
      ((config->vco_supply != ESY_ADF4372_VCO_SUPPLY_3V3) &&
       (config->vco_supply != ESY_ADF4372_VCO_SUPPLY_5V)) ||
      (config->ref_clk_hz < ADF4372_REFERENCE_MIN_HZ) ||
      (config->ref_clk_hz > (config->differential_ref_clk ?
       ADF4372_REFERENCE_DIFF_MAX_HZ : ADF4372_REFERENCE_SINGLE_MAX_HZ)) ||
      ((uint64_t)config->ref_clk_hz >
       ADF4372_PFD_MAX_HZ * config->r_counter) ||
      (!config->spi_three_wire && config->muxout_enabled) ||
      (config->charge_pump_current > 15u) ||
      (config->charge_pump_current_ua == 0u) || (config->muxout_mode > 15u) ||
      (config->rf8_power_level > 3u) ||
      ((config->main_channel == ESY_ADF4372_CHANNEL_RF8) &&
       ((config->initial_freq_hz < ADF4372_RF8_MIN_HZ) ||
        (config->initial_freq_hz > ADF4372_VCO_MAX_HZ))) ||
      ((config->main_channel == ESY_ADF4372_CHANNEL_RF16) &&
       ((config->initial_freq_hz < ADF4372_RF16_MIN_HZ) ||
        (config->initial_freq_hz > ADF4372_RF16_MAX_HZ)))) {
    return ESY_ERR_INVALID_ARG;
  }
  if (synth->ops != NULL || synth->ctx != NULL ||
      esy_freq_synth_runtime(synth)->device != NULL ||
      esy_freq_synth_runtime(synth)->faulted)
    return ESY_ERR_STATE;
  memset(synth, 0, sizeof(*synth));
  ctx = (adf4372_ctx_t *)synth->backend.bytes;
  ctx->spi = config->spi;
  ctx->ref_clk_hz = config->ref_clk_hz;
  ctx->cs_high_delay_us = config->cs_high_delay_us;
  ctx->cp_ua = config->charge_pump_current_ua;
  ctx->cs_gpio = config->cs_gpio;
  ctx->ce_gpio = config->ce_gpio;
  ctx->r_counter = config->r_counter;
  ctx->main_channel = config->main_channel;
  ctx->main_power_level = config->rf8_power_level;
  ctx->aux_vco = config->auxiliary_uses_vco;
  ctx->spi_three_wire = config->spi_three_wire;
  synth->ctx = ctx;
  esy_freq_synth_runtime(synth)->faulted = true;
  synth->ops = &k_adf4372_ops;
  if (esy_hal_gpio_write(ctx->cs_gpio, true) != ESY_OK) {
    (void)esy_adf4372_deinit(synth);
    return ESY_ERR_FAIL;
  }
  if ((ctx->ce_gpio != NULL) &&
      (esy_hal_gpio_write(ctx->ce_gpio, true) != ESY_OK)) {
    (void)esy_adf4372_deinit(synth);
    return ESY_ERR_FAIL;
  }
  if (config->spi_speed_hz != 0u)
    (void)esy_hal_spi_set_speed(ctx->spi, config->spi_speed_hz);
  err = adf4372_program_defaults(ctx, config);
  if (err == ESY_OK) err = adf4372_set_frequency(ctx, config->initial_freq_hz);
  if ((err == ESY_OK) && config->initial_output_enabled)
    err = esy_adf4372_set_output_enable_internal(ctx, ESY_FREQ_SYNTH_PORT_MAIN, true);
  if (err != ESY_OK) {
    (void)esy_adf4372_deinit(synth);
  } else {
    esy_freq_synth_runtime(synth)->faulted = false;
  }
  return err;
}

esy_err_t esy_adf4372_deinit(esy_freq_synth_t *synth) {
  if (synth == NULL) return ESY_ERR_INVALID_ARG;
  if (esy_freq_synth_runtime(synth)->device != NULL) return ESY_ERR_BUSY;
  if (synth->ctx == NULL) return ESY_OK;
  if (synth->ops != &k_adf4372_ops)
    return ESY_ERR_STATE;
  adf4372_ctx_t *ctx = synth->ctx;
  esy_err_t ret = esy_adf4372_set_output_enable_internal(ctx, ESY_FREQ_SYNTH_PORT_MAIN, false);
  esy_err_t err = esy_adf4372_set_output_enable_internal(ctx, ESY_FREQ_SYNTH_PORT_AUX, false);
  if (ret == ESY_OK) ret = err;
  if (ctx->ce_gpio != NULL) {
    err = esy_hal_gpio_write(ctx->ce_gpio, false);
    if (ret == ESY_OK) ret = err;
  }
  err = esy_hal_gpio_write(ctx->cs_gpio, true);
  if (ret == ESY_OK) ret = err;
  esy_freq_synth_runtime(synth)->faulted = ret != ESY_OK;
  if (ret == ESY_OK) esy_freq_synth_detach(synth);
  return ret;
}

esy_err_t esy_adf4372_read_vco_calibration(
    esy_freq_synth_t *synth, esy_adf4372_vco_calibration_t *calibration) {
  uint8_t fsm, value;
  if ((synth == NULL) || (calibration == NULL)) return ESY_ERR_INVALID_ARG;
  adf4372_ctx_t *ctx = synth->ctx;
  if (ctx == NULL || esy_freq_synth_runtime(synth)->faulted ||
      synth->ops != &k_adf4372_ops)
    return ESY_ERR_STATE;
  esy_err_t err = esy_freq_synth_runtime(synth)->device != NULL ? esy_device_get(esy_freq_synth_runtime(synth)->device) : ESY_OK;
  if (err != ESY_OK) return err;
  esy_adf4372_vco_calibration_t result = {0};
  err = adf4372_read(ctx, ADF4372_VCO_FSM, &fsm);
  if (err != ESY_OK) goto release;
  err = adf4372_write(ctx, ADF4372_VCO_FSM, (uint8_t)((fsm & 0x1Fu) | 0x20u));
  if (err != ESY_OK) goto restore;
  err = adf4372_read(ctx, ADF4372_VCO_CORE, &value);
  if (err != ESY_OK) goto restore;
  result.vco_core = value & 0x0Fu;
  err = adf4372_read(ctx, ADF4372_VCO_DATA, &value);
  if (err != ESY_OK) goto restore;
  result.vco_band = value;
  err = adf4372_write(ctx, ADF4372_VCO_FSM, (uint8_t)((fsm & 0x1Fu) | 0x60u));
  if (err != ESY_OK) goto restore;
  err = adf4372_read(ctx, ADF4372_VCO_DATA, &value);
  if (err != ESY_OK) goto restore;
  result.vco_bias = value & 0x0Fu;
restore: {
  const esy_err_t restore_err = adf4372_write(ctx, ADF4372_VCO_FSM, fsm);
  if (restore_err != ESY_OK) {
    esy_freq_synth_runtime(synth)->faulted = true;
    err = ESY_ERR_STATE;
  }
}
  if (err == ESY_OK) {
    result.frequency_hz = ctx->requested_hz;
    result.channel = (esy_adf4372_channel_t)ctx->main_channel;
    *calibration = result;
  }
release:
  if (esy_freq_synth_runtime(synth)->device != NULL) (void)esy_device_put(esy_freq_synth_runtime(synth)->device);
  return err;
}

esy_err_t esy_adf4372_set_frequency_fast(
    esy_freq_synth_t *synth, uint64_t frequency_hz,
    const esy_adf4372_vco_calibration_t *calibration) {
  adf4372_ctx_t *ctx;
  if ((synth == NULL) || (calibration == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  ctx = synth->ctx;
  if ((ctx == NULL) || esy_freq_synth_runtime(synth)->faulted ||
      (synth->ops != &k_adf4372_ops)) {
    return ESY_ERR_STATE;
  }
  if (calibration->channel != (esy_adf4372_channel_t)ctx->main_channel ||
      (calibration->vco_core != 1u && calibration->vco_core != 2u &&
       calibration->vco_core != 4u && calibration->vco_core != 8u) ||
      calibration->vco_bias > 15u) {
    return ESY_ERR_INVALID_ARG;
  }
  adf4372_plan_t plan;
  esy_err_t err =
      adf4372_build_plan(ctx, frequency_hz, calibration->channel, &plan);
  if (err != ESY_OK) return err;
  err = esy_freq_synth_runtime(synth)->device != NULL ? esy_device_get(esy_freq_synth_runtime(synth)->device) : ESY_OK;
  if (err != ESY_OK) return err;
  err = adf4372_write_frequency_fields(ctx, &plan, false, true);
  if (err != ESY_OK) goto release;
  err = adf4372_set_vco_fsm_test_mode(
      ctx, ADF4372_VCO_FSM_TEST_MODE_MANUAL_OVERRIDE, true);
  if (err != ESY_OK) goto release;
  err = adf4372_write_if_changed(
      ctx, ADF4372_VCO_CORE_BIAS,
      (uint8_t)((calibration->vco_core << 4u) |
                (calibration->vco_bias & 0x0Fu)));
  if (err != ESY_OK) goto release;
  err = adf4372_write_if_changed(ctx, ADF4372_VCO_BAND,
                                 calibration->vco_band);
  if (err != ESY_OK) goto release;
  err = adf4372_trigger(ctx, &plan, frequency_hz);
release:
  if (esy_freq_synth_runtime(synth)->device != NULL) (void)esy_device_put(esy_freq_synth_runtime(synth)->device);
  return err;
}
