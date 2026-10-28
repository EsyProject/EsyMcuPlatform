/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_XIO_UART_DEVICE_H
#define ESY_HAL_XIO_UART_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/xio/esy_hal_xio_uart.h"
#include "esy_hal/esy_hal_uart_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned adapter for one XIO UART instance.
 *
 * @note Initialize this binding with {0} before its first registration. Keep it
 * alive and unchanged until successful unregister, including after registry
 * deinit. Reusing an attached binding for another handle returns ESY_ERR_STATE
 * without changing its original handle or graph node. Registration failures
 * preserve an existing binding; never clear it to bypass unregister.
 */
typedef struct {
  esy_uart_device_binding_t base; /**< Shared UART DMA/reference state. */
  esy_xio_uart_config_t tx_resume_config;
  esy_xio_uart_config_t rx_resume_config;
  bool has_tx;
  bool has_rx;
} esy_xio_uart_device_binding_t;

/** @brief Register an initialized XIO UART before registry start. */
esy_err_t esy_hal_xio_uart_register_device(
    esy_xio_uart_device_binding_t *binding, esy_uart_t *uart,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_xio_uart_config_t *tx_resume_config,
    const esy_xio_uart_config_t *rx_resume_config);

/**
 * @brief Unbind XIO UART before graph start or after registry deinit.
 * Stop DMA/callbacks and serialize with I/O and lifecycle calls first. Registry
 * deinit leaves this binding attached; unbind before hardware deinit or reuse.
 * Failure retains ownership and permits retry after its cause is resolved.
 */
esy_err_t esy_hal_xio_uart_unregister_device(
    esy_xio_uart_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_XIO_UART_DEVICE_H */
