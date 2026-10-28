/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_USB_DEV_DEVICE_H
#define ESY_HAL_USB_DEV_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/esy_hal_usb_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned CPU-idle-only USB device graph binding.
 *
 * @note Initialize this binding with {0} before its first registration. Keep it
 * alive and unchanged until successful unregister, including after registry
 * deinit. Reusing an attached binding for another handle returns ESY_ERR_STATE
 * without changing its original handle or graph node. Registration failures
 * preserve an existing binding; never clear it to bypass unregister.
 */
typedef struct {
  esy_device_t device;
  esy_usb_dev_t *usb;
} esy_usb_dev_device_binding_t;

/**
 * @brief Register an initialized USB controller as a CPU-idle-only device.
 *
 * Deep states are intentionally rejected until endpoint configuration and PHY
 * wake restoration are implemented by the selected port.
 */
esy_err_t esy_hal_usb_dev_register_device(
    esy_usb_dev_device_binding_t *binding, esy_usb_dev_t *usb,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config);

/**
 * @brief Unbind USB before graph start or after registry deinit.
 * Quiesce transfers/callbacks and serialize I/O and lifecycle first. Registry
 * deinit leaves the binding attached; unbind before hardware deinit or reuse.
 * Failure retains ownership and permits retry after its cause is resolved.
 */
esy_err_t esy_hal_usb_dev_unregister_device(
    esy_usb_dev_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_USB_DEV_DEVICE_H */
