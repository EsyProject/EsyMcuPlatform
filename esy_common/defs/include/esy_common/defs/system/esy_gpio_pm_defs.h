/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_GPIO_PM_DEFS_H
#define ESY_GPIO_PM_DEFS_H

#include <stdint.h>

#include "esy_common/defs/peripheral/esy_gpio_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Optional low-power overrides for one initialized auxiliary GPIO.
 *
 * This type is for device-owned GPIO signals such as chip-select, reset, or
 * enable. It does not describe controller alternate-function multiplexing.
 */
typedef struct esy_gpio_pm_pin_s {
  esy_gpio_t *gpio; /**< Initialized caller-owned GPIO handle. */
  const esy_gpio_state_t *idle_state;  /**< Optional runtime-idle override. */
  const esy_gpio_state_t *sleep_state; /**< Optional system-sleep override. */
} esy_gpio_pm_pin_t;

/**
 * @brief GPIO-only power profile owned by one logical device adapter.
 *
 * The adapter must claim the profile for its complete registration lifetime.
 * Claiming rejects duplicate physical pin ownership, including aliases using
 * different GPIO handles. All referenced storage remains caller-owned.
 */
typedef struct esy_device_gpio_pm_profile_s {
  const esy_gpio_pm_pin_t *pins; /**< Caller-owned immutable pin policies. */
  uint32_t pin_count;            /**< Number of entries in @ref pins. */
} esy_device_gpio_pm_profile_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_GPIO_PM_DEFS_H */
