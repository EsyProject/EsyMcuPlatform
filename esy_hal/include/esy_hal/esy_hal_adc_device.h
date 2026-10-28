/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_ADC_DEVICE_H
#define ESY_HAL_ADC_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/esy_hal_adc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned adapter that makes one ADC controller a graph device.
 * @note Keep this storage alive until unregister. A finite DMA operation holds
 * one graph lease through terminal callback delivery; circular DMA retains it
 * across cycles until abort/error. Internal callback fields must not be changed.
 *
 * @note Initialize this binding with {0} before its first registration. Keep it
 * alive and unchanged until successful unregister, including after registry
 * deinit. Reusing an attached binding for another handle returns ESY_ERR_STATE
 * without changing its original handle or graph node. Registration failures
 * preserve an existing binding; never clear it to bypass unregister.
 */
typedef struct {
  esy_device_t device;
  esy_adc_t *adc;
  esy_adc_config_t resume_config;
  esy_dma_callback_t complete_cb;
  void *complete_arg;
  esy_dma_callback_t error_cb;
  void *error_arg;
  bool suspended;
  esy_device_atomic_u32_t dma_ref_held;
  bool dma_circular;
} esy_adc_device_binding_t;

/**
 * @note Register only with DMA stopped; active DMA returns ESY_ERR_BUSY.
 * Trigger policy is retained by the handle across resume. A failed partial
 * hardware suspend returns ESY_ERR_STATE and faults the device graph.
 */
/** @brief Register an initialized ADC controller before registry start. */
esy_err_t esy_hal_adc_register_device(
    esy_adc_device_binding_t *binding, esy_adc_t *adc,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_adc_config_t *resume_config);

/**
 * @brief Unbind an idle ADC before graph start or after registry deinit.
 * Serialize with I/O and lifecycle calls; finish or abort DMA first. Successful
 * registry deinit alone does not detach this binding or deinitialize hardware.
 * Failure retains the binding and permits retry after its cause is resolved.
 */
esy_err_t esy_hal_adc_unregister_device(esy_adc_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_ADC_DEVICE_H */
