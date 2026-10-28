/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TOUCH_DEVICE_H
#define ESY_TOUCH_DEVICE_H

/** @file
 * @brief Optional logical-device/GPIO PM integration; link esy_touch_device.
 * The facade target does not contain registration or auxiliary GPIO PM policy.
 */

#include "esy_common/device/esy_device.h"
#include "esy_common/defs/system/esy_gpio_pm_defs.h"
#include "esy_middleware/touch/esy_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

struct esy_touch_device_binding_s;

/** @brief Touch-specific device registration configuration. */
typedef struct {
  esy_device_registration_config_t registration; /**< Graph metadata. */
  const esy_device_gpio_pm_profile_t *gpio_pm_profile; /**< Auxiliary GPIOs. */
} esy_touch_device_config_t;

/** @brief Touch-controller-specific PM transition callback. */
typedef esy_err_t (*esy_touch_device_transition_fn_t)(
    struct esy_touch_device_binding_s *binding, esy_device_pm_action_t action,
    esy_power_state_t system_state);

/**
 * @brief Caller-owned logical touch-controller graph binding.
 *
 * Initialize with `{0}` before first registration. Retain the binding, touch
 * handle, GPIO profile and owner until unregister succeeds. Repeated
 * registration is rejected without changing the existing binding.
 */
typedef struct esy_touch_device_binding_s {
  esy_device_t device;
  esy_touch_t *touch;
  esy_touch_device_transition_fn_t transition;
  const esy_device_gpio_pm_profile_t *gpio_pm_profile;
  void *owner;
} esy_touch_device_binding_t;

/**
 * @brief Register an attached touch controller as a graph device.
 *
 * Serialize registration with handle access, graph lifecycle and callbacks.
 * Both the binding and handle must be unbound. The binding must be initialized
 * with `{0}` or have completed a successful unregister. The registry must still
 * be configuring. All referenced configuration and owner objects remain
 * caller-owned and must outlive the registration.
 *
 * On failure, successful rollback leaves the handle unbound and clears the
 * binding for retry. If GPIO rollback fails, the handle and binding retain
 * ownership without a graph node; restore default GPIO PM state and call
 * @ref esy_touch_unregister_device before retrying registration or detaching.
 *
 * @param[in,out] binding Caller-owned binding.
 * @param[in,out] touch Attached touch handle.
 * @param[in,out] registry Device registry.
 * @param[in] device_config Graph metadata and optional auxiliary GPIO profile.
 * @param[in] transition Controller-specific PM callback, invoked synchronously
 * in the graph transition caller's context under the device PM contract.
 * @param[in] owner Borrowed controller context used by the transition callback.
 * @retval ESY_OK Registered successfully.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer or unattached handle.
 * @retval ESY_ERR_STATE Binding or handle is already bound, or graph phase is
 * incompatible. GPIO cleanup failures also retain ownership as described above.
 * @retval <0 GPIO claim, graph registration or rollback failure.
 */
esy_err_t esy_touch_register_device(
    esy_touch_device_binding_t *binding, esy_touch_t *touch,
    esy_device_registry_t *registry,
    const esy_touch_device_config_t *device_config,
    esy_touch_device_transition_fn_t transition, void *owner);

/**
 * @brief Unbind touch before graph start or after registry deinit.
 * Stop polling and drain provider callbacks first; serialize all lifecycle calls.
 * Registry deinit does not detach the touch handle or release GPIO ownership.
 * Failure retains the binding; restore the GPIO default PM state and retry
 * after a GPIO release error. Detach the handle only after successful unbind.
 */
esy_err_t esy_touch_unregister_device(esy_touch_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_TOUCH_DEVICE_H */
