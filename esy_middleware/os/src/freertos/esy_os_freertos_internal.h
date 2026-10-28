/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_OS_FREERTOS_INTERNAL_H
#define ESY_OS_FREERTOS_INTERNAL_H
#ifdef ESY_PLATFORM_ESP32
#include "freertos/FreeRTOS.h"
#else
#include "FreeRTOS.h"
#endif
#include <stdint.h>

/* Finite positive waits round up and saturate below the native forever token. */
static inline TickType_t esy_os_finite_ticks(uint32_t ms) {
  uint64_t ticks = ((uint64_t)ms * configTICK_RATE_HZ + 999u) / 1000u;
  uint64_t limit = (uint64_t)portMAX_DELAY - 1u;
  return (TickType_t)(ticks > limit ? limit : ticks);
}

static inline TickType_t esy_os_timeout_ticks(uint32_t ms) {
  return ms == UINT32_MAX ? portMAX_DELAY : esy_os_finite_ticks(ms);
}
#endif
