/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/touch/esy_touch_device.h"
#include "esy_hal/esy_hal_gpio_pm.h"

#include <string.h>

static esy_err_t touch_device_pm_action(esy_device_t *device,
                                        esy_device_pm_action_t action,
                                        esy_power_state_t system_state) {
  esy_touch_device_binding_t *binding;

  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_touch_device_binding_t *)device->driver_ctx;
  return binding->transition(binding, action, system_state);
}

static const esy_device_ops_t k_touch_device_ops = {
    .pm_action = touch_device_pm_action,
};

esy_err_t esy_touch_register_device(
    esy_touch_device_binding_t *binding, esy_touch_t *touch,
    esy_device_registry_t *registry,
    const esy_touch_device_config_t *device_config,
    esy_touch_device_transition_fn_t transition, void *owner) {
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (touch == NULL) || (registry == NULL) ||
      (device_config == NULL) || (transition == NULL) ||
      !esy_touch_is_attached(touch)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((touch->device != NULL) || (binding->touch != NULL) ||
      (binding->device.registry != NULL)) {
    return ESY_ERR_STATE;
  }
  memset(binding, 0, sizeof(*binding));
  binding->touch = touch;
  binding->transition = transition;
  binding->gpio_pm_profile = device_config->gpio_pm_profile;
  binding->owner = owner;
  ret = esy_hal_gpio_pm_claim(binding->gpio_pm_profile, binding);
  if (ret != ESY_OK) {
    memset(binding, 0, sizeof(*binding));
    return ret;
  }
  config.name = device_config->registration.name;
  config.ops = &k_touch_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->registration.dependencies;
  config.dependency_count = device_config->registration.dependency_count;
  config.supported_state_mask =
      device_config->registration.supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    touch->device = &binding->device;
  } else {
    const esy_err_t cleanup_ret =
        esy_hal_gpio_pm_release(binding->gpio_pm_profile, binding);
    if (cleanup_ret != ESY_OK) {
      touch->device = &binding->device;
      return cleanup_ret;
    }
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

esy_err_t esy_touch_unregister_device(esy_touch_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->touch == NULL) ||
      (binding->touch->device != &binding->device)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    ret = esy_hal_gpio_pm_release(binding->gpio_pm_profile, binding);
  }
  if (ret == ESY_OK) {
    binding->touch->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}
