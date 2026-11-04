/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_wifi.h"
#include "esy_hal_backend/esy_hb_system.h"

#include <limits.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

#if CONFIG_LWIP_TCPIP_CORE_LOCKING && CONFIG_LWIP_CHECK_THREAD_SAFETY
#include "lwip/tcpip.h"
#endif

#define WIFI_INIT_POLL_DELAY_MS 1u
#define WIFI_MICROSECONDS_PER_MILLISECOND 1000

typedef struct {
  esp_netif_t *netif;
  esy_wifi_auth_t auth;
  int64_t connect_deadline_us;
  esy_err_t terminal_result;
  bool driver_initialized;
  bool driver_started;
  bool netif_attached;
  bool connect_active;
  bool cleanup_required;
} esy_hb_wifi_ctx_t;

/* One externally serialized task owns station control and lifecycle. SDK-owned
 * default handlers maintain the netif on its event task; no native callback
 * reads this context. Association/address queries use the SDK's public APIs. */
static esy_hb_wifi_ctx_t s_ctx;

static esy_err_t wifi_error(esp_err_t error) {
  switch (error) {
    case ESP_OK: return ESY_OK;
    case ESP_ERR_NO_MEM: return ESY_ERR_NO_MEM;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_WIFI_SSID:
    case ESP_ERR_WIFI_PASSWORD: return ESY_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE:
    case ESP_ERR_WIFI_NOT_INIT:
    case ESP_ERR_WIFI_NOT_STARTED: return ESY_ERR_STATE;
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_WIFI_TIMEOUT: return ESY_ERR_TIMEOUT;
    default: return ESY_ERR_IO;
  }
}

static bool string_length(const char *value, size_t maximum, size_t *length) {
  if (value == NULL) return false;
  for (size_t i = 0u; i <= maximum; ++i) {
    if (value[i] == '\0') {
      *length = i;
      return true;
    }
  }
  return false;
}

static esy_err_t native_config(const esy_wifi_config_t *config,
                               wifi_config_t *native) {
  size_t ssid_length;
  size_t password_length;
  if (config == NULL ||
      !string_length(config->ssid, ESY_WIFI_SSID_MAX_LENGTH, &ssid_length) ||
      ssid_length == 0u ||
      !string_length(config->password, ESY_WIFI_PASSWORD_MAX_LENGTH,
                     &password_length)) return ESY_ERR_INVALID_ARG;
  memset(native, 0, sizeof(*native));
  switch (config->auth) {
    case ESY_WIFI_AUTH_OPEN:
      if (password_length != 0u) return ESY_ERR_INVALID_ARG;
      native->sta.threshold.authmode = WIFI_AUTH_OPEN;
      break;
    case ESY_WIFI_AUTH_WPA2_AES_PSK:
    case ESY_WIFI_AUTH_WPA2_MIXED_PSK:
      if (password_length < 8u) return ESY_ERR_INVALID_ARG;
      if (password_length == ESY_WIFI_PASSWORD_MAX_LENGTH) {
        for (size_t i = 0u; i < password_length; ++i) {
          const char digit = config->password[i];
          if (!((digit >= '0' && digit <= '9') ||
                (digit >= 'a' && digit <= 'f') ||
                (digit >= 'A' && digit <= 'F'))) return ESY_ERR_INVALID_ARG;
        }
      }
      native->sta.threshold.authmode = config->auth == ESY_WIFI_AUTH_WPA2_AES_PSK
          ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_WPA_PSK;
      break;
    default: return ESY_ERR_NOT_SUPPORTED;
  }
  memcpy(native->sta.ssid, config->ssid, ssid_length);
  memcpy(native->sta.password, config->password, password_length);
  return ESY_OK;
}

static esy_err_t initialize_station(void) {
  wifi_mode_t mode;
  esp_err_t error = esp_wifi_get_mode(&mode);
  /* Never adopt a native driver or default station interface owned elsewhere.
   * Consumers exclude native lifecycle races for the entire HAL lifetime. */
  if (error == ESP_OK) return ESY_ERR_BUSY;
  if (error != ESP_ERR_WIFI_NOT_INIT) return wifi_error(error);
  const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
  if (esp_netif_get_handle_from_ifkey(netif_config.base->if_key) != NULL)
    return ESY_ERR_BUSY;

  wifi_init_config_t driver_config = WIFI_INIT_CONFIG_DEFAULT();
  driver_config.nvs_enable = false;
  /* A failed SDK init can need deinit; retain this stage until cleanup succeeds
   * or the SDK confirms NOT_INIT. There is no ownership transfer on failure. */
  s_ctx.driver_initialized = true;
  error = esp_wifi_init(&driver_config);
  if (error != ESP_OK) return wifi_error(error);
  s_ctx.netif = esp_netif_new(&netif_config);
  if (s_ctx.netif == NULL) return ESY_ERR_NO_MEM;
  /* The native attach publishes its netif before allocation/attach can fail.
   * Its clear operation must therefore run even after a failed attach. */
  s_ctx.netif_attached = true;
  error = esp_netif_attach_wifi_station(s_ctx.netif);
  if (error != ESP_OK) return wifi_error(error);
  error = esp_wifi_set_default_wifi_sta_handlers();
  if (error != ESP_OK) return wifi_error(error);
  error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (error != ESP_OK) return wifi_error(error);
  return wifi_error(esp_wifi_set_mode(WIFI_MODE_STA));
}

