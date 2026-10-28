/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_GPIO_H
#define ESY_HAL_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_hal_gpio.h
 * @brief Handle-based GPIO resource interface.
 *
 * Initialize handle storage with {0} before first init. Repeated init returns
 * ESY_ERR_STATE. Failed init leaves zero storage; successful deinit permits init
 * again. Never reset live storage to bypass ownership or pending IRQ cleanup.
 * Each handle has one owner from init through deinit. Operations are externally
 * synchronized and task-context unless explicitly documented as ISR dispatch.
 * IRQ callbacks run in ISR context, are bounded and nonblocking, and must not
 * reenter GPIO lifecycle/configuration APIs. Keep their context alive until
 * deinit, or successful disable_wakeup for a wake-source registration. Individual GPIO handles are resources, not device-graph nodes. Device
 * GPIO-only PM policy is represented by the GPIO PM profile API.
 *
 * Pico/ESP32 IRQ and wake-source lifecycle must run on the core used by the
 * first GPIO IRQ attachment, including deinit and PM-driven detach. Pin the
 * serialized lifecycle owner to that core; ordinary task migration is unsafe
 * for callback retirement. Cross-core attach returns STATE. An occupied pin
 * returns BUSY without replacing its callback. Native IRQ registrations must
 * remain under backend ownership. ESP32 cannot adopt an external ISR service.
 *
 * STM32F4 EXTI uses one owner per line across all ports. Duplicate attach returns
 * STATE, including repeated attach to the same pin; deinit releases that owner.
 * Lines 5..9 share one NVIC IRQ and lines 10..15 share another. Registrations in
 * a group must use the same board-provided priority; a conflicting attach returns
 * STATE without changing existing registrations. Deinit and wake-source detach
 * affect only their owner; ordinary GPIO cleanup cannot stop a peer interrupt.
 * Keep the group's NVIC configuration under backend ownership.

 */

/** @brief Fill a GPIO configuration with safe defaults. */
void esy_hal_gpio_default_cfg(esy_gpio_config_t *config);

/** @brief Initialize a caller-owned GPIO handle and apply its active state. */
esy_err_t esy_hal_gpio_init(esy_gpio_t *gpio,
                                 const esy_gpio_config_t *config);

/**
 * @brief Report whether GPIO initialization has completed.
 * @param[in] gpio Zero-initialized or live caller-owned handle; NULL is allowed.
 * @return false for NULL or inactive storage, true for an initialized handle.
 * @note Task-context observation serialized with GPIO lifecycle; no hardware I/O.
 */
bool esy_hal_gpio_is_initialized(const esy_gpio_t *gpio);

/**
 * @brief Copy the original GPIO configuration without changing electrical state.
 * @param[in] gpio Initialized caller-owned handle.
 * @param[out] config Configuration snapshot; unchanged on failure.
 * @retval ESY_OK Snapshot copied, including while PM selects idle/sleep state.
 * @retval ESY_ERR_INVALID_ARG Either argument is NULL.
 * @retval ESY_ERR_STATE The handle is not initialized.
 * @note Task-context observation serialized with lifecycle. The snapshot owns no
 * resources and does not authorize I/O or bypass a PM claim.
 */
esy_err_t esy_hal_gpio_get_config(const esy_gpio_t *gpio,
                                 esy_gpio_config_t *config);

/**
 * @brief Deinitialize a GPIO handle and detach its interrupt ownership.
 *
 * Wake cleanup errors preserve the initialized handle, native wake/IRQ
 * ownership and callback context for retry; no pin reset occurs on failure.
 * @retval ESY_ERR_BUSY The handle belongs to a claimed GPIO PM profile.
 */
esy_err_t esy_hal_gpio_deinit(esy_gpio_t *gpio);

/**
 * @brief Write an initialized GPIO configured as output.
 *
 * @retval ESY_ERR_STATE A PM owner selected idle or sleep state.
 */
esy_err_t esy_hal_gpio_write(esy_gpio_t *gpio, bool value);

