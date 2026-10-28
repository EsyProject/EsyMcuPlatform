/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_wifi.h"

#include <string.h>

typedef struct {
  fake_esy_hal_wifi_state_t state;
} fake_esy_hal_wifi_ctx_t;
static fake_esy_hal_wifi_ctx_t s_ctx;

static void network_progress(void *context) {
  s_ctx.state.progress_calls++;
  s_ctx.state.last_context = context;
}

static void network_enter(void *context) {
  s_ctx.state.enter_calls++;
  s_ctx.state.last_context = context;
}

static void network_leave(void *context) {
  s_ctx.state.leave_calls++;
  s_ctx.state.last_context = context;
}

void fake_esy_hal_wifi_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.state.address.octets[0] = 192u;
  s_ctx.state.address.octets[1] = 0u;
  s_ctx.state.address.octets[2] = 2u;
  s_ctx.state.address.octets[3] = 1u;
  s_ctx.state.network_runtime = (esy_wifi_network_runtime_t){
      .progress = network_progress,
      .enter = network_enter,
      .leave = network_leave,
      .context = NULL,
  };
}

fake_esy_hal_wifi_state_t *fake_esy_hal_wifi_state(void) {
  return &s_ctx.state;
}

void esy_hal_wifi_default_cfg(esy_wifi_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->auth = ESY_WIFI_AUTH_WPA2_AES_PSK;
  config->connect_timeout_ms = 30000u;
}

esy_err_t esy_hal_wifi_start_sta(const esy_wifi_config_t *config) {
  if (config == NULL) return ESY_ERR_INVALID_ARG;
  s_ctx.state.start_calls++;
  s_ctx.state.last_config = *config;
  return s_ctx.state.start_result;
}

esy_err_t esy_hal_wifi_process_sta(void) {
  s_ctx.state.process_calls++;
  if (s_ctx.state.process_result == ESY_OK) s_ctx.state.connected = true;
  return s_ctx.state.process_result;
}

esy_err_t esy_hal_wifi_init_sta(const esy_wifi_config_t *config) {
  esy_err_t ret = esy_hal_wifi_start_sta(config);
  return (ret == ESY_OK) ? esy_hal_wifi_process_sta() : ret;
}

bool esy_hal_wifi_is_connected(void) { return s_ctx.state.connected; }

esy_err_t esy_hal_wifi_get_ipv4_addr(esy_wifi_ipv4_addr_t *out_addr) {
  if (out_addr == NULL) return ESY_ERR_INVALID_ARG;
  s_ctx.state.address_calls++;
  if (s_ctx.state.address_result == ESY_OK) *out_addr = s_ctx.state.address;
  return s_ctx.state.address_result;
}

esy_err_t esy_hal_wifi_get_network_runtime(
    esy_wifi_network_runtime_t *out_runtime) {
  if (out_runtime == NULL) return ESY_ERR_INVALID_ARG;
  s_ctx.state.runtime_calls++;
  if (s_ctx.state.runtime_result == ESY_OK) {
    *out_runtime = s_ctx.state.network_runtime;
  }
  return s_ctx.state.runtime_result;
}

void esy_hal_wifi_deinit(void) {
  s_ctx.state.deinit_calls++;
  s_ctx.state.connected = false;
}
