/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_pwm.h"
#include "esy_hal/esy_hal_pwm_device.h"
#include "esy_hal_pwm_internal.h"

#include "esy_hal_backend/esy_hb_pwm.h" /* backend: esy_hb_pwm_xxx */
#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_hal/esy_hal_system.h"
#include "esy_board/esy_board.h"
#include <string.h>

static esy_err_t pwm_acquire(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  return (runtime->device == NULL) ? ESY_OK : esy_device_get(runtime->device);
}

static void pwm_release(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (runtime->device != NULL) {
    (void)esy_device_put(runtime->device);
  }
}

static esy_pwm_device_binding_t *pwm_binding(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if ((pwm == NULL) || (runtime->device == NULL)) return NULL;
  return (esy_pwm_device_binding_t *)runtime->device->driver_ctx;
}

static void pwm_set_output_state(esy_pwm_t *pwm, bool enabled) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  runtime->output_enabled = enabled;
  esy_pwm_device_binding_t *binding = pwm_binding(pwm);
  if (binding != NULL) binding->output_enabled = enabled;
}

static void pwm_release_dma(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_pwm_device_binding_t *binding = pwm_binding(pwm);
  runtime->dma_active = false;
  runtime->dma_circular = false;
  runtime->dma_complete_cb = NULL;
  runtime->dma_complete_arg = NULL;
  runtime->dma_half_cb = NULL;
  runtime->dma_half_arg = NULL;
  runtime->dma_error_cb = NULL;
  runtime->dma_error_arg = NULL;
  if (binding != NULL && binding->dma_ref_held) {
    binding->dma_ref_held = false;
    pwm_release(pwm);
  }
}

static void pwm_dma_complete(void *arg) {
  esy_pwm_t *pwm = arg;
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (pwm == NULL || !runtime->dma_active) return;
  runtime->callback_running = true;
  if (runtime->dma_complete_cb != NULL)
    runtime->dma_complete_cb(runtime->dma_complete_arg);
  runtime->callback_running = false;
  if (!runtime->dma_circular) pwm_release_dma(pwm);
}

static void pwm_dma_half(void *arg) {
  esy_pwm_t *pwm = arg;
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (pwm == NULL || !runtime->dma_active) return;
  runtime->callback_running = true;
  if (runtime->dma_half_cb != NULL) runtime->dma_half_cb(runtime->dma_half_arg);
  runtime->callback_running = false;
}

static void pwm_dma_error(void *arg) {
  esy_pwm_t *pwm = arg;
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (pwm == NULL || !runtime->dma_active) return;
  pwm_set_output_state(pwm, false);
  runtime->callback_running = true;
  if (runtime->dma_error_cb != NULL) runtime->dma_error_cb(runtime->dma_error_arg);
  runtime->callback_running = false;
  pwm_release_dma(pwm);
}

/* ================= vt_*: sole ops implementation =================
 * Notes:
 * - HAL public APIs call the shared PWM operations table; board and index
 *   completion stay in the vt_* functions.
 * - vt_* functions perform required board work and HAL backend forwarding.
 */

/* -------- main controls -------- */

static esy_err_t vt_set_duty(esy_pwm_t *pwm, float duty_cycle) {
  return esy_hb_pwm_set_duty(pwm, duty_cycle);
}

static esy_err_t vt_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz) {
  return esy_hb_pwm_set_wrap(pwm, freq_hz);
}

static esy_err_t vt_enable(esy_pwm_t *pwm, bool enable) {
  return esy_hb_pwm_enable(pwm, enable);
}

static esy_err_t vt_set_polarity(esy_pwm_t *pwm, bool start_low) {
  return esy_hb_pwm_set_polarity(pwm, start_low);
}

/* -------- trigger, interrupt, DMA, and advanced features -------- */

static esy_err_t vt_enable_trigger_output(esy_pwm_t *pwm, bool enabled) {
  return esy_hb_pwm_enable_trigger_output(pwm, enabled);
}

