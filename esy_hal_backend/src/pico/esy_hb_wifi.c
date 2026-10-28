/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_wifi.h"
#include "esy_hb_resource_assembly_internal.h"

#include <string.h>

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

typedef struct {
  esy_wifi_config_t active_config;
  absolute_time_t connect_deadline;
  uint32_t backend_auth;
  bool initialized;
  bool connected;
  bool connect_active;
} esy_hb_wifi_ctx_t;

static esy_hb_wifi_ctx_t s_ctx;

static void network_progress(void *context) {
  (void)context;
  if (!s_ctx.initialized) return;
  cyw43_arch_poll();
}

static void network_enter(void *context) {
  (void)context;
  if (!s_ctx.initialized) return;
  cyw43_arch_lwip_begin();
}

static void network_leave(void *context) {
  (void)context;
  if (!s_ctx.initialized) return;
  cyw43_arch_lwip_end();
}

esy_err_t esy_hb_wifi_get_network_runtime(
    esy_wifi_network_runtime_t *out_runtime) {
  if (out_runtime == NULL) return ESY_ERR_INVALID_ARG;
  *out_runtime = (esy_wifi_network_runtime_t){
      .progress = network_progress,
      .enter = network_enter,
      .leave = network_leave,
      .context = NULL,
  };
  return ESY_OK;
}

static bool is_sta_link_up(void) {
  int status;

  if (!s_ctx.initialized) {
    s_ctx.connected = false;
    return false;
  }

  cyw43_arch_poll();
  status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
  s_ctx.connected = (status == CYW43_LINK_UP);
  return s_ctx.connected;
}

static esy_err_t map_auth(esy_wifi_auth_t auth, uint32_t *out_auth) {
  if (out_auth == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  switch (auth) {
    case ESY_WIFI_AUTH_OPEN:
      *out_auth = CYW43_AUTH_OPEN;
      return ESY_OK;
    case ESY_WIFI_AUTH_WPA2_AES_PSK:
      *out_auth = CYW43_AUTH_WPA2_AES_PSK;
      return ESY_OK;
    case ESY_WIFI_AUTH_WPA2_MIXED_PSK:
      *out_auth = CYW43_AUTH_WPA2_MIXED_PSK;
      return ESY_OK;
    default:
      return ESY_ERR_NOT_SUPPORTED;
  }
}

static esy_err_t validate_sta_config(const esy_wifi_config_t *config,
                                     uint32_t *out_backend_auth) {
  if ((config == NULL) || (config->ssid == NULL) || (config->ssid[0] == '\0') ||
      (config->password == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  return map_auth(config->auth, out_backend_auth);
}

static esy_err_t start_sta_connect_attempt(void) {
  int status;
  int ret;

  ret = cyw43_arch_wifi_connect_async(s_ctx.active_config.ssid,
                                      s_ctx.active_config.password,
                                      s_ctx.backend_auth);
  if (ret != 0) {
    return ESY_ERR_IO;
  }

  status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
  s_ctx.connected = (status == CYW43_LINK_UP);
  return ESY_OK;
}

esy_err_t esy_hb_wifi_start_sta(const esy_wifi_config_t *config) {
  uint32_t backend_auth;
  esy_err_t ret;

  ret = validate_sta_config(config, &backend_auth);
  if (ret != ESY_OK) {
    return ret;
  }

  /* CYW43 allocates its PIO/DMA lazily during station enable/connect,
   * rather than only in arch_init. Keep the complete native start protected. */
  if (!esy_hb_resource_assembly_try_acquire()) return ESY_ERR_BUSY;
  if (!s_ctx.initialized) {
    if (cyw43_arch_init() != 0) {
      esy_hb_resource_assembly_release();
      return ESY_ERR_IO;
    }
    s_ctx.initialized = true;
  }

  cyw43_arch_enable_sta_mode();

  s_ctx.active_config = *config;
  s_ctx.backend_auth = backend_auth;
  s_ctx.connect_deadline = make_timeout_time_ms(config->connect_timeout_ms);
  s_ctx.connected = false;
  s_ctx.connect_active = false;

  ret = start_sta_connect_attempt();
  esy_hb_resource_assembly_release();
  if (ret != ESY_OK) {
    return ret;
  }

  s_ctx.connect_active = !s_ctx.connected;
  return ESY_OK;
}

esy_err_t esy_hb_wifi_process_sta(void) {
  int status;
  int ret;

  if (!s_ctx.initialized) {
    return ESY_ERR_STATE;
  }

  cyw43_arch_poll();
  status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
  if (status == CYW43_LINK_UP) {
    s_ctx.connected = true;
    s_ctx.connect_active = false;
    return ESY_OK;
  }

  s_ctx.connected = false;
  if (!s_ctx.connect_active) {
    return ESY_ERR_STATE;
  }

  if (status == CYW43_LINK_BADAUTH) {
    s_ctx.connect_active = false;
    return ESY_ERR_INVALID_ARG;
  }
  if (status == CYW43_LINK_FAIL) {
    s_ctx.connect_active = false;
    return ESY_ERR_IO;
  }
  if (time_reached(s_ctx.connect_deadline)) {
    s_ctx.connect_active = false;
    return ESY_ERR_TIMEOUT;
  }
  if (status == CYW43_LINK_NONET) {
    if (!esy_hb_resource_assembly_try_acquire()) return ESY_ERR_BUSY;
    ret = start_sta_connect_attempt();
    esy_hb_resource_assembly_release();
    if (ret != ESY_OK) {
      s_ctx.connect_active = false;
      return ret;
    }
  }

  cyw43_arch_poll();
  return ESY_ERR_BUSY;
}

void esy_hb_wifi_default_cfg(esy_wifi_config_t *config) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->auth = ESY_WIFI_AUTH_WPA2_AES_PSK;
  config->connect_timeout_ms = 30000u;
}

esy_err_t esy_hb_wifi_init_sta(const esy_wifi_config_t *config) {
  esy_err_t ret;

  ret = esy_hb_wifi_start_sta(config);
  if (ret != ESY_OK) {
    return ret;
  }

  for (;;) {
    ret = esy_hb_wifi_process_sta();
    if (ret == ESY_OK) {
      return ESY_OK;
    }
    if (ret != ESY_ERR_BUSY) {
      s_ctx.connected = false;
      return ret;
    }
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(100u));
  }
}

bool esy_hb_wifi_is_connected(void) { return is_sta_link_up(); }

esy_err_t esy_hb_wifi_get_ipv4_addr(esy_wifi_ipv4_addr_t *out_addr) {
  const ip4_addr_t *addr;

  if (out_addr == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!is_sta_link_up()) {
    return ESY_ERR_STATE;
  }

  cyw43_arch_lwip_begin();
  addr = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
  if (ip4_addr_isany_val(*addr)) {
    cyw43_arch_lwip_end();
    return ESY_ERR_STATE;
  }

  out_addr->octets[0] = ip4_addr1(addr);
  out_addr->octets[1] = ip4_addr2(addr);
  out_addr->octets[2] = ip4_addr3(addr);
  out_addr->octets[3] = ip4_addr4(addr);
  cyw43_arch_lwip_end();
  return ESY_OK;
}

void esy_hb_wifi_deinit(void) {
  if (!s_ctx.initialized) {
    return;
  }

  cyw43_arch_deinit();
  s_ctx.connected = false;
  s_ctx.connect_active = false;
  s_ctx.initialized = false;
}
