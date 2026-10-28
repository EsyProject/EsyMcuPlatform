/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_XIO_SPI_DEVICE_H
#define ESY_HAL_XIO_SPI_DEVICE_H

#include "esy_hal/esy_hal_spi_device.h"
#include "esy_hal/xio/esy_hal_xio_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register an initialized XIO SPI controller as a graph device.
 * Initialize binding storage with {0}; keep it unchanged until successful
 * unregister, including after registry deinit. An attached binding cannot be
 * reused for another handle; rejection preserves the original binding.
 * Registration may adopt a controller with an accepted DMA transfer. Such a
 * transfer continues to own its buffers; runtime/system suspend returns BUSY
 * until native work retires through owner poll or successful abort. Serialize
 * registration, graph transitions and I/O on the same owner context.
 * Use esy_hal_spi_unregister_device() for serialized cleanup before graph start
 * or after quiescent registry deinit, then deinitialize the XIO SPI handle.
 */
esy_err_t esy_hal_xio_spi_register_device(
    esy_spi_device_binding_t *binding, esy_spi_t *spi,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_xio_spi_config_t *resume_config);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_XIO_SPI_DEVICE_H */
