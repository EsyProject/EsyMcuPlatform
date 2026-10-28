/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_DMA_DEVICE_H
#define ESY_HAL_DMA_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/esy_hal_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief DMA-controller registration configuration. */
typedef struct esy_dma_device_config_s {
  esy_device_registration_config_t device;
} esy_dma_device_config_t;

/**
 * @brief Register a persistent DMA controller in a device graph.
 *
 * The channel table identifies every channel whose busy state gates suspend.
 * A NULL transition is valid when clock/domain switching is performed by a
 * parent power-domain device or the final system-power backend.
 */
esy_err_t esy_hal_dma_register_device(
    esy_dma_t *dma, esy_device_registry_t *registry,
    const esy_dma_device_config_t *config);

/** @brief Unregister an idle DMA controller before registry start. */
esy_err_t esy_hal_dma_unregister_device(esy_dma_t *dma);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_DMA_DEVICE_H */
