/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_timer_capture.h"
#include "esy_hal/esy_hal_timer_capture_device.h"
#include "esy_hal_timer_capture_internal.h"

#include "esy_hal_backend/esy_hb_timer_capture.h"
#include "esy_board/esy_board.h"
#include <string.h>

static esy_err_t capture_acquire(esy_timer_capture_t *capture) {
  esy_hal_timer_capture_runtime_t *runtime = esy_hal_timer_capture_runtime(capture);
  return (runtime->device == NULL) ? ESY_OK : esy_device_get(runtime->device);
}

static void capture_release(esy_timer_capture_t *capture) {
  esy_hal_timer_capture_runtime_t *runtime = esy_hal_timer_capture_runtime(capture);
  if (runtime->device != NULL) {
    (void)esy_device_put(runtime->device);
  }
}

/* ============== vt_* sole ops implementation ============== */

static esy_err_t vt_get_value(const esy_timer_capture_t *cap,
                              uint32_t *out_value) {
  return esy_hb_timer_capture_get_value(cap, out_value);
}

static esy_err_t vt_get_interval(const esy_timer_capture_t *cap,
                                 uint32_t *out_interval) {
  return esy_hb_timer_capture_get_interval(cap, out_interval);
}

static esy_err_t vt_get_phys_index(const esy_timer_capture_t *cap,
                                   uint8_t *out_index) {
  return esy_hb_timer_capture_get_phys_index(cap, out_index);
}

static esy_err_t vt_get_caps(const esy_timer_capture_t *cap,
                             uint32_t *out_caps) {
  return esy_hb_timer_capture_get_caps(cap, out_caps);
}

/* ============== helpers ============== */

static const esy_timer_capture_vtable_t k_timer_capture_ops = {
    .get_value = vt_get_value,
    .get_interval = vt_get_interval,
    .get_phys_index = vt_get_phys_index,
    .get_caps = vt_get_caps,
};

static esy_err_t capture_cleanup_hardware(esy_timer_capture_t *capture,
                                           bool retain_reservation) {
  esy_hal_timer_capture_runtime_t *runtime = esy_hal_timer_capture_runtime(capture);
  if (capture->ctx == NULL) return ESY_OK;
  uint8_t index = ESY_PIN_UNUSED;
  esy_err_t ret = esy_hb_timer_capture_get_phys_index(capture, &index);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_timer_capture_suspend(capture);
  if (ret != ESY_OK) return ret;
  /* Keep the resource claim until IRQ retirement and board teardown finish. */
  if (runtime->pins_configured) {
    esy_board_timer_capture_reset_pin(index);
    runtime->pins_configured = false;
  }
  return retain_reservation ? ESY_OK : esy_hb_timer_capture_deinit(capture);
}

static esy_err_t capture_hardware_init(
    esy_timer_capture_t *capture, const esy_timer_capture_config_t *config,
    bool resuming) {
  esy_hal_timer_capture_runtime_t *runtime = esy_hal_timer_capture_runtime(capture);
  esy_timer_capture_config_t local = *config;
  esy_err_t ret = esy_hb_timer_capture_claim(capture, &local);
  if (ret != ESY_OK) return ret;
  if (!esy_board_timer_capture_configure_pin(local.phys_index, &local.inner)) {
    ret = ESY_ERR_NOT_SUPPORTED;
  } else {
    runtime->pins_configured = true;
    ret = esy_hb_timer_capture_init(capture, &local);
  }
  if (ret == ESY_OK) {
    capture->ops = &k_timer_capture_ops;
    return ESY_OK;
  }
  const esy_err_t cleanup = capture_cleanup_hardware(capture, resuming);
  if (cleanup != ESY_OK) return cleanup;
  if (!resuming) capture->ops = NULL;
  return ret;
}

static esy_err_t capture_device_pm_action(esy_device_t *device,
                                          esy_device_pm_action_t action,
                                          esy_power_state_t system_state) {
  esy_timer_capture_device_binding_t *binding;
  esy_err_t ret;

  (void)system_state;
  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_timer_capture_device_binding_t *)device->driver_ctx;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (binding->suspended) return ESY_ERR_STATE;
      ret = capture_cleanup_hardware(binding->capture, true);
      /* Native retirement may already have disabled IRQs or stopped capture.
       * Fault the graph rather than promise unchanged hardware after failure. */
      if (ret != ESY_OK) return ESY_ERR_STATE;
      binding->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!binding->suspended) return ESY_ERR_STATE;
      ret = capture_hardware_init(binding->capture, &binding->resume_config, true);
      if (ret != ESY_OK) return ret;
      binding->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static bool capture_device_is_runtime_idle(const esy_device_t *device) {
  (void)device;
  return false;
}

