/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_system.h"

/** @brief Advance the HAL timebase when no FreeRTOS port owns SysTick. */
void SysTick_Handler(void) {
  esy_hb_system_tick_from_isr();
}
