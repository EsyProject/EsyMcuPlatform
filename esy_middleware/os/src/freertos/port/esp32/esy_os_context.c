/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_context.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool esy_os_context_is_in_isr(void) { return xPortInIsrContext() != 0; }
uint8_t esy_os_context_core_count(void) {
  return (uint8_t)CONFIG_FREERTOS_NUMBER_OF_CORES;
}
void esy_os_context_irq_disable(void) { portDISABLE_INTERRUPTS(); }
void esy_os_context_irq_enable(void) { portENABLE_INTERRUPTS(); }
uint32_t esy_os_context_irq_save(void) {
  return (uint32_t)portSET_INTERRUPT_MASK_FROM_ISR();
}
void esy_os_context_irq_restore(uint32_t saved_status) {
  portCLEAR_INTERRUPT_MASK_FROM_ISR((UBaseType_t)saved_status);
}
