/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_UART_DEVICE_H
#define ESY_HAL_UART_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/esy_hal_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned adapter that makes one hardware UART a graph device.
 *
 * @note Initialize this binding with {0} before its first registration. Keep it
 * alive and unchanged until successful unregister, including after registry
 * deinit. Reusing an attached binding for another handle returns ESY_ERR_STATE
 * without changing its original handle or graph node. Registration failures
 * preserve an existing binding; never clear it to bypass unregister.
 */
typedef struct {
  esy_device_t device;
  esy_uart_t *uart;
  bool suspended;
  bool tx_dma_ref_held;
  bool rx_dma_ref_held;
  bool rx_stream_ref_held;
  uint8_t pin_state; /**< Internal board-state value retained for rollback. */
} esy_uart_device_binding_t;

/** @brief Register an initialized hardware UART before registry start. */
esy_err_t esy_hal_uart_register_device(
    esy_uart_device_binding_t *binding, esy_uart_t *uart,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config);

/**
 * @brief Unbind a UART adapter before graph start or after registry deinit.
 * Stop streams, DMA and callbacks first; serialize with I/O and lifecycle calls.
 * Registry deinit leaves the binding attached; unbind before hardware deinit.
 * Failure retains ownership. Retry only when the registry permits unregister;
 * a faulted graph requires system recovery/reset and cannot be unbound here.
 */
esy_err_t esy_hal_uart_unregister_device(
    esy_uart_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_UART_DEVICE_H */
