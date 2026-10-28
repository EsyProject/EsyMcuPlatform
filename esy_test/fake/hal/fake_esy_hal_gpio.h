/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_hal_gpio.h
 * @brief Fake HAL GPIO contract for unit tests.
 */

#ifndef FAKE_ESY_HAL_GPIO_H
#define FAKE_ESY_HAL_GPIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_hal/esy_hal_gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Query whether a pin has been written.
 */
bool fake_esy_hal_gpio_has_written(uint8_t pin);

/**
 * @brief Get the last written level for a pin.
 *
 * Returns false when the pin has not been written.
 */
bool fake_esy_hal_gpio_get_written_value(uint8_t pin);

/**
 * @brief Set the value returned when a pin is read.
 */
void fake_esy_hal_gpio_set_read_value_for_pin(uint8_t pin, bool value);

/** @brief Get the pull configuration used by the most recent IRQ attachment. */
esy_gpio_pull_t fake_esy_hal_gpio_get_last_irq_pull(void);

/**
 * @brief Reset all fake GPIO state.
 *
 * - All pins are marked as unwritten.
 * - All read values are reset to false.
 */
void fake_esy_hal_gpio_reset(void);

/**
 * @brief Inject one write result after the specified number of calls.
 * @param successful_calls Calls to leave unchanged before injecting once.
 * @param result Injected error or byte count (SPI permits short transfers).
 * Reset cancels pending injections. Test-thread only; externally serialize.
 */
void fake_esy_hal_gpio_fail_write_after(size_t successful_calls, esy_err_t result);

/** @brief Inject a read result; ESY_OK restores normal pin reads. */
void fake_esy_hal_gpio_set_read_result(esy_err_t result);
/** @brief Fail profile application after a call count, optionally persistently. */
void fake_esy_hal_gpio_fail_apply_after(size_t calls, esy_err_t result, bool persistent);

/** @brief Observe the fake provider's current electrical state; unknown is zero. */
esy_gpio_state_t fake_esy_hal_gpio_get_current_state(const esy_gpio_t *gpio);
/** @brief Observe the fake PM claim owner; NULL means unclaimed/unknown. */
const void *fake_esy_hal_gpio_get_pm_owner(const esy_gpio_t *gpio);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_GPIO_H */
