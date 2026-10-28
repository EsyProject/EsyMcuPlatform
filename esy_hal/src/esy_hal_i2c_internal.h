/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_I2C_INTERNAL_H
#define ESY_HAL_I2C_INTERNAL_H

#include "esy_hal_storage_internal.h"

#include "esy_hal/esy_hal_i2c.h"

/* Only HAL implementations inspect this caller-owned lifecycle state. */
typedef struct ESY_HAL_MAY_ALIAS {
  struct esy_device_s *device;
} esy_hal_i2c_runtime_t;

_Static_assert(sizeof(esy_hal_i2c_runtime_t) <= ESY_I2C_HAL_STORAGE_BYTES,
               "HAL I2C storage is too small");
_Static_assert(_Alignof(esy_hal_i2c_runtime_t) <= _Alignof(void *),
               "HAL I2C storage alignment is insufficient");

static inline esy_hal_i2c_runtime_t *esy_hal_i2c_runtime(esy_i2c_t *handle) {
  return (esy_hal_i2c_runtime_t *)(void *)handle->hal.bytes;
}

static inline const esy_hal_i2c_runtime_t *esy_hal_i2c_runtime_const(
    const esy_i2c_t *handle) {
  return (const esy_hal_i2c_runtime_t *)(const void *)handle->hal.bytes;
}

#endif /* ESY_HAL_I2C_INTERNAL_H */
