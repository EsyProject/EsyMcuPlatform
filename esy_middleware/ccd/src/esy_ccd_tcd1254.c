/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/ccd/esy_ccd_tcd1254.h"

#include <string.h>

#define TCD1254_NS_PER_SECOND UINT64_C(1000000000)
#define TCD1254_MASTER_MIN_HZ UINT64_C(400000)
#define TCD1254_MASTER_MAX_HZ UINT64_C(4000000)
#define TCD1254_PULSE_MIN_NS 1000u
#define TCD1254_T2_MIN_NS 100u
#define TCD1254_SH_PERIOD_MIN_NS 10000u
#define TCD1254_MASTER_CYCLES_PER_SAMPLE 2u

static uint64_t ticks_from_ns(uint32_t ns, uint32_t hz) {
  /* UINT32_MAX squared plus the rounding term still fits uint64_t. */
  return ((uint64_t)ns * hz + TCD1254_NS_PER_SECOND - 1u) /
         TCD1254_NS_PER_SECOND;
}

static uint64_t round_to_period(uint64_t ticks, uint32_t period) {
  return ((ticks + period - 1u) / period) * period;
}

esy_err_t esy_ccd_tcd1254_plan_compile(const esy_ccd_tcd1254_config_t *config,
                                         esy_ccd_tcd1254_plan_t *plan) {
  if (config == NULL || plan == NULL || config->tick_hz == 0u ||
      config->master_period_ticks < 2u ||
      (config->master_period_ticks & 1u) != 0u ||
      config->tick_hz < TCD1254_MASTER_MIN_HZ * config->master_period_ticks ||
      config->tick_hz > TCD1254_MASTER_MAX_HZ * config->master_period_ticks ||
      config->icg_delay_ns < TCD1254_PULSE_MIN_NS ||
      config->icg_to_sh_ns < TCD1254_T2_MIN_NS ||
      config->icg_to_sh_ns > TCD1254_PULSE_MIN_NS ||
      config->sh_high_ns < TCD1254_PULSE_MIN_NS ||
      config->sh_period_ns < TCD1254_SH_PERIOD_MIN_NS ||
      config->integration_ns <= TCD1254_SH_PERIOD_MIN_NS) {
    return ESY_ERR_INVALID_ARG;
  }

  const uint32_t period = config->master_period_ticks;
  const uint64_t sh_rise = ticks_from_ns(config->icg_to_sh_ns, config->tick_hz);
  const uint64_t sh_high = ticks_from_ns(config->sh_high_ns, config->tick_hz);
  const uint64_t delay = ticks_from_ns(config->icg_delay_ns, config->tick_hz);
  const uint64_t icg_rise = round_to_period(sh_rise + sh_high + delay, period);
  const uint64_t sh_period = round_to_period(
      ticks_from_ns(config->sh_period_ns, config->tick_hz), period);
  const uint64_t integration = round_to_period(
      ticks_from_ns(config->integration_ns, config->tick_hz), period);
  const uint64_t readout = (uint64_t)ESY_CCD_TCD1254_LINE_SAMPLES *
                          TCD1254_MASTER_CYCLES_PER_SAMPLE * period;
  const uint64_t minimum_frame = icg_rise + readout;
  const uint64_t purge_pulses = integration >= minimum_frame ? 0u :
      (minimum_frame - integration + sh_period - 1u) / sh_period;
  const uint64_t frame_period = purge_pulses * sh_period + integration;

  if (sh_rise * TCD1254_NS_PER_SECOND >
          (uint64_t)config->tick_hz * TCD1254_PULSE_MIN_NS ||
      sh_period <= sh_high || integration < sh_rise + sh_high ||
      (purge_pulses != 0u && sh_rise + sh_period < icg_rise) ||
      sh_rise > UINT32_MAX || sh_high > UINT32_MAX ||
      icg_rise > UINT32_MAX || sh_period > UINT32_MAX ||
      integration > UINT32_MAX || readout > UINT32_MAX ||
      frame_period > UINT32_MAX || purge_pulses > UINT32_MAX) {
    return ESY_ERR_NOT_SUPPORTED;
  }

  const esy_ccd_tcd1254_plan_t compiled = {
      .tick_hz = config->tick_hz,
      .master_period_ticks = period,
      .sh_rise_ticks = (uint32_t)sh_rise,
      .sh_high_ticks = (uint32_t)sh_high,
      .icg_rise_ticks = (uint32_t)icg_rise,
      .sh_period_ticks = (uint32_t)sh_period,
      .integration_ticks = (uint32_t)integration,
      .purge_pulses = (uint32_t)purge_pulses,
      .readout_ticks = (uint32_t)readout,
      .frame_period_ticks = (uint32_t)frame_period,
  };
  *plan = compiled;
  return ESY_OK;
}

