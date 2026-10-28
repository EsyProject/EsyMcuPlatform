/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_TIMER_CAPTURE_INTERNAL_H
#define ESY_HAL_TIMER_CAPTURE_INTERNAL_H

#include "esy_hal_storage_internal.h"

#include "esy_common/defs/peripheral/esy_timer_capture_defs.h"
#include "esy_common/device/esy_device.h"

typedef struct ESY_HAL_MAY_ALIAS {
  esy_device_t *device;
  bool pins_configured;
} esy_hal_timer_capture_runtime_t;

_Static_assert(sizeof(esy_hal_timer_capture_runtime_t) <=
                   ESY_TIMER_CAPTURE_HAL_STORAGE_BYTES,
               "Timer capture HAL storage is too small");
_Static_assert(_Alignof(esy_hal_timer_capture_runtime_t) <=
                   _Alignof(esy_timer_capture_t),
               "Timer capture HAL storage alignment is insufficient");
_Static_assert(offsetof(esy_timer_capture_t, hal) %
                   _Alignof(esy_hal_timer_capture_runtime_t) == 0u,
               "Timer capture HAL storage offset is misaligned");

static inline esy_hal_timer_capture_runtime_t *esy_hal_timer_capture_runtime(
    esy_timer_capture_t *capture) {
  return capture == NULL ? NULL :
      (esy_hal_timer_capture_runtime_t *)(void *)capture->hal.bytes;
}

#endif /* ESY_HAL_TIMER_CAPTURE_INTERNAL_H */