static const esy_device_ops_t k_capture_device_ops = {
    .pm_action = capture_device_pm_action,
    .is_runtime_idle = capture_device_is_runtime_idle,
};

/* ============== defaults ============== */

extern void esy_hal_timer_capture_default_cfg(
    esy_timer_capture_config_t *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->rising_edge = true;
  cfg->inner.gpio = ESY_PIN_UNUSED;
}

/* ============== lifecycle ============== */

extern esy_err_t esy_hal_timer_capture_init(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  esy_hal_timer_capture_runtime_t *runtime = esy_hal_timer_capture_runtime(cap);
  if (!cap || !cfg) return ESY_ERR_INVALID_ARG;
  if (cap->ctx != NULL || runtime->device != NULL || cap->ops != NULL)
    return ESY_ERR_STATE;
  return capture_hardware_init(cap, cfg, false);
}

extern esy_err_t esy_hal_timer_capture_deinit(esy_timer_capture_t *cap) {
  esy_hal_timer_capture_runtime_t *runtime = esy_hal_timer_capture_runtime(cap);
  if (cap == NULL) return ESY_ERR_INVALID_ARG;
  if (runtime->device != NULL) return ESY_ERR_STATE;
  esy_err_t ret = capture_cleanup_hardware(cap, false);
  if (ret != ESY_OK) return ret;
  memset(cap, 0, sizeof(*cap));
  return ESY_OK;
}

esy_err_t esy_hal_timer_capture_register_device(
    esy_timer_capture_device_binding_t *binding, esy_timer_capture_t *capture,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_timer_capture_config_t *resume_config) {
  esy_hal_timer_capture_runtime_t *runtime = esy_hal_timer_capture_runtime(capture);
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (capture == NULL) || (registry == NULL) ||
      (device_config == NULL) || (resume_config == NULL) ||
      (runtime->device != NULL) || (capture->ctx == NULL) ||
      (capture->ops == NULL || capture->ops->get_value == NULL) ||
      !runtime->pins_configured) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->capture != NULL) return ESY_ERR_STATE;
  memset(binding, 0, sizeof(*binding));
  binding->capture = capture;
  binding->resume_config = *resume_config;
  config.name = device_config->name;
  config.ops = &k_capture_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->dependencies;
  config.dependency_count = device_config->dependency_count;
  config.supported_state_mask = device_config->supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    runtime->device = &binding->device;
  } else {
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

esy_err_t esy_hal_timer_capture_unregister_device(
    esy_timer_capture_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->capture == NULL) ||
      (esy_hal_timer_capture_runtime(binding->capture)->device != &binding->device)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    esy_hal_timer_capture_runtime(binding->capture)->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

/* ============== query wrappers: validate and forward to ops ============== */

extern esy_err_t esy_hal_timer_capture_get_value(
    const esy_timer_capture_t *cap, uint32_t *out_value) {
  esy_err_t ret;

  if (!cap || !out_value) return ESY_ERR_INVALID_ARG;
  if (cap->ops == NULL || cap->ops->get_value == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = capture_acquire((esy_timer_capture_t *)cap);
  if (ret != ESY_OK) return ret;
  ret = cap->ops->get_value(cap, out_value);
  capture_release((esy_timer_capture_t *)cap);
  return ret;
}

extern esy_err_t esy_hal_timer_capture_get_interval(
    const esy_timer_capture_t *cap, uint32_t *out_interval) {
  esy_err_t ret;

  if (!cap || !out_interval) return ESY_ERR_INVALID_ARG;
  if (cap->ops == NULL || cap->ops->get_interval == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = capture_acquire((esy_timer_capture_t *)cap);
  if (ret != ESY_OK) return ret;
  ret = cap->ops->get_interval(cap, out_interval);
  capture_release((esy_timer_capture_t *)cap);
  return ret;
}

extern esy_err_t esy_hal_timer_capture_get_phys_index(
    const esy_timer_capture_t *cap, uint8_t *out_index) {
  if (!cap || !out_index) return ESY_ERR_INVALID_ARG;
  if (cap->ops == NULL || cap->ops->get_phys_index == NULL)
    return ESY_ERR_NOT_IMPLEMENTED;
  return cap->ops->get_phys_index(cap, out_index);
}

extern esy_err_t esy_hal_timer_capture_get_caps(
    const esy_timer_capture_t *cap, uint32_t *out_caps) {
  if (!cap || !out_caps) return ESY_ERR_INVALID_ARG;
  if (cap->ops == NULL || cap->ops->get_caps == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return cap->ops->get_caps(cap, out_caps);
}
