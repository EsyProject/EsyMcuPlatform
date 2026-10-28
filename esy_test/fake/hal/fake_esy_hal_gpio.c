/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_gpio_internal.h"

#include <string.h>

#ifndef FAKE_ESY_HAL_GPIO_MAX_PINS
#define FAKE_ESY_HAL_GPIO_MAX_PINS 256u
#endif

typedef struct {
  fake_gpio_slot_t slots[FAKE_ESY_HAL_GPIO_MAX_PINS];
  bool pin_written_value[FAKE_ESY_HAL_GPIO_MAX_PINS];
  bool pin_has_written[FAKE_ESY_HAL_GPIO_MAX_PINS];
  bool pin_read_value[FAKE_ESY_HAL_GPIO_MAX_PINS];
  esy_gpio_pull_t last_irq_pull;
  esy_err_t read_result;
  bool apply_fault_armed;
  bool apply_fault_persistent;
  size_t apply_skip;
  esy_err_t apply_fault;
  bool write_fault_armed;
  size_t write_skip;
  int32_t write_fault;
} fake_esy_hal_gpio_ctx_t;

static fake_esy_hal_gpio_ctx_t s_ctx = {0};

fake_gpio_slot_t *fake_esy_hal_gpio_slot(const esy_gpio_t *gpio, bool create) {
  if (gpio == NULL) return NULL;
  fake_gpio_slot_t *free_slot = NULL;
  for (size_t i = 0u; i < FAKE_ESY_HAL_GPIO_MAX_PINS; ++i) {
    fake_gpio_slot_t *slot = &s_ctx.slots[i];
    if (slot->used && slot->handle == gpio) return slot;
    if (!slot->used && free_slot == NULL) free_slot = slot;
  }
  if (!create || free_slot == NULL) return NULL;
  *free_slot = (fake_gpio_slot_t){.handle = gpio, .used = true};
  return free_slot;
}

static bool gpio_is_valid(const esy_gpio_t *gpio) {
  const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  return slot != NULL && slot->initialized;
}

bool esy_hal_gpio_is_initialized(const esy_gpio_t *gpio) {
  return gpio_is_valid(gpio);
}

esy_err_t esy_hal_gpio_get_config(const esy_gpio_t *gpio, esy_gpio_config_t *config) {
  if (gpio == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
  const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  if (slot == NULL || !slot->initialized) return ESY_ERR_STATE;
  *config = slot->config;
  return ESY_OK;
}

static void fake_esy_hal_gpio_init_arrays(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.last_irq_pull = ESY_GPIO_PULL_NONE;
}

void esy_hal_gpio_default_cfg(esy_gpio_config_t *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->pin = ESY_PIN_UNUSED;
  config->mode = ESY_GPIO_MODE_INPUT;
}

esy_err_t esy_hal_gpio_init(esy_gpio_t *gpio, const esy_gpio_config_t *config) {
  if (gpio == NULL || config == NULL || config->pin == ESY_PIN_UNUSED)
    return ESY_ERR_INVALID_ARG;
#if FAKE_ESY_HAL_GPIO_MAX_PINS < 256u
  if (config->pin >= FAKE_ESY_HAL_GPIO_MAX_PINS) return ESY_ERR_INVALID_ARG;
#endif
  fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, true);
  if (slot == NULL) return ESY_ERR_NO_MEM;
  if (slot->initialized) return ESY_ERR_STATE;
  slot->config = *config;
  slot->current_state = (esy_gpio_state_t){config->mode, config->output_value};
  slot->initialized = true;
  memset(gpio, 0, sizeof(*gpio));
  if (config->mode == ESY_GPIO_MODE_OUTPUT)
    return esy_hal_gpio_write(gpio, config->output_value);
  return ESY_OK;
}

esy_err_t esy_hal_gpio_deinit(esy_gpio_t *gpio) {
  fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  if (slot == NULL || !slot->initialized) return ESY_ERR_INVALID_ARG;
  if (slot->pm_owner != NULL) return ESY_ERR_BUSY;
  memset(slot, 0, sizeof(*slot));
  memset(gpio, 0, sizeof(*gpio));
  return ESY_OK;
}

