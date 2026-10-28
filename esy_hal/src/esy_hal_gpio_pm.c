/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_gpio_pm.h"

#include "esy_hal_gpio_internal.h"

#define ESY_GPIO_PM_MAX_CLAIMED_PINS 64u

typedef struct {
  uint8_t pin;
  esy_gpio_t *gpio;
  const esy_device_gpio_pm_profile_t *profile;
  const void *owner;
} esy_gpio_pm_claim_t;

typedef struct {
  esy_gpio_pm_claim_t claims[ESY_GPIO_PM_MAX_CLAIMED_PINS];
} esy_hal_gpio_pm_ctx_t;

static esy_hal_gpio_pm_ctx_t s_ctx;

static esy_gpio_pm_claim_t *find_claim(uint8_t pin) {
  for (uint32_t i = 0u; i < ESY_GPIO_PM_MAX_CLAIMED_PINS; i++) {
    if ((s_ctx.claims[i].owner != NULL) && (s_ctx.claims[i].pin == pin)) {
      return &s_ctx.claims[i];
    }
  }
  return NULL;
}

esy_err_t esy_hal_gpio_check_claim(uint8_t pin, const esy_gpio_t *gpio) {
  const esy_gpio_pm_claim_t *claim = find_claim(pin);
  return claim != NULL && claim->gpio != gpio ? ESY_ERR_BUSY : ESY_OK;
}

static esy_gpio_pm_claim_t *find_free_claim(void) {
  for (uint32_t i = 0u; i < ESY_GPIO_PM_MAX_CLAIMED_PINS; i++) {
    if (s_ctx.claims[i].owner == NULL) return &s_ctx.claims[i];
  }
  return NULL;
}

static esy_err_t get_state(const esy_gpio_pm_pin_t *pin,
                            esy_gpio_pm_state_id_t state_id,
                            esy_gpio_state_t *state) {
  const esy_gpio_state_t *selected = NULL;

  switch (state_id) {
    case ESY_GPIO_PM_STATE_DEFAULT:
      break;
    case ESY_GPIO_PM_STATE_IDLE:
      selected = pin->idle_state;
      break;
    case ESY_GPIO_PM_STATE_SLEEP:
      selected = pin->sleep_state;
      break;
    default:
      return ESY_ERR_INVALID_ARG;
  }
  /* Do not expose a non-aliasing pointer into the private byte storage. */
  *state = selected != NULL ? *selected
      : esy_hal_gpio_runtime_const(pin->gpio)->configured_state;
  return ESY_OK;
}

static bool state_is_valid(const esy_gpio_state_t *state) {
  return (state != NULL) &&
         ((uint32_t)state->mode <= (uint32_t)ESY_GPIO_MODE_INPUT_PULLDOWN);
}

static esy_err_t validate_profile(
    const esy_device_gpio_pm_profile_t *profile) {
  if ((profile->pin_count != 0u) && (profile->pins == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    const esy_gpio_pm_pin_t *pin = &profile->pins[i];

    if ((pin->gpio == NULL) || !esy_hal_gpio_runtime(pin->gpio)->initialized) {
      return ESY_ERR_INVALID_ARG;
    }
    for (uint32_t id = 0u; id < (uint32_t)ESY_GPIO_PM_STATE_COUNT; ++id) {
      esy_gpio_state_t state;
      if (get_state(pin, (esy_gpio_pm_state_id_t)id, &state) != ESY_OK ||
          !state_is_valid(&state)) {
        return ESY_ERR_INVALID_ARG;
      }
    }
    for (uint32_t j = 0u; j < i; j++) {
      if (esy_hal_gpio_runtime(pin->gpio)->pin == esy_hal_gpio_runtime(profile->pins[j].gpio)->pin) {
        return ESY_ERR_INVALID_ARG;
      }
    }
  }
  return ESY_OK;
}

esy_err_t esy_hal_gpio_pm_claim(
    const esy_device_gpio_pm_profile_t *profile, const void *owner) {
  esy_err_t ret;

  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  if (profile == NULL) return ESY_OK;
  ret = validate_profile(profile);
  if (ret != ESY_OK) return ret;
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    if ((esy_hal_gpio_runtime(profile->pins[i].gpio)->pm_owner != NULL) ||
        (find_claim(esy_hal_gpio_runtime(profile->pins[i].gpio)->pin) != NULL)) {
      return ESY_ERR_BUSY;
    }
  }
  uint32_t free_count = 0u;
  for (uint32_t i = 0u; i < ESY_GPIO_PM_MAX_CLAIMED_PINS; i++) {
    if (s_ctx.claims[i].owner == NULL) free_count++;
  }
  if (free_count < profile->pin_count) return ESY_ERR_NO_MEM;
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    esy_gpio_pm_claim_t *claim = find_free_claim();
    esy_gpio_t *gpio = profile->pins[i].gpio;

    claim->pin = esy_hal_gpio_runtime(gpio)->pin;
    claim->gpio = gpio;
    claim->profile = profile;
    claim->owner = owner;
    esy_hal_gpio_runtime(gpio)->pm_owner = owner;
    esy_hal_gpio_runtime(gpio)->pm_state = ESY_GPIO_PM_STATE_DEFAULT;
  }
  return ESY_OK;
}

