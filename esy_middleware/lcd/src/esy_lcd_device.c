/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lcd/esy_lcd_device.h"
#include "esy_hal/esy_hal_gpio_pm.h"

#include <string.h>

static esy_err_t lcd_device_pm_action(esy_device_t *device,
                                      esy_device_pm_action_t action,
                                      esy_power_state_t system_state) {
  esy_lcd_device_binding_t *binding;

  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_lcd_device_binding_t *)device->driver_ctx;
  return binding->transition(binding, action, system_state);
}

static bool lcd_device_is_runtime_idle(const esy_device_t *device) {
  (void)device;
  return false;
}

static const esy_device_ops_t k_lcd_device_ops = {
    .pm_action = lcd_device_pm_action,
    .is_runtime_idle = lcd_device_is_runtime_idle,
};

esy_err_t esy_lcd_register_device(
    esy_lcd_device_binding_t *binding, esy_lcd_t *lcd,
    esy_device_registry_t *registry,
    const esy_lcd_device_config_t *device_config,
    esy_lcd_device_transition_fn_t transition, void *owner) {
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (lcd == NULL) || (registry == NULL) ||
      (device_config == NULL) || (transition == NULL) ||
      !esy_lcd_is_attached(lcd) || (lcd->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->lcd != NULL) return ESY_ERR_STATE;
  memset(binding, 0, sizeof(*binding));
  atomic_init(&binding->flush_ref_held, 0u);
  binding->lcd = lcd;
  binding->transition = transition;
  binding->gpio_pm_profile = device_config->gpio_pm_profile;
  binding->owner = owner;
  ret = esy_hal_gpio_pm_claim(binding->gpio_pm_profile, binding);
  if (ret != ESY_OK) {
    memset(binding, 0, sizeof(*binding));
    return ret;
  }
  config.name = device_config->registration.name;
  config.ops = &k_lcd_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->registration.dependencies;
  config.dependency_count = device_config->registration.dependency_count;
  config.supported_state_mask =
      device_config->registration.supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    lcd->device = &binding->device;
  } else {
    (void)esy_hal_gpio_pm_release(binding->gpio_pm_profile, binding);
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

esy_err_t esy_lcd_unregister_device(esy_lcd_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->lcd == NULL) ||
      (binding->lcd->device != &binding->device) ||
      atomic_load_explicit(&binding->flush_ref_held, memory_order_acquire)) {
    return ESY_ERR_INVALID_ARG;
  }
  /* A NULL registry also covers registry teardown and a previous unregister
   * whose GPIO release failed. Keep the binding until every stage succeeds. */
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    ret = esy_hal_gpio_pm_release(binding->gpio_pm_profile, binding);
  }
  if (ret == ESY_OK) {
    binding->lcd->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}
