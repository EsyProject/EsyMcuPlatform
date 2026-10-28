/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_SPI_INTERNAL_H
#define ESY_HAL_SPI_INTERNAL_H

#include "esy_hal_storage_internal.h"

#include "esy_hal/esy_hal_spi.h"

/* Only HAL implementations inspect this caller-owned lifecycle state. */
typedef struct ESY_HAL_MAY_ALIAS {
  struct esy_device_s *device;
  bool pins_configured;
} esy_hal_spi_runtime_t;

_Static_assert(sizeof(esy_hal_spi_runtime_t) <= ESY_SPI_HAL_STORAGE_BYTES,
               "HAL SPI storage is too small");
_Static_assert(_Alignof(esy_hal_spi_runtime_t) <= _Alignof(void *),
               "HAL SPI storage alignment is insufficient");

static inline esy_hal_spi_runtime_t *esy_hal_spi_runtime(esy_spi_t *handle) {
  return (esy_hal_spi_runtime_t *)(void *)handle->hal.bytes;
}

static inline const esy_hal_spi_runtime_t *esy_hal_spi_runtime_const(
    const esy_spi_t *handle) {
  return (const esy_hal_spi_runtime_t *)(const void *)handle->hal.bytes;
}

#endif /* ESY_HAL_SPI_INTERNAL_H */
