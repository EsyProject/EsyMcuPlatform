/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LCD_DEVICE_H
#define ESY_LCD_DEVICE_H

/** @file
 * @brief Optional logical-device/GPIO PM integration; link esy_lcd_device.
 * The facade target does not contain registration or auxiliary GPIO PM policy.
 */

#include "esy_common/device/esy_device.h"
#include "esy_common/defs/system/esy_gpio_pm_defs.h"
#include "esy_middleware/lcd/esy_lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

struct esy_lcd_device_binding_s;

/** @brief LCD-specific device registration configuration. */
typedef struct {
  esy_device_registration_config_t registration; /**< Graph metadata. */
  const esy_device_gpio_pm_profile_t *gpio_pm_profile; /**< Auxiliary GPIOs. */
} esy_lcd_device_config_t;

/** @brief LCD-controller-specific PM transition callback. */
typedef esy_err_t (*esy_lcd_device_transition_fn_t)(
    struct esy_lcd_device_binding_s *binding, esy_device_pm_action_t action,
    esy_power_state_t system_state);

/**
 * @brief Caller-owned logical LCD graph binding; initialize with {0}.
 *
 * Keep this object and its profile alive until unregister succeeds. Never
 * clear or reuse a live binding, including after registry deinitialization.
 * Registration, cleanup and LCD user callbacks must be externally serialized.
 */
typedef struct esy_lcd_device_binding_s {
  esy_device_t device;
  esy_lcd_t *lcd;
  esy_lcd_device_transition_fn_t transition;
  const esy_device_gpio_pm_profile_t *gpio_pm_profile;
  void *owner;
  void (*flush_done_cb)(void *);
  void *flush_done_arg;
  esy_device_atomic_u32_t flush_ref_held; /**< Internal task handoff flag. */
} esy_lcd_device_binding_t;

/**
 * @brief Register an attached LCD as a logical graph device.
 * @retval ESY_ERR_STATE The binding is already in use; it is unchanged.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments or an already-bound LCD.
 * @retval ESY_OK Registered; the caller retains all referenced storage.
 * @retval <0 Registration failed; an unused binding remains reusable.
 */
esy_err_t esy_lcd_register_device(
    esy_lcd_device_binding_t *binding, esy_lcd_t *lcd,
    esy_device_registry_t *registry,
    const esy_lcd_device_config_t *device_config,
    esy_lcd_device_transition_fn_t transition, void *owner);

/**
 * @brief Release a binding before registry start or after registry deinit.
 *
 * After normal operation, stop producers, drain callbacks, resume any suspended
 * devices, deinitialize the registry, then unregister each LCD binding before
 * detach. No LCD operations are allowed between registry deinit and cleanup.
 * GPIO profiles must be in DEFAULT state. On release failure, retain the
 * binding, restore DEFAULT and retry; a removed graph node is not re-removed.
 * @retval ESY_OK All binding resources released; reuse is allowed.
 * @retval ESY_ERR_STATE Registry still running, or GPIO release precondition.
 * @retval ESY_ERR_INVALID_ARG Invalid binding or a flush reference is held.
 * @retval <0 Cleanup failed; retain resources and retry after recovery.
 */
esy_err_t esy_lcd_unregister_device(esy_lcd_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_LCD_DEVICE_H */
