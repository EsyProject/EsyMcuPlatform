/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_UART_INTERNAL_H
#define ESY_HAL_UART_INTERNAL_H

#include "esy_hal_storage_internal.h"

#include "esy_hal/esy_hal_uart.h"
#include <stdatomic.h>

/* Atomic builtins receive this field's address independently of its runtime. */
typedef _Atomic(uint32_t) esy_uart_dma_atomic_u32_t ESY_HAL_MAY_ALIAS;

_Static_assert(sizeof(esy_uart_dma_atomic_u32_t) == sizeof(_Atomic(uint32_t)),
               "UART atomic state size changed");
_Static_assert(_Alignof(esy_uart_dma_atomic_u32_t) == _Alignof(_Atomic(uint32_t)),
               "UART atomic state alignment changed");

/* The lifecycle owner serializes task operations. DMA state publishes callback
 * ownership with the existing atomic acquire/release protocol. */
typedef struct ESY_HAL_MAY_ALIAS {
  esy_uart_dma_atomic_u32_t state;
  esy_uart_t *owner;
  bool circular;
  esy_dma_callback_t complete_cb;
  void *complete_arg;
  esy_dma_callback_t half_cb;
  void *half_arg;
  esy_dma_callback_t error_cb;
  void *error_arg;
} esy_uart_dma_runtime_t;

typedef struct ESY_HAL_MAY_ALIAS {
  struct esy_device_s *device;
  esy_uart_dma_runtime_t tx_dma;
  esy_uart_dma_runtime_t rx_dma;
  bool rx_stream_owned;
} esy_hal_uart_runtime_t;

_Static_assert(sizeof(esy_hal_uart_runtime_t) <= ESY_UART_HAL_STORAGE_BYTES,
               "HAL UART storage is too small");
_Static_assert(_Alignof(esy_hal_uart_runtime_t) <= _Alignof(void *),
               "HAL UART storage alignment is insufficient");

static inline esy_hal_uart_runtime_t *esy_hal_uart_runtime(esy_uart_t *handle) {
  return (esy_hal_uart_runtime_t *)(void *)handle->hal.bytes;
}

static inline const esy_hal_uart_runtime_t *esy_hal_uart_runtime_const(
    const esy_uart_t *handle) {
  return (const esy_hal_uart_runtime_t *)(const void *)handle->hal.bytes;
}

#endif /* ESY_HAL_UART_INTERNAL_H */