esy_err_t esy_ccd_tcd1254_init(esy_ccd_tcd1254_t *sensor,
                                 const esy_ccd_tcd1254_config_t *config) {
  if (sensor == NULL || config == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->initialized) {
    return ESY_ERR_STATE;
  }
  esy_ccd_tcd1254_plan_t plan;
  const esy_err_t result = esy_ccd_tcd1254_plan_compile(config, &plan);
  if (result != ESY_OK) {
    return result;
  }
  sensor->plan = plan;
  sensor->startup_readouts = 0u;
  sensor->pending = false;
  sensor->initialized = true;
  return ESY_OK;
}

esy_err_t esy_ccd_tcd1254_begin(esy_ccd_tcd1254_t *sensor,
                                  esy_ccd_tcd1254_readout_t *readout) {
  if (sensor == NULL || readout == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!sensor->initialized) {
    return ESY_ERR_STATE;
  }
  if (sensor->pending) {
    return ESY_ERR_BUSY;
  }
  if (sensor->sequence == UINT64_MAX) {
    return ESY_ERR_STATE;
  }
  sensor->sequence++;
  sensor->pending = true;
  const esy_ccd_tcd1254_readout_t next = {
      .token = sensor->sequence,
      .discard = sensor->startup_readouts < ESY_CCD_TCD1254_STARTUP_READOUTS,
      .plan = sensor->plan,
  };
  *readout = next;
  return ESY_OK;
}

esy_err_t esy_ccd_tcd1254_complete(esy_ccd_tcd1254_t *sensor, uint64_t token,
                                     uint32_t sample_count, bool *usable) {
  if (sensor == NULL || usable == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!sensor->initialized || !sensor->pending || token != sensor->sequence ||
      token == 0u) {
    return ESY_ERR_STATE;
  }
  sensor->pending = false;
  if (sample_count != ESY_CCD_TCD1254_LINE_SAMPLES) {
    sensor->startup_readouts = 0u;
    return ESY_ERR_IO;
  }
  const bool ready =
      sensor->startup_readouts == ESY_CCD_TCD1254_STARTUP_READOUTS;
  if (!ready) {
    sensor->startup_readouts++;
  }
  *usable = ready;
  return ESY_OK;
}

esy_err_t esy_ccd_tcd1254_cancel(esy_ccd_tcd1254_t *sensor, uint64_t token) {
  if (sensor == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!sensor->initialized || !sensor->pending || token != sensor->sequence ||
      token == 0u) {
    return ESY_ERR_STATE;
  }
  sensor->pending = false;
  sensor->startup_readouts = 0u;
  return ESY_OK;
}

esy_err_t esy_ccd_tcd1254_deinit(esy_ccd_tcd1254_t *sensor) {
  if (sensor == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->pending) {
    return ESY_ERR_BUSY;
  }
  const uint64_t sequence = sensor->sequence;
  memset(sensor, 0, sizeof(*sensor));
  sensor->sequence = sequence;
  return ESY_OK;
}
