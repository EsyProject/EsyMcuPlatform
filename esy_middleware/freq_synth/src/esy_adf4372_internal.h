/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ADF4372_INTERNAL_H
#define ESY_ADF4372_INTERNAL_H

#include "esy_middleware/freq_synth/esy_adf4372.h"
#include "esy_common/device/esy_device.h"

#define ADF4372_FAST_SHADOW_COUNT 17u

typedef struct {
  esy_spi_t *spi;
  uint64_t ref_clk_hz;
  uint64_t requested_hz;
  uint64_t actual_hz;
  uint32_t cs_high_delay_us;
  uint32_t fract1;
  uint16_t integer;
  uint16_t fract2;
  uint16_t mod2;
  uint16_t cp_ua;
  esy_gpio_t *cs_gpio;
  esy_gpio_t *ce_gpio;
  uint8_t r_counter;
  uint8_t main_channel;
  uint8_t rf_divider;
  uint8_t main_power_level;
  uint8_t reg24;
  uint8_t reg25;
  uint8_t reg34;
  uint8_t reg72;
  uint8_t fast_register_shadow[ADF4372_FAST_SHADOW_COUNT];
  uint32_t fast_register_shadow_valid_mask;
  bool main_enabled;
  bool aux_enabled;
  bool aux_vco;
  bool spi_three_wire;
  bool resume_main_enabled;
  bool resume_aux_enabled;
  bool suspended;
  const esy_device_t *pm_requester;
} adf4372_ctx_t;

/** @brief Borrow context only for this driver's immutable operations table. */
ESY_INTERNAL adf4372_ctx_t *esy_adf4372_get_context_internal(
    esy_freq_synth_t *synth);

/** @brief Driver output operation shared only with its device PM adapter. */
ESY_INTERNAL esy_err_t esy_adf4372_set_output_enable_internal(
    void *context, esy_freq_synth_port_t port, bool enable);

#endif /* ESY_ADF4372_INTERNAL_H */
