/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_USB_DEV_INTERNAL_H
#define ESY_HAL_USB_DEV_INTERNAL_H

#include "esy_hal_storage_internal.h"

#include "esy_hal/esy_hal_usb_dev_device.h"
#include <stddef.h>

typedef struct ESY_HAL_MAY_ALIAS {
  esy_device_t *device;
} esy_hal_usb_runtime_t;

_Static_assert(sizeof(esy_hal_usb_runtime_t) <=
                   sizeof(((esy_usb_dev_t *)0)->hal.bytes),
               "USB HAL storage is too small");
_Static_assert(offsetof(esy_usb_dev_t, hal) % _Alignof(esy_hal_usb_runtime_t) == 0u &&
                   _Alignof(esy_usb_dev_t) >= _Alignof(esy_hal_usb_runtime_t),
               "USB HAL storage is misaligned");

static inline esy_hal_usb_runtime_t *esy_hal_usb_runtime(esy_usb_dev_t *usb) {
  return (esy_hal_usb_runtime_t *)(void *)usb->hal.bytes;
}

#endif /* ESY_HAL_USB_DEV_INTERNAL_H */
