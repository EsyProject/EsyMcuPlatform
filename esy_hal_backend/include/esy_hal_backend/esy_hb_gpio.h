/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_gpio.h
 * @brief Low-level GPIO HAL backend interface.
 *
 * This header defines the platform-independent GPIO HAL backend API, including:
 * - Pin mode configuration (input/output/pull-up/pull-down)
 * - Digital read/write operations
 * - Interrupt registration, control, and wake-source setup
 *
 * Conventions:
 * - All functions return @ref esy_err_t where applicable:
 *   - `ESY_OK` (0) indicates success
 *   - Negative values indicate failure
 * - GPIO pin numbers are platform-defined global identifiers
 *   (see @ref esy_gpio_defs.h).
 * - Interrupt behavior is configured via @ref esy_gpio_irq_config_t.
 * - Unsupported features must return @ref ESY_ERR_NOT_SUPPORTED.
 *
 * Pico/ESP32 IRQ lifecycle:
 * - Serialize task-context attach, enable, disable and wake-source lifecycle on
 *   the core that first attaches an IRQ through this backend. Cross-core
 *   attach returns STATE; void IRQ operations ignore calls on other cores.
 * - An ESP32 SDK ISR service installed outside this backend is not adopted.
 *   A second binding for an occupied pin returns BUSY without replacing it.
 * - Disable detaches the callback before returning. Do not call lifecycle APIs
 *   from a callback or change the SDK GPIO ISR service outside this backend.
 *
 * STM32F4 EXTI ownership:
 * - Init, attach, enable/disable, wake-source changes and deinit are serialized
 *   task operations. Status-returning lifecycle calls reject ISR context; void
 *   lifecycle calls ignore it. A callback must not reenter these operations.
 * - One encoded port/pin owns each EXTI line until deinit or wake-source detach.
 *   A second attach, including the same pin, returns ESY_ERR_STATE unchanged.
 * - All registered lines in EXTI9_5 or EXTI15_10 must use the same priority,
 *   including temporarily disabled lines. Conflicting attach returns STATE;
 *   out-of-range priorities return INVALID_ARG, without changing existing IRQs.
 * - Disable masks only that owner's line and discards its pending event. Its
 *   callback binding remains borrowed; enable resumes it and discards events
 *   accumulated while disabled. Repeated enable/disable is harmless.
 * - Deinit and disable_wakeup detach their own registration and retire callback
 *   access before returning. Only the final owner disables the group NVIC IRQ.
 *   Calls for an unowned pin cannot change another port/line's IRQ. Shared-group
 *   pending events remain intact. Do not change the group's NVIC configuration
 *   outside this backend while it owns lines.
 */

#ifndef ESY_HB_GPIO_H
#define ESY_HB_GPIO_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a GPIO pin with the specified mode.
 *
 * Configures the pin as input, output, or input with pull-up/pull-down.
 * ESP32 rejects an attached IRQ or wake-source binding with BUSY before any
 * electrical/interrupt change. Detach through the same lifecycle owner first.
 *
 * @param[in] pin   GPIO pin number (platform-defined)
 * @param[in] mode  GPIO mode (see @ref esy_gpio_mode_t)
 * @retval ESY_OK   Success
 * @retval <0       Error code (invalid pin, unsupported mode, etc.)
 */
extern esy_err_t esy_hb_gpio_init(uint8_t pin, esy_gpio_mode_t mode);

/**
 * @brief Deinitialize a GPIO pin.
 *
 * Releases any hardware resources associated with the pin and restores
 * it to a platform-defined default state.
 *
 * @param[in] pin GPIO pin number
 */
extern void esy_hb_gpio_deinit(uint8_t pin);

/**
 * @brief Write a logic level to a GPIO pin.
 *
 * This function is only effective when the pin is configured as output.
 *
 * @param[in] pin    GPIO pin number
 * @param[in] value  `true` for logic high, `false` for logic low
 */
extern void esy_hb_gpio_write(uint8_t pin, bool value);

/**
 * @brief Read the logic level from a GPIO pin.
 *
 * @param[in] pin GPIO pin number
 * @retval true   Logic high
 * @retval false  Logic low
 */
extern bool esy_hb_gpio_read(uint8_t pin);

/**
 * @brief Attach (register) a GPIO interrupt.
 *
 * Configures the interrupt trigger mode, callback handler, and priority
 * according to the provided configuration.
 *
 * @param[in] config Pointer to GPIO interrupt configuration
 * @retval ESY_OK    Success
 * @retval <0        Error code (invalid pin, already registered,
 *                   unsupported interrupt mode, etc.)
 */
extern esy_err_t esy_hb_gpio_attach_irq(
    const esy_gpio_irq_config_t *config);

/**
 * @brief Enable GPIO interrupt for a given pin.
 *
 * @param[in] pin GPIO pin number
 */
extern void esy_hb_gpio_enable_irq(uint8_t pin);

/**
 * @brief Disable GPIO interrupt for a given pin.
 *
 * @param[in] pin GPIO pin number
 */
extern void esy_hb_gpio_disable_irq(uint8_t pin);

/**
 * @brief Enable GPIO wake-up capability in low-power mode.
 *
 * When enabled, an interrupt on this pin can wake the system
 * from a low-power or sleep state.
 *
 * @param[in] config Pointer to GPIO interrupt configuration
 * @param[out] out_cleanup_required Required ownership result, set even on error.
 * True means the callback context or native wake state remains borrowed; keep
 * both alive and retry disable_wakeup until ESY_OK before reuse. False means
 * this call retains neither. Successful enable sets it true. Configuration and
 * rollback errors preserve this ownership result independently of status.
 * @retval ESY_OK    Success
 * @retval <0        Error code (feature not supported, pin cannot wake system,
 * etc.)
 */
extern esy_err_t esy_hb_gpio_enable_wakeup(
    const esy_gpio_irq_config_t *config, bool *out_cleanup_required);

/**
 * @brief Disable low-power wakeup capability for one GPIO pin.
 * On STM32F4 this detaches a wake-source registration and retires its callback;
 * it does not detach an ordinary IRQ registration. Repetition is harmless.
 * @param[in] pin GPIO pin number.
 * @return ESY_OK after wake state and callback access retire, or an error.
 * Failure retains ownership for a same-core task retry; do not reclaim context.
 */
extern esy_err_t esy_hb_gpio_disable_wakeup(uint8_t pin);

/**
 * @brief GPIO interrupt dispatch entry point.
 *
 * This function must be called from the platform-specific
 * interrupt service routine (ISR) to dispatch GPIO interrupts
 * belonging to the specified interrupt group.
 *
 * @param[in] irq_group Interrupt group identifier (platform-defined)
 */
extern void esy_hb_gpio_handle_irq(uint8_t irq_group);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_GPIO_H */
