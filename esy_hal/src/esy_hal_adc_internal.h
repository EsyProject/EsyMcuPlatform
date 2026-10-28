/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_ADC_INTERNAL_H
#define ESY_HAL_ADC_INTERNAL_H

#include "esy_hal_storage_internal.h"

#include "esy_common/defs/peripheral/esy_adc_defs.h"
#include "esy_common/device/esy_device.h"

typedef struct ESY_HAL_MAY_ALIAS {
  esy_device_t *device;
  esy_adc_trigger_edge_t trigger_edge;
  esy_adc_trigger_source_t trigger_source;
} esy_hal_adc_runtime_t;

_Static_assert(sizeof(esy_hal_adc_runtime_t) <= ESY_ADC_HAL_STORAGE_BYTES,
               "ADC HAL storage is too small");
_Static_assert(_Alignof(esy_hal_adc_runtime_t) <= _Alignof(esy_adc_t),
               "ADC HAL storage alignment is insufficient");
_Static_assert(offsetof(esy_adc_t, hal) % _Alignof(esy_hal_adc_runtime_t) == 0u,
               "ADC HAL storage offset is misaligned");

static inline esy_hal_adc_runtime_t *esy_hal_adc_runtime(esy_adc_t *adc) {
  return adc == NULL ? NULL : (esy_hal_adc_runtime_t *)(void *)adc->hal.bytes;
}

#endif /* ESY_HAL_ADC_INTERNAL_H */