static esy_err_t stop_station(void) {
  if (!s_ctx.driver_started) return ESY_OK;
  const esp_err_t error = esp_wifi_stop();
  if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_STARTED) {
    s_ctx.cleanup_required = true;
    return wifi_error(error);
  }
  s_ctx.driver_started = false;
  return ESY_OK;
}

static esy_err_t finish_attempt(esy_err_t result) {
  s_ctx.connect_active = false;
  s_ctx.terminal_result = result;
  const esy_err_t cleanup = stop_station();
  return cleanup == ESY_OK ? result : cleanup;
}

static bool authentication_matches(const wifi_ap_record_t *ap) {
  if (s_ctx.auth == ESY_WIFI_AUTH_OPEN) return ap->authmode == WIFI_AUTH_OPEN;
  /* AP compatibility with WPA does not exclude a WPA2/CCMP connection. */
  if (s_ctx.auth == ESY_WIFI_AUTH_WPA2_AES_PSK)
    return (ap->authmode == WIFI_AUTH_WPA2_PSK ||
            ap->authmode == WIFI_AUTH_WPA_WPA2_PSK ||
            ap->authmode == WIFI_AUTH_WPA2_WPA3_PSK) &&
           ap->pairwise_cipher == WIFI_CIPHER_TYPE_CCMP;
  return ap->authmode == WIFI_AUTH_WPA_PSK || ap->authmode == WIFI_AUTH_WPA2_PSK ||
         ap->authmode == WIFI_AUTH_WPA_WPA2_PSK ||
         ap->authmode == WIFI_AUTH_WPA2_WPA3_PSK;
}

void esy_hb_wifi_default_cfg(esy_wifi_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->auth = ESY_WIFI_AUTH_WPA2_AES_PSK;
  config->connect_timeout_ms = 30000u;
}

esy_err_t esy_hb_wifi_start_sta(const esy_wifi_config_t *config) {
  wifi_config_t native;
  esy_err_t result = native_config(config, &native);
  if (result != ESY_OK) return result;
  if (s_ctx.cleanup_required) return ESY_ERR_STATE;
  const int64_t now_us = esp_timer_get_time();
  const int64_t timeout_us =
      (int64_t)config->connect_timeout_ms * WIFI_MICROSECONDS_PER_MILLISECOND;
  if (now_us < 0 || now_us > INT64_MAX - timeout_us)
    return ESY_ERR_INVALID_ARG;

  if (!s_ctx.driver_initialized) {
    result = initialize_station();
    if (result != ESY_OK) {
      if (s_ctx.driver_initialized) s_ctx.cleanup_required = true;
      return result;
    }
  }
  s_ctx.connect_active = false;
  s_ctx.terminal_result = ESY_ERR_STATE;
  result = stop_station();
  if (result != ESY_OK) return result;
  esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &native);
  if (error == ESP_OK) {
    /* Start may allocate native state before failing. Preserve the obligation
     * to call stop; NOT_STARTED is accepted only by that retirement stage. */
    s_ctx.driver_started = true;
    error = esp_wifi_start();
  }
  if (error != ESP_OK) {
    s_ctx.cleanup_required = true;
    return wifi_error(error);
  }
  error = esp_wifi_connect();
  if (error != ESP_OK) {
    s_ctx.cleanup_required = true;
    return wifi_error(error);
  }
  s_ctx.auth = config->auth;
  s_ctx.connect_deadline_us = now_us + timeout_us;
  s_ctx.connect_active = true;
  s_ctx.terminal_result = ESY_ERR_BUSY;
  return ESY_OK;
}

