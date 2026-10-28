/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_boot.h"

void esy_hb_boot_jump_to_app(uint32_t app_address) {
  const uint32_t vtor_address = 0xe0000000u + 0x0000ed08u;

  asm volatile(
      "mov r0, %[start]\n"
      "mov r1, %[vtable]\n"
      "str r0, [r1]\n"
      "ldmia r0, {r0, r1}\n"
      "msr msp, r0\n"
      "bx r1\n"
      :
      : [start] "r"(app_address), [vtable] "r"(vtor_address)
      : "r0", "r1");
}
