/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_TIMER_CAPTURE_DEVICE_H
#define ESY_HAL_TIMER_CAPTURE_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/esy_hal_timer_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned adapter for one timer-capture input.
 *
 * Suspension retains the physical claim. A native teardown failure may leave
 * capture stopped or partially released and returns ESY_ERR_STATE to fault the
 * graph. Preserve handle and callback storage until system recovery/reset; a
 * faulted registry cannot be unregistered or deinitialized for local reuse.
 * Do not resume ordinary I/O or clear the registry to bypass its ownership.
 *
 * @note Initialize this binding with {0} before its first registration. Keep it
 * alive and unchanged until successful unregister, including after registry
 * deinit. Reusing an attached binding for another handle returns ESY_ERR_STATE
 * without changing its original handle or graph node. Registration failures
 * preserve an existing binding; never clear it to bypass unregister.
 */
typedef struct {
  esy_device_t device;
  esy_timer_capture_t *capture;
  esy_timer_capture_config_t resume_config;
  bool suspended;
} esy_timer_capture_device_binding_t;

/** @brief Register an initialized timer-capture input before registry start. */
esy_err_t esy_hal_timer_capture_register_device(
    esy_timer_capture_device_binding_t *binding, esy_timer_capture_t *capture,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_timer_capture_config_t *resume_config);

/**
 * @brief Unbind capture before graph start or after registry deinit.
 * Quiesce producers/callbacks and serialize I/O and lifecycle first. Registry
 * deinit leaves the binding attached; unbind before hardware deinit or reuse.
 * Failure retains ownership and permits retry after its cause is resolved.
 */
esy_err_t esy_hal_timer_capture_unregister_device(
    esy_timer_capture_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_TIMER_CAPTURE_DEVICE_H */