esy_err_t esy_hb_wifi_process_sta(void) {
  if (!s_ctx.driver_initialized || s_ctx.netif == NULL || s_ctx.cleanup_required)
    return ESY_ERR_STATE;
  /* Terminal failure/cancellation is latched before sampling time or the SDK.
   * Only an explicit successful start can create another connection attempt. */
  if (!s_ctx.connect_active && s_ctx.terminal_result != ESY_OK)
    return s_ctx.terminal_result;
  const int64_t now_us = esp_timer_get_time();
  if (now_us < 0) return finish_attempt(ESY_ERR_IO);
  wifi_ap_record_t ap;
  const esp_err_t error = esp_wifi_sta_get_ap_info(&ap);
  if (error == ESP_OK) {
    if (!authentication_matches(&ap)) return finish_attempt(ESY_ERR_NOT_SUPPORTED);
    s_ctx.connect_active = false;
    s_ctx.terminal_result = ESY_OK;
    return ESY_OK;
  }
  if (error != ESP_ERR_WIFI_NOT_CONNECT) return finish_attempt(wifi_error(error));
  if (!s_ctx.connect_active) {
    s_ctx.terminal_result = ESY_ERR_STATE;
    return ESY_ERR_STATE;
  }
  if (now_us >= s_ctx.connect_deadline_us)
    return finish_attempt(ESY_ERR_TIMEOUT);
  return ESY_ERR_BUSY;
}

esy_err_t esy_hb_wifi_init_sta(const esy_wifi_config_t *config) {
  esy_err_t result = esy_hb_wifi_start_sta(config);
  if (result != ESY_OK) return result;
  do {
    result = esy_hb_wifi_process_sta();
    if (result == ESY_ERR_BUSY) esy_hb_system_delay_ms(WIFI_INIT_POLL_DELAY_MS);
  } while (result == ESY_ERR_BUSY);
  return result;
}

bool esy_hb_wifi_is_connected(void) {
  if (!s_ctx.driver_started || s_ctx.cleanup_required ||
      (!s_ctx.connect_active && s_ctx.terminal_result != ESY_OK)) return false;
  wifi_ap_record_t ap;
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK && authentication_matches(&ap);
}

esy_err_t esy_hb_wifi_get_ipv4_addr(esy_wifi_ipv4_addr_t *out_addr) {
  if (out_addr == NULL) return ESY_ERR_INVALID_ARG;
  if (!esy_hb_wifi_is_connected() || !esp_netif_is_netif_up(s_ctx.netif))
    return ESY_ERR_STATE;
  esp_netif_ip_info_t info;
  const esp_err_t error = esp_netif_get_ip_info(s_ctx.netif, &info);
  if (error != ESP_OK) return wifi_error(error);
  if (info.ip.addr == 0u) return ESY_ERR_STATE;
  const uint8_t *octets = (const uint8_t *)&info.ip.addr;
  *out_addr = (esy_wifi_ipv4_addr_t){
      .octets = {octets[0], octets[1], octets[2], octets[3]},
  };
  return ESY_OK;
}

#if CONFIG_LWIP_TCPIP_CORE_LOCKING && CONFIG_LWIP_CHECK_THREAD_SAFETY
static void network_progress(void *context) {
  /* Native tcpip and event tasks progress independently of the station owner. */
  (void)context;
}

static void network_enter(void *context) {
  (void)context;
  LOCK_TCPIP_CORE();
}

static void network_leave(void *context) {
  (void)context;
  UNLOCK_TCPIP_CORE();
}
#endif

esy_err_t esy_hb_wifi_get_network_runtime(
    esy_wifi_network_runtime_t *out_runtime) {
  if (out_runtime == NULL) return ESY_ERR_INVALID_ARG;
#if CONFIG_LWIP_TCPIP_CORE_LOCKING && CONFIG_LWIP_CHECK_THREAD_SAFETY
  *out_runtime = (esy_wifi_network_runtime_t){
      .progress = network_progress,
      .enter = network_enter,
      .leave = network_leave,
      .context = NULL,
  };
  return ESY_OK;
#else
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

esy_err_t esy_hb_wifi_deinit(void) {
  s_ctx.connect_active = false;
  s_ctx.terminal_result = ESY_ERR_STATE;
  const esy_err_t result = stop_station();
  if (result != ESY_OK) return result;
  if (s_ctx.netif_attached) {
    const esp_err_t error =
        esp_wifi_clear_default_wifi_driver_and_handlers(s_ctx.netif);
    /* ESP-IDF 5.3.4 always destroys the attached driver, including when its
     * preceding netif detach reports an error. Do not destroy it twice. The
     * netif and Wi-Fi driver remain owned for the next cleanup stage/retry. */
    s_ctx.netif_attached = false;
    if (error != ESP_OK) {
      s_ctx.cleanup_required = true;
      return wifi_error(error);
    }
  }
  if (s_ctx.netif != NULL) {
    esp_netif_destroy(s_ctx.netif);
    s_ctx.netif = NULL;
  }
  if (s_ctx.driver_initialized) {
    const esp_err_t error = esp_wifi_deinit();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_INIT) {
      s_ctx.cleanup_required = true;
      return wifi_error(error);
    }
    s_ctx.driver_initialized = false;
  }
  s_ctx.cleanup_required = false;
  return ESY_OK;
}
