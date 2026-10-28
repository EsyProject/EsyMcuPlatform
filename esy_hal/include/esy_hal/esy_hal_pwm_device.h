/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_PWM_DEVICE_H
#define ESY_HAL_PWM_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/esy_hal_pwm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned adapter that makes one PWM channel a graph device.
 *
 * @note Initialize this binding with {0} before its first registration. Keep it
 * alive and unchanged until successful unregister, including after registry
 * deinit. Reusing an attached binding for another handle returns ESY_ERR_STATE
 * without changing its original handle or graph node. Registration failures
 * preserve an existing binding; never clear it to bypass unregister.
 */
typedef struct {
  esy_device_t device;
  esy_pwm_t *pwm;
  bool suspended;
  bool dma_ref_held;
  bool output_enabled;
} esy_pwm_device_binding_t;

/** @note Register only with DMA stopped; active DMA returns ESY_ERR_BUSY. */
/** @brief Register an initialized PWM channel before registry start. */
esy_err_t esy_hal_pwm_register_device(
    esy_pwm_device_binding_t *binding, esy_pwm_t *pwm,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config);

/**
 * @brief Unbind an idle PWM adapter before registry start or after deinit.
 * @note A quiescent registry deinit detaches the graph but does not retire DMA.
 * Retire all DMA/callback work before unbinding. A busy adapter remains intact.
 */
esy_err_t esy_hal_pwm_unregister_device(esy_pwm_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_PWM_DEVICE_H */
