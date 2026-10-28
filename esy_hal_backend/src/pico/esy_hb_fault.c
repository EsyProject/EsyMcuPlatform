/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_fault.h"

#include <stddef.h>
#include "pico.h"
#include "hardware/exception.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/psm.h"
#include "hardware/regs/watchdog.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/scb.h"

#include "hardware/sync.h"

#if PICO_RP2350 && (!defined(__ARM_ARCH_8M_MAIN__) || !defined(__ARM_FEATURE_CMSE))
#error "The RP2350 fault port requires Arm secure-only execution"
#endif

#define FAULT_CORE_COUNT 2
#if PICO_RP2350
#define FAULT_CLAIM_STYLE 1
#define FAULT_HAS_STATUS 1
#define FAULT_LAST_VECTOR 7u
#else
#define FAULT_CLAIM_STYLE 2
#define FAULT_HAS_STATUS 0
#define FAULT_LAST_VECTOR 3u
#endif

static ESY_FAULT_RAM_FUNC __attribute__((noreturn)) void fault_port_reset(void) {
  watchdog_hw->ctrl = 0u;
  psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);
  watchdog_hw->scratch[4] = 0u;
  __asm volatile("dsb" ::: "memory");
  watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
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
  if (get_core_num() != 0u || PICO_NO_RAM_VECTOR_TABLE) return ESY_ERR_NOT_SUPPORTED;
  uintptr_t callback = (uintptr_t)config->capture & ~(uintptr_t)1u;
  if (callback < SRAM_BASE || callback >= SRAM_END ||
      (config->ctx != NULL && ((uintptr_t)config->ctx < SRAM_BASE ||
                               (uintptr_t)config->ctx >= SRAM_END))) return ESY_ERR_INVALID_ARG;
  uintptr_t vector = scb_hw->vtor;
  if (vector < SRAM_BASE || vector > SRAM_END - 8u * sizeof(uint32_t)) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  extern char __default_isrs_start, __default_isrs_end;
  for (uint32_t number = 3u; number <= FAULT_LAST_VECTOR; ++number) {
    uintptr_t current = (uintptr_t)exception_get_vtable_handler((enum exception_number)number) & ~(uintptr_t)1u;
    if (current < (uintptr_t)&__default_isrs_start ||
        current >= (uintptr_t)&__default_isrs_end) return ESY_ERR_BUSY;
  }
#if !PICO_RP2350
  /* Claim once during startup; a fatal reader tries the dedicated lock once.
   * It is never shared with ordinary SDK users or released before reset. */
  int claim = spin_lock_claim_unused(false);
  if (claim < 0) return ESY_ERR_NO_MEM;
  s_ctx.claim_lock = spin_lock_instance((uint)claim);
#endif
  s_ctx.config = *config;
  s_ctx.claimed = 0u;
  __asm volatile("dmb" ::: "memory");
  s_ctx.initialized = true;
  for (uint32_t number = 3u; number <= FAULT_LAST_VECTOR; ++number) {
    (void)exception_set_exclusive_handler((enum exception_number)number, fault_hardware_entry);
  }
  return ESY_OK;
}