/* Resolve phys_index and complete board DMA metadata inside the vt function */
static esy_err_t vt_dma_output(esy_pwm_t *pwm, esy_dma_config_t *dma_config,
                               bool loop) {
  if (!pwm || !dma_config) return ESY_ERR_INVALID_ARG;

  uint8_t idx;
  esy_err_t r = esy_hb_pwm_get_phys_index(pwm, &idx);
  if (r != ESY_OK) return r;

  if (!esy_board_pwm_get_dma_info(idx, &dma_config->inner))
    return ESY_ERR_NOT_SUPPORTED;
  return esy_hb_pwm_dma_output(pwm, dma_config, loop);
}

/* Resolve phys_index and read IRQ priority from the board inside the vt
 * function */
static esy_err_t vt_set_wrap_handler(esy_pwm_t *pwm, void (*handler)(void *),
                                     void *arg) {
  if (!pwm) return ESY_ERR_INVALID_ARG;

  uint8_t idx;
  esy_err_t r = esy_hb_pwm_get_phys_index(pwm, &idx);
  if (r != ESY_OK) return r;

  uint8_t prio = esy_board_pwm_get_irq_priority(idx);
  return esy_hb_pwm_set_wrap_handler(pwm, handler, arg, prio);
}

/* -------- low power and clock -------- */

static esy_err_t vt_suspend(esy_pwm_t *pwm) {
  return esy_hb_pwm_suspend(pwm);
}

static esy_err_t vt_resume(esy_pwm_t *pwm) {
  return esy_hb_pwm_resume(pwm);
}

static esy_err_t vt_set_low_power_mode(esy_pwm_t *pwm, bool enable_lprun) {
  return esy_hb_pwm_set_low_power_mode(pwm, enable_lprun);
}

static esy_err_t vt_set_clock_source(esy_pwm_t *pwm, uint8_t clock_source) {
  return esy_hb_pwm_set_clock_source(pwm, clock_source);
}

/* -------- queries -------- */

static esy_err_t vt_get_caps(const esy_pwm_t *pwm, uint32_t *out_caps) {
  return esy_hb_pwm_get_caps(pwm, out_caps);
}

static esy_err_t vt_get_phys_index(const esy_pwm_t *pwm, uint8_t *out_index) {
  return esy_hb_pwm_get_phys_index(pwm, out_index);
}

/* ================= helpers ================= */

static const esy_pwm_vtable_t k_pwm_ops = {
    .set_duty = vt_set_duty,
    .set_wrap = vt_set_wrap,
    .enable = vt_enable,
    .set_polarity = vt_set_polarity,
    .enable_trigger_output = vt_enable_trigger_output,
    .dma_output = vt_dma_output,
    .abort_dma = esy_hb_pwm_abort_dma,
    .poll = esy_hb_pwm_poll,
    .set_wrap_handler = vt_set_wrap_handler,
    .suspend = vt_suspend,
    .resume = vt_resume,
    .set_low_power_mode = vt_set_low_power_mode,
    .set_clock_source = vt_set_clock_source,
    .get_caps = vt_get_caps,
    .get_phys_index = vt_get_phys_index,
};

static bool pwm_dma_is_busy(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_dma_config_t dma = {0};
  uint8_t index;

  if (runtime->dma_active || runtime->callback_running) return true;

  if (esy_hb_pwm_get_phys_index(pwm, &index) != ESY_OK) return false;
  if (!esy_board_pwm_get_dma_info(index, &dma.inner)) return false;
  return esy_hb_dma_is_busy(dma.inner.dreq, dma.inner.channel);
}

static esy_err_t pwm_device_pm_action(esy_device_t *device,
                                      esy_device_pm_action_t action,
                                      esy_power_state_t system_state) {
  esy_pwm_device_binding_t *binding;
  esy_err_t ret;

  (void)system_state;
  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_pwm_device_binding_t *)device->driver_ctx;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (binding->suspended) return ESY_ERR_STATE;
      if (pwm_dma_is_busy(binding->pwm)) return ESY_ERR_BUSY;
      ret = vt_suspend(binding->pwm);
      if (ret != ESY_OK) return ret;
      binding->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!binding->suspended) return ESY_ERR_STATE;
      ret = vt_resume(binding->pwm);
      if (ret != ESY_OK) return ret;
      binding->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static bool pwm_device_is_runtime_idle(const esy_device_t *device) {
  const esy_pwm_device_binding_t *binding;

  if ((device == NULL) || (device->driver_ctx == NULL)) return false;
  binding = device->driver_ctx;
  return !binding->output_enabled &&
         !binding->dma_ref_held && !pwm_dma_is_busy(binding->pwm);
}

