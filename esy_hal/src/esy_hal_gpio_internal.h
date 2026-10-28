/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_GPIO_INTERNAL_H
#define ESY_HAL_GPIO_INTERNAL_H

#include "esy_hal_storage_internal.h"

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include <stddef.h>

typedef struct ESY_HAL_MAY_ALIAS {
  const void *pm_owner;
  esy_gpio_state_t configured_state;
  esy_gpio_state_t current_state;
  esy_gpio_pm_state_id_t pm_state;
  uint8_t pin;
  bool initialized;
  bool wake_source;
} esy_hal_gpio_runtime_t;

_Static_assert(sizeof(esy_hal_gpio_runtime_t) <=
                   sizeof(((esy_gpio_t *)0)->hal.bytes),
               "GPIO HAL storage is too small");
_Static_assert(offsetof(esy_gpio_t, hal) % _Alignof(esy_hal_gpio_runtime_t) == 0u &&
                   _Alignof(esy_gpio_t) >= _Alignof(esy_hal_gpio_runtime_t),
               "GPIO HAL storage is misaligned");

static inline esy_hal_gpio_runtime_t *esy_hal_gpio_runtime(esy_gpio_t *gpio) {
  return (esy_hal_gpio_runtime_t *)(void *)gpio->hal.bytes;
}

static inline const esy_hal_gpio_runtime_t *esy_hal_gpio_runtime_const(
    const esy_gpio_t *gpio) {
  return (const esy_hal_gpio_runtime_t *)(const void *)gpio->hal.bytes;
}

/* Shared module ownership check; no hardware access. */
esy_err_t esy_hal_gpio_check_claim(uint8_t pin, const esy_gpio_t *gpio);

ESY_INTERNAL esy_err_t esy_hal_gpio_apply_pm_state(
    esy_gpio_t *gpio, const esy_gpio_state_t *state, const void *owner,
    esy_gpio_pm_state_id_t pm_state);

#endif /* ESY_HAL_GPIO_INTERNAL_H */
