/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/defs/coordination/esy_atomic_defs.h"
#include "esy_middleware/wifi/esy_wifi_manager.h"
#include "esy_middleware/wifi/esy_wifi_manager_device.h"

#include <stdatomic.h>
#include <string.h>

#include "esy_hal/esy_hal_wifi.h"

typedef struct {
  /* Lifecycle calls are externally serialized; the backend has one owner. */
  esy_wifi_manager_t *owner;
} esy_wifi_manager_ctx_t;

static esy_wifi_manager_ctx_t s_ctx;

static esy_err_t wifi_acquire(esy_wifi_manager_t *manager) {
  return (manager->device == NULL) ? ESY_OK : esy_device_get(manager->device);
}

static esy_err_t wifi_release(esy_wifi_manager_t *manager, esy_err_t result) {
  if (manager->device != NULL) {
    esy_err_t ret = esy_device_put(manager->device);
    if (ret != ESY_OK) result = ret;
  }
  atomic_store(&manager->last_error, result);
  return result;
}

static bool valid_string(const char *value, size_t maximum, bool allow_empty) {
  if ((value == NULL) || (!allow_empty && (value[0] == '\0'))) return false;
  for (size_t i = 0u; i <= maximum; i++) {
    if (value[i] == '\0') return true;
  }
  return false;
}

static esy_err_t validate_config(const esy_wifi_config_t *config) {
  if (!valid_string(config->ssid, ESY_WIFI_SSID_MAX_LENGTH, false) ||
      !valid_string(config->password, ESY_WIFI_PASSWORD_MAX_LENGTH, true)) {
    return ESY_ERR_INVALID_ARG;
  }
  switch (config->auth) {
    case ESY_WIFI_AUTH_OPEN:
    case ESY_WIFI_AUTH_WPA2_AES_PSK:
    case ESY_WIFI_AUTH_WPA2_MIXED_PSK:
      return ESY_OK;
    default:
      return ESY_ERR_NOT_SUPPORTED;
  }
}

void esy_wifi_manager_default_cfg(esy_wifi_manager_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  esy_hal_wifi_default_cfg(&config->sta_config);
}

esy_err_t esy_wifi_manager_init(esy_wifi_manager_t *manager,
                                const esy_wifi_manager_config_t *config) {
  esy_err_t ret;
  if ((manager == NULL) || (config == NULL)) return ESY_ERR_INVALID_ARG;
  if (s_ctx.owner == manager) return ESY_ERR_STATE;
  if (s_ctx.owner != NULL) return ESY_ERR_BUSY;
  ret = validate_config(&config->sta_config);
  if (ret != ESY_OK) return ret;

  /* No manager state is read before first initialization. */
  memset(manager, 0, sizeof(*manager));
  manager->config = *config;
  memcpy(manager->ssid, config->sta_config.ssid,
         strlen(config->sta_config.ssid) + 1u);
  memcpy(manager->password, config->sta_config.password,
         strlen(config->sta_config.password) + 1u);
  manager->config.sta_config.ssid = manager->ssid;
  manager->config.sta_config.password = manager->password;
  atomic_init(&manager->state, ESY_WIFI_MANAGER_STATE_IDLE);
  atomic_init(&manager->last_error, ESY_OK);
  atomic_init(&manager->connect_requested, 0u);
  atomic_init(&manager->ipv4_snapshot, 0u);
  atomic_init(&manager->has_ipv4_addr, 0u);
  manager->initialized = true;
  s_ctx.owner = manager;
  return ESY_OK;
}

esy_err_t esy_wifi_manager_request_connect(esy_wifi_manager_t *manager) {
  if (manager == NULL) return ESY_ERR_INVALID_ARG;
  if (!manager->initialized) return ESY_ERR_STATE;
  /* MPSC task requests only publish intent; they never reset an active attempt. */
  atomic_store(&manager->connect_requested, 1u);
  return ESY_OK;
}

static esy_err_t refresh_address(esy_wifi_manager_t *manager) {
  esy_wifi_ipv4_addr_t address;
  esy_err_t ret = esy_hal_wifi_get_ipv4_addr(&address);
  if (ret == ESY_OK) {
    uint32_t packed = ((uint32_t)address.octets[0] << 24u) |
                      ((uint32_t)address.octets[1] << 16u) |
                      ((uint32_t)address.octets[2] << 8u) | address.octets[3];
    atomic_store(&manager->ipv4_snapshot, packed);
    atomic_store(&manager->has_ipv4_addr, 1u);
  } else {
    atomic_store(&manager->has_ipv4_addr, 0u);
  }
  return ret;
}

