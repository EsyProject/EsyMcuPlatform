/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_freq_synth_device.h"

#include <string.h>
#include "esy_hal/esy_hal_gpio_pm.h"

static esy_err_t esy_freq_synth_device_pm_action(
    esy_device_t *device, esy_device_pm_action_t action,
    esy_power_state_t system_state) {
  esy_freq_synth_device_binding_t *binding;

  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_freq_synth_device_binding_t *)device->driver_ctx;
  esy_freq_synth_runtime_t *runtime = esy_freq_synth_runtime(binding->synth);
  if (runtime->faulted) return ESY_ERR_STATE;
  esy_err_t ret = binding->transition(binding, action, system_state);
  if (ret == ESY_ERR_STATE) runtime->faulted = true;
  return ret;
}

static bool esy_freq_synth_device_is_runtime_idle(
    const esy_device_t *device) {
  const esy_freq_synth_device_binding_t *binding;

  if ((device == NULL) || (device->driver_ctx == NULL)) return false;
  binding = device->driver_ctx;
  const esy_freq_synth_runtime_t *runtime =
      esy_freq_synth_runtime_const(binding->synth);
  return !runtime->faulted && binding->is_runtime_idle(binding);
}

static const esy_device_ops_t k_freq_synth_device_ops = {
    .pm_action = esy_freq_synth_device_pm_action,
    .is_runtime_idle = esy_freq_synth_device_is_runtime_idle,
};

esy_err_t esy_freq_synth_register_device(
    esy_freq_synth_device_binding_t *binding, esy_freq_synth_t *synth,
    esy_device_registry_t *registry,
    const esy_freq_synth_device_config_t *device_config,
    esy_freq_synth_device_transition_fn_t transition,
    esy_freq_synth_device_is_runtime_idle_fn_t is_runtime_idle, void *owner) {
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (synth == NULL) || (registry == NULL) ||
      (device_config == NULL) || (transition == NULL) ||
      (is_runtime_idle == NULL) ||
      !esy_freq_synth_is_attached(synth)) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_freq_synth_runtime_t *runtime = esy_freq_synth_runtime(synth);
  if (binding->synth != NULL || binding->registered || runtime->device != NULL ||
      runtime->faulted) return ESY_ERR_STATE;
  memset(binding, 0, sizeof(*binding));
  binding->synth = synth;
  binding->transition = transition;
  binding->is_runtime_idle = is_runtime_idle;
  binding->gpio_pm_profile = device_config->gpio_pm_profile;
  binding->owner = owner;

  ret = esy_hal_gpio_pm_claim(binding->gpio_pm_profile, binding);
  if (ret != ESY_OK) {
    memset(binding, 0, sizeof(*binding));
    return ret;
  }

  config.name = device_config->registration.name;
  config.ops = &k_freq_synth_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->registration.dependencies;
  config.dependency_count = device_config->registration.dependency_count;
  config.supported_state_mask =
      device_config->registration.supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    runtime->device = &binding->device;
    binding->registered = true;
  } else {
    const esy_err_t release =
        esy_hal_gpio_pm_release(binding->gpio_pm_profile, binding);
    if (release == ESY_OK) {
      memset(binding, 0, sizeof(*binding));
    } else {
      /* Retain the owner link so unregister can finish releasing the pins. */
      runtime->device = &binding->device;
      ret = release;
    }
  }
  return ret;
}

esy_err_t esy_freq_synth_unregister_device(
    esy_freq_synth_device_binding_t *binding) {
  esy_err_t ret;

  if (binding == NULL) return ESY_ERR_INVALID_ARG;
  if (binding->synth == NULL) return ESY_OK;
  esy_freq_synth_runtime_t *runtime = esy_freq_synth_runtime(binding->synth);
  if (runtime->device != &binding->device) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->registered) {
    ret = binding->device.registry == NULL
              ? ESY_OK : esy_device_unregister(&binding->device);
    if (ret != ESY_OK) return ret;
    binding->registered = false;
  }
  ret = esy_hal_gpio_pm_release(binding->gpio_pm_profile, binding);
  if (ret == ESY_OK) {
    runtime->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}