static const esy_device_ops_t k_pwm_device_ops = {
    .pm_action = pwm_device_pm_action,
    .is_runtime_idle = pwm_device_is_runtime_idle,
};

/* ================= defaults ================= */

extern void esy_hal_pwm_default_cfg(esy_pwm_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->freq_hz = 1000u;
  config->duty_cycle = 50u;
}

/* ================= lifecycle ================= */

extern esy_err_t esy_hal_pwm_init(esy_pwm_t *pwm,
                                       const esy_pwm_config_t *config) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (!pwm || !config) return ESY_ERR_INVALID_ARG;
  if (runtime->device != NULL || pwm->ops != NULL)
    return ESY_ERR_STATE;
  if (config->freq_hz == 0u || config->duty_cycle > 100u)
    return ESY_ERR_INVALID_ARG;
  /* Reject a live logical slot before board mux/reset can affect its owner. */
  esy_err_t ret = esy_hb_pwm_check_available(config->phys_index);
  if (ret != ESY_OK) return ret;

  esy_pwm_config_t local = *config;
  config = &local;

  /* Configure board pin mux and related resources */
  esy_board_pwm_configure_pin(config->phys_index,
                              &((esy_pwm_config_t *)config)->inner);

  /* Bind vt_* functions to ops once */
  pwm->ops = &k_pwm_ops;

  /* Call HAL backend init/deinit directly without routing through ops */
  ret = esy_hb_pwm_init(pwm, config);
  if (ret != ESY_OK) {
    esy_err_t cleanup = esy_hb_pwm_deinit(pwm);
    if (cleanup != ESY_OK) return cleanup;
    esy_board_pwm_reset_pin(config->phys_index);
    memset(pwm, 0, sizeof(*pwm));
  }
  return ret;
}

extern esy_err_t esy_hal_pwm_deinit(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (pwm == NULL) return ESY_ERR_INVALID_ARG;
  if (runtime->device != NULL) return ESY_ERR_STATE;
  if (runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL && pwm->ctx == NULL) return ESY_OK;
  esy_err_t ret = esy_hal_pwm_abort_dma(pwm);
  if (ret != ESY_OK) return ret;
  uint8_t index;
  ret = esy_hb_pwm_get_phys_index(pwm, &index);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_pwm_deinit(pwm);
  if (ret != ESY_OK) return ret;
  esy_board_pwm_reset_pin(index);
  memset(pwm, 0, sizeof(*pwm));
  return ESY_OK;
}

esy_err_t esy_hal_pwm_register_device(
    esy_pwm_device_binding_t *binding, esy_pwm_t *pwm,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (pwm == NULL) || (registry == NULL) ||
      (device_config == NULL) || (runtime->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->pwm != NULL) return ESY_ERR_STATE;
  if (pwm_dma_is_busy(pwm)) return ESY_ERR_BUSY;
  memset(binding, 0, sizeof(*binding));
  binding->pwm = pwm;
  binding->output_enabled = runtime->output_enabled;
  config.name = device_config->name;
  config.ops = &k_pwm_device_ops;
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

esy_err_t esy_hal_pwm_unregister_device(
    esy_pwm_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->pwm == NULL) ||
      (esy_hal_pwm_runtime(binding->pwm)->device != &binding->device)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->dma_ref_held || pwm_dma_is_busy(binding->pwm))
    return ESY_ERR_BUSY;
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    esy_hal_pwm_runtime(binding->pwm)->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

/* ================= main control wrappers: validate and forward to ops
 * ================= */

