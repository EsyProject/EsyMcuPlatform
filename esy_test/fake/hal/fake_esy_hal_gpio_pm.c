/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_gpio_pm.h"

#include "fake_esy_hal_gpio_pm.h"
#include "fake_esy_hal_gpio_internal.h"

typedef struct {
  esy_err_t release_result;
  bool release_failure_armed;
} fake_esy_hal_gpio_pm_ctx_t;

static fake_esy_hal_gpio_pm_ctx_t s_ctx;

void fake_esy_hal_gpio_pm_reset(void) {
  s_ctx.release_result = ESY_OK;
  s_ctx.release_failure_armed = false;
}

void fake_esy_hal_gpio_pm_fail_release_once(esy_err_t result) {
  s_ctx.release_result = result;
  s_ctx.release_failure_armed = result != ESY_OK;
}

static esy_gpio_state_t fake_gpio_pm_state(
    const esy_gpio_pm_pin_t *pin, esy_gpio_pm_state_id_t state_id) {
  if (state_id == ESY_GPIO_PM_STATE_IDLE && pin->idle_state != NULL) {
    return *pin->idle_state;
  }
  if (state_id == ESY_GPIO_PM_STATE_SLEEP && pin->sleep_state != NULL) {
    return *pin->sleep_state;
  }
  const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(pin->gpio, false);
  return slot == NULL ? (esy_gpio_state_t){0} :
      (esy_gpio_state_t){slot->config.mode, slot->config.output_value};
}

esy_err_t esy_hal_gpio_pm_claim(
    const esy_device_gpio_pm_profile_t *profile, const void *owner) {
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  if (profile == NULL) return ESY_OK;
  if ((profile->pin_count != 0u) && (profile->pins == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    esy_gpio_t *gpio = profile->pins[i].gpio;

    const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_INVALID_ARG;
    if (slot->pm_owner != NULL) return ESY_ERR_BUSY;
  }
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    fake_esy_hal_gpio_slot(profile->pins[i].gpio, false)->pm_owner = owner;
  }
  return ESY_OK;
}

esy_err_t esy_hal_gpio_pm_release(
    const esy_device_gpio_pm_profile_t *profile, const void *owner) {
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  if (profile == NULL) return ESY_OK;
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    const fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(profile->pins[i].gpio, false);
    if (slot == NULL || slot->pm_owner != owner ||
        slot->pm_state != ESY_GPIO_PM_STATE_DEFAULT) {
      return ESY_ERR_STATE;
    }
  }
  if (s_ctx.release_failure_armed) {
    s_ctx.release_failure_armed = false;
    return s_ctx.release_result;
  }
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    fake_esy_hal_gpio_slot(profile->pins[i].gpio, false)->pm_owner = NULL;
  }
  return ESY_OK;
}

esy_err_t esy_hal_gpio_pm_select(
    const esy_device_gpio_pm_profile_t *profile, const void *owner,
    esy_gpio_pm_state_id_t state_id) {
  if ((owner == NULL) ||
      ((uint32_t)state_id >= (uint32_t)ESY_GPIO_PM_STATE_COUNT)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (profile == NULL) return ESY_OK;
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    esy_gpio_t *gpio = profile->pins[i].gpio;

    fake_gpio_slot_t *slot = fake_esy_hal_gpio_slot(gpio, false);
    if (slot == NULL || slot->pm_owner != owner) return ESY_ERR_STATE;
    const esy_gpio_state_t state = fake_gpio_pm_state(&profile->pins[i], state_id);
    esy_err_t ret = fake_esy_hal_gpio_apply_owned_state(gpio, &state);
    if (ret != ESY_OK) return ret;
    slot->pm_state = state_id;
  }
  return ESY_OK;
}
