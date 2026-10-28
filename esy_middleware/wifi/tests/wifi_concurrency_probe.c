/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/wifi/esy_wifi_manager_device.h"
#include "fake_esy_hal_wifi.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#define PROBE_ITERATIONS 10000u

typedef struct {
  esy_wifi_manager_t manager;
  esy_device_registry_t registry;
  atomic_bool done;
} wifi_probe_ctx_t;

static void *request_and_query(void *arg) {
  wifi_probe_ctx_t *ctx = arg;
  esy_wifi_ipv4_addr_t address;
  do {
    assert(esy_wifi_manager_request_connect(&ctx->manager) == ESY_OK);
    (void)esy_wifi_manager_get_state(&ctx->manager);
    (void)esy_wifi_manager_is_connected(&ctx->manager);
    (void)esy_wifi_manager_get_last_error(&ctx->manager);
    esy_err_t ret = esy_wifi_manager_get_ipv4_addr(&ctx->manager, &address);
    assert(ret == ESY_OK || ret == ESY_ERR_STATE);
    if (ret == ESY_OK) {
      assert(address.octets[0] == 192u && address.octets[3] == 1u);
    }
  } while (!atomic_load(&ctx->done));
  return NULL;
}

static void *power_owner(void *arg) {
  wifi_probe_ctx_t *ctx = arg;
  do {
    esy_err_t ret = esy_device_registry_runtime_suspend_idle(&ctx->registry);
    assert(ret == ESY_OK || ret == ESY_ERR_BUSY);
  } while (!atomic_load(&ctx->done));
  return NULL;
}

int main(void) {
  wifi_probe_ctx_t ctx = {0};
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t rc = {.static_resources = &resources};
  esy_wifi_manager_device_binding_t binding = {0};
  esy_device_registration_config_t dc = {
      .name = "wifi",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  esy_wifi_manager_config_t config = {.sta_config = {
      .ssid = "test", .password = "secret", .auth = ESY_WIFI_AUTH_WPA2_AES_PSK,
      .connect_timeout_ms = 1000u}};
  pthread_t producers[2];
  pthread_t pm;
  fake_esy_hal_wifi_reset();
  atomic_init(&ctx.done, false);
  assert(esy_wifi_manager_init(&ctx.manager, &config) == ESY_OK);
  assert(esy_device_registry_init(&ctx.registry, &rc) == ESY_OK);
  assert(esy_wifi_manager_register_device(&binding, &ctx.manager, &ctx.registry, &dc) == ESY_OK);
  assert(esy_device_registry_start(&ctx.registry) == ESY_OK);
  assert(pthread_create(&pm, NULL, power_owner, &ctx) == 0);
  for (unsigned i = 0u; i < 2u; i++) {
    assert(pthread_create(&producers[i], NULL, request_and_query, &ctx) == 0);
  }
  for (unsigned i = 0u; i < PROBE_ITERATIONS; i++) {
    esy_err_t ret = esy_wifi_manager_process(&ctx.manager);
    assert(ret == ESY_OK || ret == ESY_ERR_BUSY);
    ret = esy_wifi_manager_stop(&ctx.manager);
    assert(ret == ESY_OK || ret == ESY_ERR_BUSY);
  }
  atomic_store(&ctx.done, true);
  for (unsigned i = 0u; i < 2u; i++) assert(pthread_join(producers[i], NULL) == 0);
  assert(pthread_join(pm, NULL) == 0);
  /* A final request after contention must still reach the backend. */
  assert(esy_wifi_manager_request_connect(&ctx.manager) == ESY_OK);
  assert(esy_wifi_manager_process(&ctx.manager) == ESY_OK);
  assert(esy_wifi_manager_is_connected(&ctx.manager));
  assert(esy_device_registry_deinit(&ctx.registry) == ESY_OK);
  assert(esy_wifi_manager_unregister_device(&binding) == ESY_OK);
  assert(esy_wifi_manager_deinit(&ctx.manager) == ESY_OK);
  return 0;
}