extern esy_err_t esy_hal_pwm_set_duty(esy_pwm_t *pwm, float duty_cycle) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_err_t ret;

  if (!pwm) return ESY_ERR_INVALID_ARG;
  /* HAL API contract: 0.0 to 1.0 */
  if (!(duty_cycle >= 0.0f && duty_cycle <= 1.0f)) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->set_duty == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  if (!runtime->output_enabled) return ESY_ERR_STATE;
  ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  ret = pwm->ops->set_duty(pwm, duty_cycle);
  pwm_release(pwm);
  return ret;
}

extern esy_err_t esy_hal_pwm_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_err_t ret;

  if (!pwm || !freq_hz) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->set_wrap == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  ret = pwm->ops->set_wrap(pwm, freq_hz);
  pwm_release(pwm);
  return ret;
}

extern esy_err_t esy_hal_pwm_enable(esy_pwm_t *pwm, bool enable) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_err_t ret;

  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->enable == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  ret = pwm->ops->enable(pwm, enable);
  if (ret == ESY_OK) {
    runtime->output_enabled = enable;
    if (pwm_binding(pwm) != NULL) pwm_binding(pwm)->output_enabled = enable;
  }
  pwm_release(pwm);
  return ret;
}

extern esy_err_t esy_hal_pwm_set_polarity(esy_pwm_t *pwm, bool start_low) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_err_t ret;

  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->set_polarity == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  ret = pwm->ops->set_polarity(pwm, start_low);
  pwm_release(pwm);
  return ret;
}

extern esy_err_t esy_hal_pwm_enable_trigger_output(esy_pwm_t *pwm,
                                                        bool enabled) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_err_t ret;

  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->enable_trigger_output == NULL)
    return ESY_ERR_NOT_SUPPORTED;
  ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  ret = pwm->ops->enable_trigger_output(pwm, enabled);
  pwm_release(pwm);
  return ret;
}

/* ================= DMA wrappers ================= */

extern esy_err_t esy_hal_pwm_dma_output(esy_pwm_t *pwm,
                                       esy_dma_config_t *dma_config,
                                       bool loop) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (pwm == NULL || dma_config == NULL) return ESY_ERR_INVALID_ARG;
  if (pwm->ops == NULL || pwm->ops->dma_output == NULL ||
      pwm->ops->abort_dma == NULL)
    return ESY_ERR_NOT_SUPPORTED;
  if (runtime->dma_active || runtime->callback_running || runtime->output_enabled)
    return ESY_ERR_BUSY;
  esy_err_t ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  esy_dma_config_t transfer = *dma_config;
  runtime->dma_active = true;
  runtime->dma_circular = loop || transfer.circular;
  runtime->dma_complete_cb = transfer.callback;
  runtime->dma_complete_arg = transfer.callback_arg;
  runtime->dma_half_cb = transfer.half_callback;
  runtime->dma_half_arg = transfer.half_callback_arg;
  runtime->dma_error_cb = transfer.error_callback;
  runtime->dma_error_arg = transfer.error_callback_arg;
  esy_pwm_device_binding_t *binding = pwm_binding(pwm);
  if (binding != NULL) binding->dma_ref_held = true;
  transfer.circular = runtime->dma_circular;
  transfer.callback = pwm_dma_complete;
  transfer.callback_arg = pwm;
  transfer.half_callback = pwm_dma_half;
  transfer.half_callback_arg = pwm;
  transfer.error_callback = pwm_dma_error;
  transfer.error_callback_arg = pwm;
  /* Publish before submission: a provider may finish synchronously. */
  pwm_set_output_state(pwm, true);
  ret = pwm->ops->dma_output(pwm, &transfer, runtime->dma_circular);
  if (ret != ESY_OK) {
    esy_err_t cleanup = pwm->ops->abort_dma(pwm);
    if (cleanup != ESY_OK) return cleanup;
    pwm_set_output_state(pwm, false);
    pwm_release_dma(pwm);
  }
  return ret;
}

