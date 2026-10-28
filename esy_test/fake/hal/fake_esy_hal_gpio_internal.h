/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_GPIO_INTERNAL_H
#define FAKE_ESY_HAL_GPIO_INTERNAL_H
#include "fake_esy_hal_gpio.h"
#include "esy_hal/esy_hal_gpio_pm.h"

typedef struct {
  const esy_gpio_t *handle;
  bool used;
  bool initialized;
  bool wake_source;
  esy_gpio_config_t config;
  esy_gpio_state_t current_state;
  const void *pm_owner;
  esy_gpio_pm_state_id_t pm_state;
} fake_gpio_slot_t;

/* Private cooperation between the GPIO fake and its PM-profile adapter. */
fake_gpio_slot_t *fake_esy_hal_gpio_slot(const esy_gpio_t *gpio, bool create);
esy_err_t fake_esy_hal_gpio_apply_owned_state(esy_gpio_t *gpio,
                                               const esy_gpio_state_t *state);
#endif
