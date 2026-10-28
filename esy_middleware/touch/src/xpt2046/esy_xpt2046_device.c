/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/touch/esy_xpt2046_device.h"
#include "esy_xpt2046_internal.h"
#include "esy_hal/esy_hal_gpio_pm.h"

static esy_err_t xpt2046_device_transition(esy_touch_device_binding_t *binding,
                                           esy_device_pm_action_t action,
                                           esy_power_state_t system_state) {
  esy_xpt2046_ctx_t *ctx = (esy_xpt2046_ctx_t *)binding->owner;
  esy_err_t ret;

  (void)system_state;
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (ctx->suspended) return ESY_ERR_STATE;
      if (ctx->sample_state != XPT2046_SAMPLE_IDLE) return ESY_ERR_BUSY;
      ret = esy_hal_gpio_pm_select(
          binding->gpio_pm_profile, binding,
          action == ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND
              ? ESY_GPIO_PM_STATE_IDLE
              : ESY_GPIO_PM_STATE_SLEEP);
      if (ret != ESY_OK) {
        esy_err_t cleanup = esy_hal_gpio_pm_select(
            binding->gpio_pm_profile, binding, ESY_GPIO_PM_STATE_DEFAULT);
        return cleanup == ESY_OK ? ret : ESY_ERR_STATE;
      }
      ctx->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!ctx->suspended) return ESY_ERR_STATE;
      ret = esy_hal_gpio_pm_select(binding->gpio_pm_profile, binding,
                                        ESY_GPIO_PM_STATE_DEFAULT);
      if (ret != ESY_OK) return ret;
      ctx->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

esy_err_t esy_xpt2046_register_device(
    esy_touch_device_binding_t *binding, esy_touch_t *touch,
    esy_device_registry_t *registry,
    const esy_touch_device_config_t *device_config) {
  esy_xpt2046_ctx_t *ctx = esy_xpt2046_context(touch);
  bool has_spi_dependency = false;

  if ((ctx == NULL) || (device_config == NULL) ||
      ((device_config->registration.dependency_count != 0u) &&
       (device_config->registration.dependencies == NULL))) {
    return ESY_ERR_INVALID_ARG;
  }
  if (ctx->magic != XPT2046_CTX_MAGIC || ctx->spi_device == NULL)
    return ESY_ERR_INVALID_ARG;
  for (uint32_t i = 0u; i < device_config->registration.dependency_count;
       i++) {
    if (device_config->registration.dependencies[i] == ctx->spi_device) {
      has_spi_dependency = true;
      break;
    }
  }
  if (!has_spi_dependency) return ESY_ERR_INVALID_ARG;
  return esy_touch_register_device(binding, touch, registry, device_config,
                                   xpt2046_device_transition, ctx);
}
