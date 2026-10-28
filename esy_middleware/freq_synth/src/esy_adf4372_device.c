/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_adf4372_device.h"
#include "esy_adf4372_internal.h"

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_gpio_pm.h"
#include "esy_hal/esy_hal_spi_device.h"

static esy_err_t adf4372_restore_pins(esy_freq_synth_device_binding_t *binding) {
  adf4372_ctx_t *ctx = binding->owner;
  esy_err_t ret = esy_hal_gpio_pm_select(binding->gpio_pm_profile, binding,
                                               ESY_GPIO_PM_STATE_DEFAULT);
  if (ret != ESY_OK) return ret;
  ret = esy_hal_gpio_write(ctx->cs_gpio, true);
  if (ret != ESY_OK) return ret;
  return ctx->ce_gpio != NULL ? esy_hal_gpio_write(ctx->ce_gpio, true) : ESY_OK;
}

static esy_err_t adf4372_device_transition(
    esy_freq_synth_device_binding_t *binding, esy_device_pm_action_t action,
    esy_power_state_t system_state) {
  adf4372_ctx_t *ctx = (adf4372_ctx_t *)binding->owner;
  esy_err_t ret;

  if (ctx == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  (void)system_state;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (ctx->suspended) {
        return ESY_ERR_STATE;
      }
      ctx->resume_main_enabled = ctx->main_enabled;
      ctx->resume_aux_enabled = ctx->aux_enabled;
      ctx->pm_requester = &binding->device;
      if (ctx->main_enabled) {
        ret = esy_adf4372_set_output_enable_internal(ctx, ESY_FREQ_SYNTH_PORT_MAIN, false);
        if (ret != ESY_OK) goto suspend_rollback;
      }
      if (ctx->aux_enabled) {
        ret = esy_adf4372_set_output_enable_internal(ctx, ESY_FREQ_SYNTH_PORT_AUX, false);
        if (ret != ESY_OK) goto suspend_rollback;
      }
      ret = esy_hal_gpio_pm_select(
          binding->gpio_pm_profile, binding,
          action == ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND
              ? ESY_GPIO_PM_STATE_IDLE
              : ESY_GPIO_PM_STATE_SLEEP);
      if (ret != ESY_OK) goto suspend_rollback;
      ctx->suspended = true;
      ctx->pm_requester = NULL;
      return ESY_OK;

    suspend_rollback: {
      esy_err_t restore = adf4372_restore_pins(binding);
      if (restore == ESY_OK) {
        const esy_err_t main_ret = esy_adf4372_set_output_enable_internal(
            ctx, ESY_FREQ_SYNTH_PORT_MAIN, ctx->resume_main_enabled);
        const esy_err_t aux_ret = esy_adf4372_set_output_enable_internal(
            ctx, ESY_FREQ_SYNTH_PORT_AUX, ctx->resume_aux_enabled);
        if (main_ret != ESY_OK || aux_ret != ESY_OK) restore = ESY_ERR_STATE;
      }
      ctx->pm_requester = NULL;
      return restore == ESY_OK ? ret : ESY_ERR_STATE;
    }

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!ctx->suspended) {
        return ESY_ERR_STATE;
      }
      ctx->pm_requester = &binding->device;
      ret = adf4372_restore_pins(binding);
      if (ret != ESY_OK) goto resume_failed;
      if (ctx->resume_main_enabled) {
        ret = esy_adf4372_set_output_enable_internal(ctx, ESY_FREQ_SYNTH_PORT_MAIN, true);
        if (ret != ESY_OK) goto resume_failed;
      }
      if (ctx->resume_aux_enabled) {
        ret = esy_adf4372_set_output_enable_internal(ctx, ESY_FREQ_SYNTH_PORT_AUX, true);
        if (ret != ESY_OK) goto resume_failed;
      }
      ctx->suspended = false;
      ctx->pm_requester = NULL;
      return ESY_OK;

    resume_failed:
      ctx->pm_requester = NULL;
      esy_freq_synth_runtime(binding->synth)->faulted = true;
      return ret;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static bool adf4372_device_is_runtime_idle(
    const esy_freq_synth_device_binding_t *binding) {
  const adf4372_ctx_t *ctx = binding->owner;
  return (ctx != NULL) && !ctx->main_enabled && !ctx->aux_enabled;
}

esy_err_t esy_adf4372_register_device(
    esy_freq_synth_device_binding_t *binding, esy_freq_synth_t *synth,
    esy_device_registry_t *registry,
    const esy_freq_synth_device_config_t *device_config) {
  adf4372_ctx_t *ctx = esy_adf4372_get_context_internal(synth);
  bool has_spi_dependency = false;

  if ((ctx == NULL) || (device_config == NULL) ||
      (device_config->registration.dependency_count != 0u &&
       device_config->registration.dependencies == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((ctx->spi == NULL) || (esy_hal_spi_get_device(ctx->spi) == NULL) ||
      (ctx->spi->ops == NULL || ctx->spi->ops->write_blocking == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0u; i < device_config->registration.dependency_count;
       i++) {
    if (device_config->registration.dependencies[i] == esy_hal_spi_get_device(ctx->spi)) {
      has_spi_dependency = true;
      break;
    }
  }
  if (!has_spi_dependency) {
    return ESY_ERR_INVALID_ARG;
  }
  return esy_freq_synth_register_device(
      binding, synth, registry, device_config, adf4372_device_transition,
      adf4372_device_is_runtime_idle, ctx);
}
