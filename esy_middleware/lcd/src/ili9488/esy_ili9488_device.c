/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lcd/esy_ili9488_device.h"
#include "esy_ili9488_internal.h"
#include "esy_hal/esy_hal_gpio_pm.h"
#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_spi_device.h"

static esy_err_t ili9488_device_transition(esy_lcd_device_binding_t *binding,
                                           esy_device_pm_action_t action,
                                           esy_power_state_t system_state) {
  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)binding->owner;
  esy_err_t ret;

  if ((ctx == NULL) || (ctx->magic != ILI9488_CTX_MAGIC)) {
    return ESY_ERR_INVALID_ARG;
  }
  (void)system_state;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (ctx->suspended) return ESY_ERR_STATE;
      if (atomic_load_explicit(&ctx->state, memory_order_acquire) ==
          ILI9488_STATE_CLEANUP_FAILED) return ESY_ERR_STATE;
      if (atomic_load_explicit(&ctx->state, memory_order_acquire) !=
          ILI9488_STATE_IDLE) return ESY_ERR_BUSY;
      if (ctx->backlight_set_cb != NULL) {
        ret = ctx->backlight_set_cb(false, ctx->backlight_set_cb_arg);
        if (ret != ESY_OK) return ret;
      }
      ret = esy_hal_gpio_pm_select(
          binding->gpio_pm_profile, binding,
          action == ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND
              ? ESY_GPIO_PM_STATE_IDLE
              : ESY_GPIO_PM_STATE_SLEEP);
      if (ret != ESY_OK) {
        esy_err_t rollback = esy_hal_gpio_pm_select(
            binding->gpio_pm_profile, binding, ESY_GPIO_PM_STATE_DEFAULT);
        if (ctx->backlight_set_cb != NULL) {
          if (ctx->backlight_set_cb(ctx->backlight_enabled,
                                    ctx->backlight_set_cb_arg) != ESY_OK)
            rollback = ESY_ERR_STATE;
        }
        return rollback == ESY_OK ? ret : ESY_ERR_STATE;
      }
      ctx->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!ctx->suspended) return ESY_ERR_STATE;
      ret = esy_hal_gpio_pm_select(binding->gpio_pm_profile, binding,
                                        ESY_GPIO_PM_STATE_DEFAULT);
      if (ret != ESY_OK) return ret;
      if (ctx->backlight_set_cb != NULL) {
        ret = ctx->backlight_set_cb(ctx->backlight_enabled,
                                    ctx->backlight_set_cb_arg);
        if (ret != ESY_OK) return ret;
      }
      ctx->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

/* PM only preserves powered controller state; it cannot reinitialize a reset
 * panel while the scheduler is suspended during system resume. */
static esy_err_t validate_pm_profile(
    const esy_ili9488_ctx_t *ctx,
    const esy_device_gpio_pm_profile_t *profile) {
  if (profile == NULL) return ESY_OK;
  if (profile->pin_count != 0u && profile->pins == NULL)
    return ESY_ERR_INVALID_ARG;
  for (uint32_t i = 0u; i < profile->pin_count; ++i) {
    const esy_gpio_pm_pin_t *pin = &profile->pins[i];
    if (pin->gpio == NULL) return ESY_ERR_INVALID_ARG;
    if (pin->gpio != ctx->cs_gpio && pin->gpio != ctx->dc_gpio &&
        pin->gpio != ctx->rst_gpio) return ESY_ERR_NOT_SUPPORTED;
    if (pin->gpio != ctx->rst_gpio) continue;
    esy_gpio_config_t config;
    esy_err_t ret = esy_hal_gpio_get_config(pin->gpio, &config);
    if (ret != ESY_OK) return ret;
    const esy_gpio_state_t configured = {config.mode, config.output_value};
    const esy_gpio_state_t *states[] = {&configured, pin->idle_state, pin->sleep_state};
    for (uint32_t j = 0u; j < sizeof(states) / sizeof(states[0]); ++j) {
      if (states[j] != NULL &&
          (states[j]->mode != ESY_GPIO_MODE_OUTPUT || !states[j]->output_value))
        return ESY_ERR_NOT_SUPPORTED;
    }
  }
  return ESY_OK;
}

esy_err_t esy_ili9488_register_device(
    esy_lcd_device_binding_t *binding, esy_lcd_t *lcd,
    esy_device_registry_t *registry,
    const esy_lcd_device_config_t *device_config) {
  esy_ili9488_ctx_t *ctx;
  bool has_spi_dependency = false;

  if ((lcd == NULL) || (lcd->ctx == NULL) || (device_config == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  ctx = (esy_ili9488_ctx_t *)lcd->ctx;
  if ((ctx->magic != ILI9488_CTX_MAGIC) || (ctx->spi == NULL) ||
      (esy_hal_spi_get_device(ctx->spi) == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (device_config->registration.dependency_count != 0u &&
      device_config->registration.dependencies == NULL)
    return ESY_ERR_INVALID_ARG;
  esy_err_t profile_result =
      validate_pm_profile(ctx, device_config->gpio_pm_profile);
  if (profile_result != ESY_OK) return profile_result;
  for (uint32_t i = 0u; i < device_config->registration.dependency_count;
       i++) {
    if (device_config->registration.dependencies[i] == esy_hal_spi_get_device(ctx->spi)) {
      has_spi_dependency = true;
      break;
    }
  }
  if (!has_spi_dependency) return ESY_ERR_INVALID_ARG;
  return esy_lcd_register_device(binding, lcd, registry, device_config,
                                 ili9488_device_transition, ctx);
}