static esy_err_t finish_connected(esy_wifi_manager_t *manager) {
  esy_err_t address_ret = refresh_address(manager);
  esy_err_t ret = wifi_release(
      manager, (address_ret == ESY_ERR_STATE) ? ESY_OK : address_ret);
  if ((ret == ESY_OK) && (address_ret == ESY_ERR_STATE)) {
    /* Pending DHCP is nonfatal, but retain its diagnostic status. */
    atomic_store(&manager->last_error, address_ret);
  }
  return ret;
}

esy_err_t esy_wifi_manager_process(esy_wifi_manager_t *manager) {
  esy_err_t ret;
  if (manager == NULL) return ESY_ERR_INVALID_ARG;
  if (!manager->initialized) return ESY_ERR_STATE;
  ret = wifi_acquire(manager);
  if (ret != ESY_OK) {
    atomic_store(&manager->last_error, ret);
    return ret;
  }

  if (atomic_load(&manager->state) == ESY_WIFI_MANAGER_STATE_CONNECTED) {
    if (esy_hal_wifi_is_connected()) {
      /* Refresh on every poll, including delayed DHCP and address changes. */
      return finish_connected(manager);
    }
    atomic_store(&manager->state, ESY_WIFI_MANAGER_STATE_DISCONNECTED);
    atomic_store(&manager->has_ipv4_addr, 0u);
    manager->connect_started = false;
  }

  if (atomic_load(&manager->connect_requested) == 0u) {
    return wifi_release(manager, ESY_OK);
  }
  if (!manager->connect_started) {
    atomic_store(&manager->state, ESY_WIFI_MANAGER_STATE_CONNECTING);
    ret = esy_hal_wifi_start_sta(&manager->config.sta_config);
    if (ret == ESY_ERR_BUSY) {
      return wifi_release(manager, ESY_OK);
    }
    if (ret != ESY_OK) {
      atomic_store(&manager->state, ESY_WIFI_MANAGER_STATE_FAILED);
      atomic_store(&manager->has_ipv4_addr, 0u);
      return wifi_release(manager, ret);
    }
    manager->connect_started = true;
  }

  ret = esy_hal_wifi_process_sta();
  if (ret == ESY_ERR_BUSY) {
    atomic_store(&manager->state, ESY_WIFI_MANAGER_STATE_CONNECTING);
    return wifi_release(manager, ESY_OK);
  }
  if (ret != ESY_OK) {
    atomic_store(&manager->state, ESY_WIFI_MANAGER_STATE_FAILED);
    atomic_store(&manager->has_ipv4_addr, 0u);
    manager->connect_started = false;
    return wifi_release(manager, ret);
  }

  atomic_store(&manager->state, ESY_WIFI_MANAGER_STATE_CONNECTED);
  manager->connect_started = false;
  return finish_connected(manager);
}

esy_err_t esy_wifi_manager_stop(esy_wifi_manager_t *manager) {
  esy_err_t ret;
  if (manager == NULL) return ESY_ERR_INVALID_ARG;
  if (!manager->initialized) return ESY_ERR_STATE;
  ret = wifi_acquire(manager);
  if (ret != ESY_OK) {
    atomic_store(&manager->last_error, ret);
    return ret;
  }
  /* Requests published after this cancellation remain pending for process(). */
  atomic_store(&manager->connect_requested, 0u);
  esy_hal_wifi_deinit();
  manager->connect_started = false;
  atomic_store(&manager->has_ipv4_addr, 0u);
  atomic_store(&manager->state, ESY_WIFI_MANAGER_STATE_IDLE);
  return wifi_release(manager, ESY_OK);
}

esy_err_t esy_wifi_manager_deinit(esy_wifi_manager_t *manager) {
  if (manager == NULL) return ESY_ERR_INVALID_ARG;
  if (s_ctx.owner != manager) return ESY_ERR_STATE;
  if (manager->device != NULL) return ESY_ERR_BUSY;
  /* Lifecycle callers have quiesced request/query/process and PM endpoints. */
  esy_hal_wifi_deinit();
  memset(manager, 0, sizeof(*manager));
  s_ctx.owner = NULL;
  return ESY_OK;
}

