/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_low_power.h"

#include "esy_hb_internal.h"

static bool s_prepared;

esy_err_t esy_hb_low_power_get_capabilities(
    esy_hb_low_power_capabilities_t *out_capabilities) {
  if (out_capabilities == NULL) return ESY_ERR_INVALID_ARG;
  *out_capabilities = (esy_hb_low_power_capabilities_t){
      .supported_mode_mask =
          ESY_HB_LOW_POWER_MODE_MASK(ESY_HB_LOW_POWER_MODE_SLEEP),
      .entry_latency_us = {1u, 0u},
      .exit_latency_us = {1u, 0u},
      .min_residency_us = {0u, 0u},
  };
  return ESY_OK;
}

esy_err_t esy_hb_low_power_prepare(esy_hb_low_power_mode_t mode) {
  if (mode != ESY_HB_LOW_POWER_MODE_SLEEP) {
    return ((uint32_t)mode < (uint32_t)ESY_HB_LOW_POWER_MODE_COUNT)
               ? ESY_ERR_NOT_SUPPORTED
               : ESY_ERR_INVALID_ARG;
  }
  if (s_prepared) return ESY_ERR_STATE;
  s_prepared = true;
  return ESY_OK;
}

esy_err_t esy_hb_low_power_enter(
    esy_hb_low_power_mode_t mode,
    esy_hb_low_power_result_t *out_result) {
  if (out_result == NULL) return ESY_ERR_INVALID_ARG;
  *out_result = (esy_hb_low_power_result_t){0};
  if (mode != ESY_HB_LOW_POWER_MODE_SLEEP) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  if (!s_prepared) return ESY_ERR_STATE;
  esy_cpu_wfi();
  out_result->entered = true;
  s_prepared = false;
  return ESY_OK;
}

esy_err_t esy_hb_low_power_cancel(esy_hb_low_power_mode_t mode) {
  if (mode != ESY_HB_LOW_POWER_MODE_SLEEP) {
    return ((uint32_t)mode < (uint32_t)ESY_HB_LOW_POWER_MODE_COUNT)
               ? ESY_ERR_NOT_SUPPORTED
               : ESY_ERR_INVALID_ARG;
  }
  if (!s_prepared) return ESY_ERR_STATE;
  s_prepared = false;
  return ESY_OK;
}

esy_err_t esy_hb_shutdown(esy_hb_shutdown_mode_t mode) {
  return ((uint32_t)mode < (uint32_t)ESY_HB_SHUTDOWN_MODE_COUNT)
             ? ESY_ERR_NOT_SUPPORTED
             : ESY_ERR_INVALID_ARG;
}
