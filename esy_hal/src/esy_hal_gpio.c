/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal_gpio_internal.h"

#include <string.h>

#include "esy_board/esy_board.h"
#include "esy_hal_backend/esy_hb_gpio.h"

static bool mode_is_valid(esy_gpio_mode_t mode) {
  return (uint32_t)mode <= (uint32_t)ESY_GPIO_MODE_INPUT_PULLDOWN;
}

static esy_err_t apply_state(esy_gpio_t *gpio,
                             const esy_gpio_state_t *state) {
  esy_err_t ret = esy_hb_gpio_init(esy_hal_gpio_runtime(gpio)->pin, state->mode);

  if (ret != ESY_OK) return ret;
  if (state->mode == ESY_GPIO_MODE_OUTPUT) {
    esy_hb_gpio_write(esy_hal_gpio_runtime(gpio)->pin, state->output_value);
  }
  esy_hal_gpio_runtime(gpio)->current_state = *state;
  return ESY_OK;
}

esy_err_t esy_hal_gpio_apply_state(esy_gpio_t *gpio,
                                        const esy_gpio_state_t *state) {
  esy_err_t ret;

  if ((gpio == NULL) || !esy_hal_gpio_runtime(gpio)->initialized || (state == NULL) ||
      !mode_is_valid(state->mode)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (esy_hal_gpio_check_claim(esy_hal_gpio_runtime(gpio)->pin, gpio) != ESY_OK)
    return ESY_ERR_BUSY;
  if (esy_hal_gpio_runtime(gpio)->pm_state != ESY_GPIO_PM_STATE_DEFAULT) return ESY_ERR_STATE;
  ret = apply_state(gpio, state);
  return ret;
}

ESY_INTERNAL esy_err_t esy_hal_gpio_apply_pm_state(
    esy_gpio_t *gpio, const esy_gpio_state_t *state, const void *owner,
    esy_gpio_pm_state_id_t pm_state) {
  esy_err_t ret;

  if ((gpio == NULL) || !esy_hal_gpio_runtime(gpio)->initialized || (state == NULL) ||
      !mode_is_valid(state->mode) || (owner == NULL) ||
      (esy_hal_gpio_runtime(gpio)->pm_owner != owner) ||
      ((uint32_t)pm_state >= (uint32_t)ESY_GPIO_PM_STATE_COUNT)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = apply_state(gpio, state);
  if (ret == ESY_OK) esy_hal_gpio_runtime(gpio)->pm_state = pm_state;
  return ret;
}

void esy_hal_gpio_default_cfg(esy_gpio_config_t *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->pin = ESY_PIN_UNUSED;
  config->mode = ESY_GPIO_MODE_INPUT;
}

esy_err_t esy_hal_gpio_init(esy_gpio_t *gpio,
                                 const esy_gpio_config_t *config) {
  esy_err_t ret;

  if ((gpio == NULL) || (config == NULL) || (config->pin == ESY_PIN_UNUSED) ||
      !mode_is_valid(config->mode)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (esy_hal_gpio_check_claim(config->pin, gpio) != ESY_OK)
    return ESY_ERR_BUSY;
  if (esy_hal_gpio_runtime(gpio)->initialized) {
    return ESY_ERR_STATE;
  }

  memset(gpio, 0, sizeof(*gpio));
  esy_hal_gpio_runtime(gpio)->pin = config->pin;
  esy_hal_gpio_runtime(gpio)->configured_state.mode = config->mode;
  esy_hal_gpio_runtime(gpio)->configured_state.output_value = config->output_value;
  esy_hal_gpio_runtime(gpio)->initialized = true;

  const esy_gpio_state_t configured = esy_hal_gpio_runtime(gpio)->configured_state;
  ret = esy_hal_gpio_apply_state(gpio, &configured);
  if (ret != ESY_OK) {
    memset(gpio, 0, sizeof(*gpio));
  }
  return ret;
}

esy_err_t esy_hal_gpio_deinit(esy_gpio_t *gpio) {
  if ((gpio == NULL) || !esy_hal_gpio_runtime(gpio)->initialized) {
    return ESY_ERR_INVALID_ARG;
  }
  if (esy_hal_gpio_check_claim(esy_hal_gpio_runtime(gpio)->pin, gpio) != ESY_OK)
    return ESY_ERR_BUSY;
  if (esy_hal_gpio_runtime(gpio)->pm_owner != NULL) return ESY_ERR_BUSY;
  if (esy_hal_gpio_runtime(gpio)->wake_source) {
    esy_err_t ret = esy_hb_gpio_disable_wakeup(esy_hal_gpio_runtime(gpio)->pin);
    if (ret != ESY_OK) return ret;
  } else {
    esy_hb_gpio_disable_irq(esy_hal_gpio_runtime(gpio)->pin);
  }
  esy_hb_gpio_deinit(esy_hal_gpio_runtime(gpio)->pin);
  memset(gpio, 0, sizeof(*gpio));
  return ESY_OK;
}

esy_err_t esy_hal_gpio_write(esy_gpio_t *gpio, bool value) {
  if ((gpio == NULL) || !esy_hal_gpio_runtime(gpio)->initialized) return ESY_ERR_INVALID_ARG;
  if (esy_hal_gpio_check_claim(esy_hal_gpio_runtime(gpio)->pin, gpio) != ESY_OK)
    return ESY_ERR_BUSY;
  if ((esy_hal_gpio_runtime(gpio)->pm_state != ESY_GPIO_PM_STATE_DEFAULT) ||
      (esy_hal_gpio_runtime(gpio)->current_state.mode != ESY_GPIO_MODE_OUTPUT)) {
    return ESY_ERR_STATE;
  }
  esy_hb_gpio_write(esy_hal_gpio_runtime(gpio)->pin, value);
  esy_hal_gpio_runtime(gpio)->current_state.output_value = value;
  return ESY_OK;
}

esy_err_t esy_hal_gpio_read(const esy_gpio_t *gpio, bool *out_value) {
  if ((gpio == NULL) || !esy_hal_gpio_runtime_const(gpio)->initialized || (out_value == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (esy_hal_gpio_check_claim(esy_hal_gpio_runtime_const(gpio)->pin, gpio) != ESY_OK)
    return ESY_ERR_BUSY;
  if (esy_hal_gpio_runtime_const(gpio)->pm_state != ESY_GPIO_PM_STATE_DEFAULT) return ESY_ERR_STATE;
  *out_value = esy_hb_gpio_read(esy_hal_gpio_runtime_const(gpio)->pin);
  return ESY_OK;
}

esy_err_t esy_hal_gpio_apply_configured_state(esy_gpio_t *gpio) {
  if (gpio == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  const esy_gpio_state_t configured = esy_hal_gpio_runtime(gpio)->configured_state;
  return esy_hal_gpio_apply_state(gpio, &configured);
}

esy_err_t esy_hal_gpio_attach_irq(esy_gpio_t *gpio,
                                  esy_gpio_irq_mode_t irq_mode,
                                  esy_gpio_pull_t pull,
                                  esy_gpio_irq_handler_t handler,
                                  void *arg) {
  esy_gpio_irq_config_t config;

  if ((gpio == NULL) || !esy_hal_gpio_runtime(gpio)->initialized || (handler == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (esy_hal_gpio_check_claim(esy_hal_gpio_runtime(gpio)->pin, gpio) != ESY_OK)
    return ESY_ERR_BUSY;
  if (esy_hal_gpio_runtime(gpio)->pm_state != ESY_GPIO_PM_STATE_DEFAULT) return ESY_ERR_STATE;
  config.pin = esy_hal_gpio_runtime(gpio)->pin;
  config.irq_mode = irq_mode;
  config.handler = handler;
  config.arg = arg;
  config.priority = esy_board_gpio_get_irq_priority(esy_hal_gpio_runtime(gpio)->pin);
  config.pull = pull;
  return esy_hb_gpio_attach_irq(&config);
}

esy_err_t esy_hal_gpio_enable_wakeup(esy_gpio_t *gpio,
                                          esy_gpio_irq_mode_t irq_mode,
                                          esy_gpio_irq_handler_t handler,
                                          void *arg) {
  esy_gpio_irq_config_t config;
  esy_err_t ret;
  bool cleanup_required = false;

  if ((gpio == NULL) || !esy_hal_gpio_runtime(gpio)->initialized || (handler == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (esy_hal_gpio_check_claim(esy_hal_gpio_runtime(gpio)->pin, gpio) != ESY_OK)
    return ESY_ERR_BUSY;
  if (esy_hal_gpio_runtime(gpio)->pm_state != ESY_GPIO_PM_STATE_DEFAULT) return ESY_ERR_STATE;
  if (esy_hal_gpio_runtime(gpio)->wake_source) return ESY_ERR_STATE;
  config.pin = esy_hal_gpio_runtime(gpio)->pin;
  config.irq_mode = irq_mode;
  config.handler = handler;
  config.arg = arg;
  config.priority = esy_board_gpio_get_irq_priority(esy_hal_gpio_runtime(gpio)->pin);
  config.pull = ESY_GPIO_PULL_NONE;
  ret = esy_hb_gpio_enable_wakeup(&config, &cleanup_required);
  esy_hal_gpio_runtime(gpio)->wake_source = cleanup_required;
  return ret;
}

esy_err_t esy_hal_gpio_disable_wakeup(esy_gpio_t *gpio) {
  if ((gpio == NULL) || !esy_hal_gpio_runtime(gpio)->initialized) {
    return ESY_ERR_INVALID_ARG;
  }
  if (esy_hal_gpio_check_claim(esy_hal_gpio_runtime(gpio)->pin, gpio) != ESY_OK)
    return ESY_ERR_BUSY;
  if (esy_hal_gpio_runtime(gpio)->pm_state != ESY_GPIO_PM_STATE_DEFAULT) return ESY_ERR_STATE;
  if (!esy_hal_gpio_runtime(gpio)->wake_source) {
    return ESY_ERR_STATE;
  }
  esy_err_t ret = esy_hb_gpio_disable_wakeup(esy_hal_gpio_runtime(gpio)->pin);
  if (ret != ESY_OK) return ret;
  esy_hal_gpio_runtime(gpio)->wake_source = false;
  return ESY_OK;
}

void esy_hal_gpio_handle_irq(uint8_t irq_group) {
  esy_hb_gpio_handle_irq(irq_group);
}

bool esy_hal_gpio_is_initialized(const esy_gpio_t *gpio) {
  return gpio != NULL && esy_hal_gpio_runtime_const(gpio)->initialized;
}

esy_err_t esy_hal_gpio_get_config(const esy_gpio_t *gpio,
                                 esy_gpio_config_t *config) {
  if (gpio == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
  const esy_hal_gpio_runtime_t *state = esy_hal_gpio_runtime_const(gpio);
  if (!state->initialized) return ESY_ERR_STATE;
  *config = (esy_gpio_config_t){.pin = state->pin,
                               .mode = state->configured_state.mode,
                               .output_value = state->configured_state.output_value};
  return ESY_OK;
}