static esy_err_t wifi_device_pm_action(esy_device_t *device,
                                       esy_device_pm_action_t action,
                                       esy_power_state_t system_state) {
  esy_wifi_manager_device_binding_t *binding;
  esy_wifi_manager_t *manager;
  (void)system_state;
  if ((device == NULL) || (device->driver_ctx == NULL)) return ESY_ERR_INVALID_ARG;
  binding = device->driver_ctx;
  manager = binding->manager;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (binding->suspended) return ESY_ERR_STATE;
      esy_hal_wifi_deinit();
      atomic_store(&manager->state, ESY_WIFI_MANAGER_STATE_DISCONNECTED);
      manager->connect_started = false;
      atomic_store(&manager->has_ipv4_addr, 0u);
      binding->suspended = true;
      return ESY_OK;
    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!binding->suspended) return ESY_ERR_STATE;
      /* Keep the live atomic request, including requests made while suspended. */
      manager->connect_started = false;
      binding->suspended = false;
      return ESY_OK;
    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static bool wifi_device_is_runtime_idle(const esy_device_t *device) {
  const esy_wifi_manager_device_binding_t *binding;
  if ((device == NULL) || (device->driver_ctx == NULL)) return false;
  binding = device->driver_ctx;
  /* The persistent request covers connecting, connected and retry states.
   * Device core also checks references before suspension. No owner-only state
   * is read here: this callback may overlap process() or MPSC requests. */
  return atomic_load(&binding->manager->connect_requested) == 0u;
}

static const esy_device_ops_t k_wifi_device_ops = {
    .pm_action = wifi_device_pm_action,
    .is_runtime_idle = wifi_device_is_runtime_idle,
};

esy_err_t esy_wifi_manager_register_device(
    esy_wifi_manager_device_binding_t *binding, esy_wifi_manager_t *manager,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config) {
  esy_device_config_t config;
  esy_err_t ret;
  if ((binding == NULL) || (manager == NULL) || (registry == NULL) ||
      (device_config == NULL)) return ESY_ERR_INVALID_ARG;
  if ((s_ctx.owner != manager) || !manager->initialized) return ESY_ERR_STATE;
  /* Binding storage is explicitly required to be zero-initialized on first use. */
  if ((manager->device != NULL) || (binding->manager != NULL) ||
      (binding->device.registry != NULL)) return ESY_ERR_STATE;
  config.name = device_config->name;
  config.ops = &k_wifi_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->dependencies;
  config.dependency_count = device_config->dependency_count;
  config.supported_state_mask = device_config->supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    binding->manager = manager;
    binding->suspended = false;
    manager->device = &binding->device;
  }
  return ret;
}

esy_err_t esy_wifi_manager_unregister_device(
    esy_wifi_manager_device_binding_t *binding) {
  esy_err_t ret;
  if ((binding == NULL) || (binding->manager == NULL) ||
      (binding->manager->device != &binding->device)) return ESY_ERR_INVALID_ARG;
  /* Registry deinit invalidates the embedded device. Detach the surviving
   * typed association after all graph and manager endpoints are quiescent. */
  ret = (binding->device.registry == NULL) ? ESY_OK :
        esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    binding->manager->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

esy_wifi_manager_state_t esy_wifi_manager_get_state(
    const esy_wifi_manager_t *manager) {
  esy_wifi_manager_state_t state;
  if ((manager == NULL) || !manager->initialized) return ESY_WIFI_MANAGER_STATE_FAILED;
  state = (esy_wifi_manager_state_t)atomic_load(&manager->state);
  if ((state == ESY_WIFI_MANAGER_STATE_IDLE) &&
      (atomic_load(&manager->connect_requested) != 0u)) {
    state = ESY_WIFI_MANAGER_STATE_DISCONNECTED;
  }
  return state;
}

bool esy_wifi_manager_is_connected(const esy_wifi_manager_t *manager) {
  return esy_wifi_manager_get_state(manager) == ESY_WIFI_MANAGER_STATE_CONNECTED;
}

esy_err_t esy_wifi_manager_get_ipv4_addr(const esy_wifi_manager_t *manager,
                                         esy_wifi_ipv4_addr_t *out_addr) {
  uint32_t packed;
  if ((manager == NULL) || (out_addr == NULL)) return ESY_ERR_INVALID_ARG;
  if (!manager->initialized || (atomic_load(&manager->has_ipv4_addr) == 0u)) {
    return ESY_ERR_STATE;
  }
  packed = atomic_load(&manager->ipv4_snapshot);
  out_addr->octets[0] = (uint8_t)(packed >> 24u);
  out_addr->octets[1] = (uint8_t)(packed >> 16u);
  out_addr->octets[2] = (uint8_t)(packed >> 8u);
  out_addr->octets[3] = (uint8_t)packed;
  return ESY_OK;
}

esy_err_t esy_wifi_manager_get_last_error(const esy_wifi_manager_t *manager) {
  if (manager == NULL) return ESY_ERR_INVALID_ARG;
  if (!manager->initialized) return ESY_ERR_STATE;
  return atomic_load(&manager->last_error);
}