esy_err_t esy_hal_gpio_pm_release(
    const esy_device_gpio_pm_profile_t *profile, const void *owner) {
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  if (profile == NULL) return ESY_OK;
  if (validate_profile(profile) != ESY_OK) return ESY_ERR_INVALID_ARG;
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    const esy_gpio_t *gpio = profile->pins[i].gpio;
    const esy_gpio_pm_claim_t *claim = find_claim(esy_hal_gpio_runtime_const(gpio)->pin);

    if ((claim == NULL) || (claim->owner != owner) ||
        (claim->profile != profile) || (claim->gpio != gpio) ||
        (esy_hal_gpio_runtime_const(gpio)->pm_owner != owner) ||
        (esy_hal_gpio_runtime_const(gpio)->pm_state != ESY_GPIO_PM_STATE_DEFAULT)) {
      return ESY_ERR_STATE;
    }
  }
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    esy_gpio_t *gpio = profile->pins[i].gpio;
    esy_gpio_pm_claim_t *claim = find_claim(esy_hal_gpio_runtime(gpio)->pin);

    esy_hal_gpio_runtime(gpio)->pm_owner = NULL;
    claim->pin = ESY_PIN_UNUSED;
    claim->gpio = NULL;
    claim->profile = NULL;
    claim->owner = NULL;
  }
  return ESY_OK;
}

static esy_err_t apply_profile(
    const esy_device_gpio_pm_profile_t *profile, const void *owner,
    esy_gpio_pm_state_id_t state_id) {
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    esy_gpio_t *gpio = profile->pins[i].gpio;
    esy_gpio_state_t state;
    esy_err_t ret = get_state(&profile->pins[i], state_id, &state);
    if (ret != ESY_OK) return ret;
    ret = esy_hal_gpio_apply_pm_state(gpio, &state, owner, state_id);

    if (ret != ESY_OK) return ret;
  }
  return ESY_OK;
}

esy_err_t esy_hal_gpio_pm_select(
    const esy_device_gpio_pm_profile_t *profile, const void *owner,
    esy_gpio_pm_state_id_t state_id) {
  esy_err_t ret;

  if ((owner == NULL) ||
      ((uint32_t)state_id >= (uint32_t)ESY_GPIO_PM_STATE_COUNT)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (profile == NULL) return ESY_OK;
  ret = validate_profile(profile);
  if (ret != ESY_OK) return ret;
  for (uint32_t i = 0u; i < profile->pin_count; i++) {
    if (esy_hal_gpio_runtime(profile->pins[i].gpio)->pm_owner != owner) return ESY_ERR_STATE;
  }
  ret = apply_profile(profile, owner, state_id);
  if ((ret != ESY_OK) && (state_id != ESY_GPIO_PM_STATE_DEFAULT)) {
    (void)apply_profile(profile, owner, ESY_GPIO_PM_STATE_DEFAULT);
  }
  return ret;
}
