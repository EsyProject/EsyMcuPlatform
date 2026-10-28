/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_PWM_INTERNAL_H
#define ESY_HAL_PWM_INTERNAL_H

#include "esy_hal_storage_internal.h"

#include "esy_common/defs/peripheral/esy_pwm_defs.h"
#include "esy_common/device/esy_device.h"

typedef struct ESY_HAL_MAY_ALIAS {
  esy_device_t *device;
  esy_dma_callback_t dma_complete_cb;
  void *dma_complete_arg;
  esy_dma_callback_t dma_half_cb;
  void *dma_half_arg;
  esy_dma_callback_t dma_error_cb;
  void *dma_error_arg;
  bool output_enabled;
  bool dma_active;
  bool dma_circular;
  bool callback_running;
} esy_hal_pwm_runtime_t;

_Static_assert(sizeof(esy_hal_pwm_runtime_t) <= ESY_PWM_HAL_STORAGE_BYTES,
               "PWM HAL storage is too small");
_Static_assert(_Alignof(esy_hal_pwm_runtime_t) <= _Alignof(esy_pwm_t),
               "PWM HAL storage alignment is insufficient");
_Static_assert(offsetof(esy_pwm_t, hal) % _Alignof(esy_hal_pwm_runtime_t) == 0u,
               "PWM HAL storage offset is misaligned");

static inline esy_hal_pwm_runtime_t *esy_hal_pwm_runtime(esy_pwm_t *pwm) {
  return pwm == NULL ? NULL : (esy_hal_pwm_runtime_t *)(void *)pwm->hal.bytes;
}

#endif /* ESY_HAL_PWM_INTERNAL_H */