/**
 * @brief Read an initialized GPIO into @p out_value.
 *
 * @retval ESY_ERR_STATE A PM owner selected idle or sleep state.
 */
esy_err_t esy_hal_gpio_read(const esy_gpio_t *gpio, bool *out_value);

/**
 * @brief Apply an explicit electrical state to an initialized GPIO.
 *
 * ESP32 preserves attached IRQ/wake configuration by rejecting state changes
 * with BUSY. Deinitialize an ordinary IRQ handle, or successfully disable a
 * wake source, before reconfiguration. GPIO PM state selection has the same
 * restriction; keep IRQ-bearing pins out of changing profiles until detached.
 * @retval ESY_ERR_BUSY ESP32 has an attached IRQ or wake-source binding.
 * @retval ESY_ERR_STATE A PM owner selected idle or sleep state.
 */
esy_err_t esy_hal_gpio_apply_state(esy_gpio_t *gpio,
                                        const esy_gpio_state_t *state);

/**
 * @brief Restore the normal state captured during GPIO initialization.
 *
 * @retval ESY_ERR_BUSY ESP32 has an attached IRQ or wake-source binding;
 * its electrical state and interrupt configuration remain unchanged.
 * @retval ESY_ERR_STATE A PM owner selected idle or sleep state.
 */
esy_err_t esy_hal_gpio_apply_configured_state(esy_gpio_t *gpio);

/**
 * @brief Attach an IRQ and configure its input pull.
 *
 * @param[in,out] gpio Initialized GPIO handle owned by the caller.
 * @param[in] irq_mode Interrupt edge or level selection.
 * @param[in] pull Input pull selection; ESY_GPIO_PULL_NONE disables pulls.
 * @param[in] handler Callback invoked synchronously in the GPIO ISR.
 * @param[in] arg Caller-owned callback context, valid while the IRQ is attached.
 * @note Serialize configuration and lifecycle from the owning task. The callback
 * must be bounded and ISR-safe.
 * @retval ESY_OK The backend accepted the IRQ configuration.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or callback.
 * @retval ESY_ERR_BUSY Another PM owner claims the physical pin.
 * @retval ESY_ERR_STATE A PM owner selected idle or sleep state.
 * @retval <0 Backend configuration error.
 */
esy_err_t esy_hal_gpio_attach_irq(esy_gpio_t *gpio,
                                  esy_gpio_irq_mode_t irq_mode,
                                  esy_gpio_pull_t pull,
                                  esy_gpio_irq_handler_t handler,
                                  void *arg);

/**
 * @brief Configure an IRQ-capable GPIO as a low-power wake source.
 *
 * A backend setup/rollback failure may retain wake/IRQ ownership and the
 * callback argument even when this function returns an error. Keep both handle
 * and argument alive until @ref esy_hal_gpio_disable_wakeup or
 * @ref esy_hal_gpio_deinit returns ESY_OK. Deinit also safely closes an attempt
 * rejected before borrowing resources. Do not retry enable or clear the handle
 * while cleanup remains required. Retry cleanup after a cleanup error.
 *
 * @retval ESY_ERR_STATE A PM owner selected idle or sleep state.
 */
esy_err_t esy_hal_gpio_enable_wakeup(esy_gpio_t *gpio,
                                          esy_gpio_irq_mode_t irq_mode,
                                          esy_gpio_irq_handler_t handler,
                                          void *arg);

/**
 * @brief Disable wake-source ownership previously enabled on a GPIO.
 *
 * Native cleanup failure retains ownership and callback arguments for retry.
 * @retval ESY_ERR_STATE The GPIO is not a wake source or PM selected a
 * non-default state.
 */
esy_err_t esy_hal_gpio_disable_wakeup(esy_gpio_t *gpio);

/**
 * @brief Dispatch a GPIO interrupt group from the platform ISR.
 *
 * @note ISR-safe. The HAL backend invokes the registered pin callback synchronously.
 */
void esy_hal_gpio_handle_irq(uint8_t irq_group);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_GPIO_H */