esy_err_t esy_hal_gpio_write(esy_gpio_t *gpio, bool value) {
  if (!gpio_is_valid(gpio)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (s_ctx.write_fault_armed && s_ctx.write_skip-- == 0u) {
    s_ctx.write_fault_armed = false;
    return (esy_err_t)s_ctx.write_fault;
  }
  fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  if (slot->pm_state != ESY_GPIO_PM_STATE_DEFAULT) return ESY_ERR_STATE;
  s_ctx.pin_written_value[slot->config.pin] = value;
  s_ctx.pin_has_written[slot->config.pin] = true;
  slot->current_state.output_value = value;
  return ESY_OK;
}

esy_err_t esy_hal_gpio_read(const esy_gpio_t *gpio, bool *out_value) {
  if (!gpio_is_valid(gpio) || (out_value == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (s_ctx.read_result != ESY_OK) return s_ctx.read_result;
  const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  if (slot->pm_state != ESY_GPIO_PM_STATE_DEFAULT) return ESY_ERR_STATE;
  *out_value = s_ctx.pin_read_value[slot->config.pin];
  return ESY_OK;
}

esy_err_t fake_esy_hal_gpio_apply_owned_state(esy_gpio_t *gpio,
                                        const esy_gpio_state_t *state) {
  if (!gpio_is_valid(gpio) || (state == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (s_ctx.apply_fault_armed) {
    if (s_ctx.apply_skip != 0u) --s_ctx.apply_skip;
    else {
      s_ctx.apply_fault_armed = s_ctx.apply_fault_persistent;
      return s_ctx.apply_fault;
    }
  }
  fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  if (state->mode == ESY_GPIO_MODE_OUTPUT) {
    if (s_ctx.write_fault_armed && s_ctx.write_skip-- == 0u) {
      s_ctx.write_fault_armed = false;
      return (esy_err_t)s_ctx.write_fault;
    }
    s_ctx.pin_written_value[slot->config.pin] = state->output_value;
    s_ctx.pin_has_written[slot->config.pin] = true;
  }
  slot->current_state = *state;
  return ESY_OK;
}

esy_err_t esy_hal_gpio_apply_state(esy_gpio_t *gpio, const esy_gpio_state_t *state) {
  const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  if (slot == NULL || !slot->initialized || state == NULL) return ESY_ERR_INVALID_ARG;
  if (slot->pm_state != ESY_GPIO_PM_STATE_DEFAULT) return ESY_ERR_STATE;
  return fake_esy_hal_gpio_apply_owned_state(gpio, state);
}

esy_err_t esy_hal_gpio_apply_configured_state(esy_gpio_t *gpio) {
  if (!gpio_is_valid(gpio)) {
    return ESY_ERR_INVALID_ARG;
  }
  const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  const esy_gpio_state_t configured = {slot->config.mode, slot->config.output_value};
  return esy_hal_gpio_apply_state(gpio, &configured);
}

esy_err_t esy_hal_gpio_attach_irq(esy_gpio_t *gpio,
                                  esy_gpio_irq_mode_t irq_mode,
                                  esy_gpio_pull_t pull,
                                  esy_gpio_irq_handler_t handler,
                                  void *arg) {
  (void)irq_mode;
  (void)arg;
  if (!gpio_is_valid(gpio) || (handler == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  s_ctx.last_irq_pull = pull;
  return ESY_OK;
}

esy_err_t esy_hal_gpio_enable_wakeup(esy_gpio_t *gpio,
                                          esy_gpio_irq_mode_t irq_mode,
                                          esy_gpio_irq_handler_t handler,
                                          void *arg) {
  esy_err_t ret = esy_hal_gpio_attach_irq(
      gpio, irq_mode, ESY_GPIO_PULL_NONE, handler, arg);

  if (ret == ESY_OK) {
    fake_esy_hal_gpio_slot(gpio, false)->wake_source = true;
  }
  return ret;
}

void esy_hal_gpio_handle_irq(uint8_t irq_group) { (void)irq_group; }

bool fake_esy_hal_gpio_has_written(uint8_t pin) {
  return s_ctx.pin_has_written[pin];
}

bool fake_esy_hal_gpio_get_written_value(uint8_t pin) {
  return s_ctx.pin_written_value[pin];
}

void fake_esy_hal_gpio_set_read_value_for_pin(uint8_t pin, bool value) {
  s_ctx.pin_read_value[pin] = value;
}

esy_gpio_pull_t fake_esy_hal_gpio_get_last_irq_pull(void) {
  return s_ctx.last_irq_pull;
}

void fake_esy_hal_gpio_reset(void) {
  fake_esy_hal_gpio_init_arrays();
}

__attribute__((constructor)) static void fake_esy_hal_gpio_ctor(void) {
  fake_esy_hal_gpio_init_arrays();
}

void fake_esy_hal_gpio_fail_write_after(size_t successful_calls, esy_err_t result) {
  s_ctx.write_fault_armed = true;
  s_ctx.write_skip = successful_calls;
  s_ctx.write_fault = result;
}

void fake_esy_hal_gpio_set_read_result(esy_err_t result) {
  s_ctx.read_result = result;
}

void fake_esy_hal_gpio_fail_apply_after(size_t calls, esy_err_t result, bool persistent) {
  s_ctx.apply_fault_armed = true;
  s_ctx.apply_fault_persistent = persistent;
  s_ctx.apply_skip = calls;
  s_ctx.apply_fault = result;
}

esy_gpio_state_t fake_esy_hal_gpio_get_current_state(const esy_gpio_t *gpio) {
  const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  return slot == NULL ? (esy_gpio_state_t){0} : slot->current_state;
}

const void *fake_esy_hal_gpio_get_pm_owner(const esy_gpio_t *gpio) {
  const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
  return slot == NULL ? NULL : slot->pm_owner;
}
