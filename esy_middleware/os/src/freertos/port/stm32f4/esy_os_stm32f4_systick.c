/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_system.h"
#include "esy_middleware/os/esy_os.h"

void SysTick_Handler(void);

void SysTick_Handler(void) {
  esy_hal_system_tick_from_isr();
  if (esy_os_started()) {
    esy_os_systick();
  }
}
