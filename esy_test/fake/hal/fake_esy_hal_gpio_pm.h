/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_GPIO_PM_H
#define FAKE_ESY_HAL_GPIO_PM_H

#include "esy_hal/esy_hal_gpio_pm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Clear PM fault injection without changing any GPIO claim or state.
 * Call alongside fake_esy_hal_gpio_reset() with all fake callers quiescent. */
void fake_esy_hal_gpio_pm_reset(void);

/** @brief Reject the next otherwise valid profile release with this error.
 * The rejection retains every claim/owner and consumes the injection. ESY_OK
 * cancels an armed failure. NULL/invalid profile calls do not consume it.
 * Single-owner test setup only; reset also cancels an armed injection. */
void fake_esy_hal_gpio_pm_fail_release_once(esy_err_t result);

#ifdef __cplusplus
}
#endif
#endif /* FAKE_ESY_HAL_GPIO_PM_H */
