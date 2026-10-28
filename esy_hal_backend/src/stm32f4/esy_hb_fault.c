/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_fault.h"

#include <stddef.h>
#include "stm32f4xx_hal.h"

/* The first STM32 port shares the verified Flash/boot device family. CCM is
 * deliberately excluded: a generic pointer must name contiguous main SRAM. */
#if !defined(STM32F405xx) && !defined(STM32F407xx) && !defined(STM32F415xx) && !defined(STM32F417xx)
#error "The fault port requires a verified STM32F405/407/415/417 SRAM layout"
#endif

#define SRAM_END 0x20020000u
#define FAULT_CORE_COUNT 1
#define FAULT_CLAIM_STYLE 0
#define FAULT_HAS_STATUS 1
#define FAULT_VECTOR_COUNT 128u

static ESY_FAULT_RAM_FUNC __attribute__((noreturn)) void fault_port_reset(void) {
  __asm volatile("dsb" ::: "memory");
  *(volatile uint32_t *)0xe000ed0cu = 0x05fa0004u;
  __asm volatile("dsb" ::: "memory");
  for (;;) __asm volatile("wfe" ::: "memory");
}

#include "../arm/esy_hb_fault_core.h"

FAULT_NAKED __attribute__((noreturn))
void esy_hb_fault_raise(esy_fault_reason_t reason __attribute__((unused)),
                        uint32_t detail0 __attribute__((unused)),
                        uint32_t detail1 __attribute__((unused))) {
  __asm volatile("b fault_software_entry\n");
}

esy_err_t esy_hb_fault_init(const esy_fault_config_t *config) {
  if (config == NULL || config->capture == NULL ||
      config->stack_snapshot_bytes > ESY_FAULT_STACK_MAX_BYTES) return ESY_ERR_INVALID_ARG;
  if (s_ctx.initialized) return ESY_ERR_STATE;
  if (__get_IPSR() != 0u || (__get_CONTROL() & 1u) != 0u ||
      (MPU->CTRL & MPU_CTRL_ENABLE_Msk) != 0u) return ESY_ERR_NOT_SUPPORTED;
  uintptr_t callback = (uintptr_t)config->capture & ~(uintptr_t)1u;
  if (callback < SRAM_BASE || callback >= SRAM_END ||
      (config->ctx != NULL && ((uintptr_t)config->ctx < SRAM_BASE ||
                               (uintptr_t)config->ctx >= SRAM_END))) return ESY_ERR_INVALID_ARG;
  uint32_t vector_address = SCB->VTOR;
  uint32_t flash_kib = *(const volatile uint16_t *)FLASHSIZE_BASE;
  if (flash_kib != 512u && flash_kib != 1024u) return ESY_ERR_NOT_SUPPORTED;
  uint32_t flash_end = FLASH_BASE + flash_kib * 1024u;
  bool flash_vector = vector_address >= FLASH_BASE &&
                      vector_address <= flash_end - sizeof(s_ctx.vectors);
  if ((!flash_vector && (vector_address < SRAM_BASE ||
                         vector_address > SRAM_END - sizeof(s_ctx.vectors))) ||
      (vector_address & 0x1ffu) != 0u) return ESY_ERR_NOT_SUPPORTED;
  const uint32_t *original = (const uint32_t *)(uintptr_t)vector_address;
  extern void Default_Handler(void);
  for (uint32_t number = 3u; number <= 6u; ++number) {
    if (original[number] != (uint32_t)(uintptr_t)Default_Handler) return ESY_ERR_BUSY;
  }
  for (size_t i = 0u; i < FAULT_VECTOR_COUNT; ++i) s_ctx.vectors[i] = original[i];
  for (uint32_t number = 3u; number <= 6u; ++number) {
    s_ctx.vectors[number] = (uint32_t)(uintptr_t)fault_hardware_entry;
  }
  s_ctx.config = *config;
  s_ctx.claimed = 0u;
  s_ctx.initialized = true;
  __DSB();
  SCB->VTOR = (uint32_t)(uintptr_t)s_ctx.vectors;
  __DSB();
  __ISB();
  return ESY_OK;
}