extern esy_err_t esy_hal_pwm_abort_dma(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (pwm == NULL) return ESY_ERR_INVALID_ARG;
  if (runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->abort_dma == NULL) return ESY_ERR_NOT_SUPPORTED;
  esy_err_t ret = pwm->ops->abort_dma(pwm);
  if (ret != ESY_OK) return ret;
  if (runtime->dma_active) {
    pwm_set_output_state(pwm, false);
    pwm_release_dma(pwm);
  }
  return ESY_OK;
}

extern esy_err_t esy_hal_pwm_poll(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (pwm == NULL) return ESY_ERR_INVALID_ARG;
  if (runtime->callback_running) return ESY_ERR_BUSY;
  return pwm->ops != NULL && pwm->ops->poll != NULL
             ? pwm->ops->poll(pwm) : ESY_ERR_NOT_SUPPORTED;
}

/* ================= IRQ and power wrappers ================= */

extern esy_err_t esy_hal_pwm_set_wrap_handler(esy_pwm_t *pwm,
                                                   void (*handler)(void *),
                                                   void *arg) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_err_t ret;

  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->set_wrap_handler == NULL)
    return ESY_ERR_NOT_IMPLEMENTED;
  /* The vt function resolves phys_index and board IRQ priority internally */
  ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  ret = pwm->ops->set_wrap_handler(pwm, handler, arg);
  pwm_release(pwm);
  return ret;
}

extern esy_err_t esy_hal_pwm_suspend(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (runtime->device != NULL) return ESY_ERR_STATE;
  if (pwm->ops == NULL || pwm->ops->suspend == NULL) return ESY_ERR_NOT_SUPPORTED;
  return pwm->ops->suspend(pwm);
}

extern esy_err_t esy_hal_pwm_resume(esy_pwm_t *pwm) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (runtime->device != NULL) return ESY_ERR_STATE;
  if (pwm->ops == NULL || pwm->ops->resume == NULL) return ESY_ERR_NOT_SUPPORTED;
  return pwm->ops->resume(pwm);
}

extern esy_err_t esy_hal_pwm_set_low_power_mode(esy_pwm_t *pwm,
                                                     bool enable_lprun) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_err_t ret;

  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->set_low_power_mode == NULL)
    return ESY_ERR_NOT_SUPPORTED;
  ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  ret = pwm->ops->set_low_power_mode(pwm, enable_lprun);
  pwm_release(pwm);
  return ret;
}

extern esy_err_t esy_hal_pwm_set_clock_source(esy_pwm_t *pwm,
                                                   uint8_t clock_source) {
  esy_hal_pwm_runtime_t *runtime = esy_hal_pwm_runtime(pwm);
  esy_err_t ret;

  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (runtime->dma_active || runtime->callback_running) return ESY_ERR_BUSY;
  if (pwm->ops == NULL || pwm->ops->set_clock_source == NULL)
    return ESY_ERR_NOT_SUPPORTED;
  ret = pwm_acquire(pwm);
  if (ret != ESY_OK) return ret;
  ret = pwm->ops->set_clock_source(pwm, clock_source);
  pwm_release(pwm);
  return ret;
}

/* ================= queries ================= */

extern esy_err_t esy_hal_pwm_get_caps(const esy_pwm_t *pwm,
                                           uint32_t *out_caps) {
  if (!pwm || !out_caps) return ESY_ERR_INVALID_ARG;
  if (pwm->ops == NULL || pwm->ops->get_caps == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return pwm->ops->get_caps(pwm, out_caps);
}

extern esy_err_t esy_hal_pwm_get_phys_index(const esy_pwm_t *pwm,
                                                 uint8_t *out_index) {
  if (!pwm || !out_index) return ESY_ERR_INVALID_ARG;
  if (pwm->ops == NULL || pwm->ops->get_phys_index == NULL)
    return ESY_ERR_NOT_IMPLEMENTED;
  return pwm->ops->get_phys_index(pwm, out_index);
}

/* ================= IRQ entry ================= */

extern void esy_hal_pwm_handle_irq(uint32_t index) {
  esy_hb_pwm_handle_irq(index);
}
