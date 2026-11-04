/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_event.h"
#include "esp_netif.h"
#include "esy_hal/esy_hal_wifi.h"
#include "esy_middleware/wifi/esy_wifi_manager.h"
#include "esy_middleware/network/esy_network_lwip.h"

void esy_native_probe_radio(void) {
  esy_wifi_config_t wifi_config;
  esy_wifi_network_runtime_t runtime = {0};
  esy_network_lwip_t endpoint = {0};
  uint8_t rx_buffer[128];
  esy_network_lwip_static_resources_t resources = {
      .rx_buffer = rx_buffer, .rx_buffer_size = sizeof(rx_buffer)};
  esy_network_lwip_config_t network_config = {.static_resources = &resources};
  (void)esp_netif_init();
  (void)esp_event_loop_create_default();
  esy_hal_wifi_default_cfg(&wifi_config);
  (void)esy_hal_wifi_init_sta(&wifi_config);
  if (esy_hal_wifi_get_network_runtime(&runtime) == ESY_OK) {
    network_config.progress = runtime.progress;
    network_config.progress_context = runtime.context;
    network_config.owner_enter = runtime.enter;
    network_config.owner_leave = runtime.leave;
    network_config.owner_context = runtime.context;
    (void)esy_network_lwip_init(&endpoint, &network_config);
    (void)esy_network_lwip_deinit(&endpoint);
  }
  (void)esy_wifi_manager_init(NULL, NULL);
  (void)esy_hal_wifi_deinit();
}
