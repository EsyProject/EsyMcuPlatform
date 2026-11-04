/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_low_power.h"

/* Compile/link qualification only: app_main never executes this probe. */
void esy_native_probe_power(void) {
  const esy_power_sleep_config_t config = {
      .state = ESY_POWER_STATE_RETENTION, .duration_us = 1000u,
  };
  esy_power_sleep_result_t result;
  esy_power_capabilities_t capabilities;
  (void)esy_hal_power_get_capabilities(&capabilities);
  (void)esy_hal_power_sleep(&config, &result);
  (void)esy_hal_shutdown(ESY_HAL_SHUTDOWN_MODE_STANDBY);
}
