/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_boot.h"

#include "stm32f4xx_hal.h"

/* Only basic assembly belongs in this naked function. Once MSP changes, no
 * compiler-generated load, store, call epilogue or C local may use the old stack.
 * A reset handler that unexpectedly returns stays in a stack-free trap. */
__attribute__((naked, noreturn, noinline)) static void enter_application(
    uint32_t initial_msp __attribute__((unused)),
    uint32_t reset_handler __attribute__((unused))) {
  __asm volatile("msr msp, r0\n"
                 "dsb 0xf\n"
                 "isb 0xf\n"
                 "blx r1\n"
                 "1: b 1b\n");
}

void esy_hb_boot_jump_to_app(uint32_t app_address) {
  /* Handoff is a privileged startup operation, not an exception return or an
   * RTOS task switch. Reject incompatible CPU contexts before changing state. */
  if ((__get_IPSR() != 0u) ||
      ((__get_CONTROL() & (CONTROL_nPRIV_Msk | CONTROL_SPSEL_Msk)) != 0u)) {
    return;
  }
  const uint32_t initial_msp = *(const uint32_t *)app_address;
  const uint32_t reset_handler = *(const uint32_t *)(app_address + 4u);

  __disable_irq();

  HAL_SuspendTick();

  for (uint8_t i = 0; i < 8; ++i) {
    NVIC->ICER[i] = 0xFFFFFFFF;
    NVIC->ICPR[i] = 0xFFFFFFFF;
  }

  SCB->VTOR = app_address;
  __DSB();
  __ISB();
  enter_application(initial_msp, reset_handler);
}
