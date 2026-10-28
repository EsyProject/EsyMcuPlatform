/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_wifi.h"

#include "esy_hal_backend/esy_hb_wifi.h"

void esy_hal_wifi_default_cfg(esy_wifi_config_t *config) {
  esy_hb_wifi_default_cfg(config);
}

esy_err_t esy_hal_wifi_init_sta(const esy_wifi_config_t *config) {
  return esy_hb_wifi_init_sta(config);
}

esy_err_t esy_hal_wifi_start_sta(
    const esy_wifi_config_t *config) {
  return esy_hb_wifi_start_sta(config);
}

esy_err_t esy_hal_wifi_process_sta(void) {
  return esy_hb_wifi_process_sta();
}

bool esy_hal_wifi_is_connected(void) {
  return esy_hb_wifi_is_connected();
}

esy_err_t esy_hal_wifi_get_ipv4_addr(esy_wifi_ipv4_addr_t *out_addr) {
  return esy_hb_wifi_get_ipv4_addr(out_addr);
}

esy_err_t esy_hal_wifi_get_network_runtime(
    esy_wifi_network_runtime_t *out_runtime) {
  if (out_runtime == NULL) return ESY_ERR_INVALID_ARG;
  esy_wifi_network_runtime_t runtime;
  esy_err_t ret = esy_hb_wifi_get_network_runtime(&runtime);
  if (ret == ESY_OK) *out_runtime = runtime;
  return ret;
}

void esy_hal_wifi_deinit(void) { esy_hb_wifi_deinit(); }
