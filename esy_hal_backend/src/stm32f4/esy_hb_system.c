/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_system.h"

#include "esy_common/defs/system/esy_power_defs.h"

#include "stm32f4xx_hal.h"

#define SYSTEM_US_PER_SECOND 1000000u
#define SYSTEM_DELAY_MAX_CYCLES (UINT32_MAX / 2u)

typedef struct {
  uint32_t saved_systick_ctrl;
  bool tick_suspended;
} esy_hb_system_ctx_t;

static esy_hb_system_ctx_t s_ctx;

/* Startup owns enabling the shared cycle counter. Preserve its value and other
 * trace settings so profiling and STOP clock restoration can share the reader. */
static esy_err_t cycle_counter_init(void) {
  const uint32_t saved_demcr = CoreDebug->DEMCR;
  const uint32_t saved_ctrl = DWT->CTRL;

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0u) {
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    const uint32_t start = DWT->CYCCNT;
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    if (DWT->CYCCNT != start) return ESY_OK;
  }

  if ((saved_ctrl & DWT_CTRL_CYCCNTENA_Msk) == 0u)
    DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk;
  if ((saved_demcr & CoreDebug_DEMCR_TRCENA_Msk) == 0u)
    CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_system_init(const esy_system_config_t *config) {
  if (!config) {
    return ESY_ERR_INVALID_ARG;
  }

  if (HAL_Init() != HAL_OK) return ESY_ERR_IO;

  /* Delay support is unconditional; the optional timestamp setting does not
   * change this port's millisecond-resolution system time query. */
  (void)config->enable_us_tick;
  return cycle_counter_init();
}

uint32_t esy_hb_system_clk_sys_get_hz(void) {
  return HAL_RCC_GetSysClockFreq();
}

esy_err_t esy_hb_system_random_bytes(uint8_t *buffer, size_t length) {
  if (buffer == NULL && length != 0u) return ESY_ERR_INVALID_ARG;
  return length == 0u ? ESY_OK : ESY_ERR_NOT_SUPPORTED;
}

void esy_hb_system_delay_ms(uint32_t ms) { HAL_Delay(ms); }

void esy_hb_system_delay_us(uint32_t us) {
  if (us == 0u) return;

  /* HCLK is the processor clock, which may differ from SYSCLK after the AHB
   * divider. The clock owner keeps SystemCoreClock current and fixed per call.
   * Round upward and retain 64-bit arithmetic for the complete uint32_t range. */
  uint64_t cycles = ((uint64_t)HAL_RCC_GetHCLKFreq() * us +
                     SYSTEM_US_PER_SECOND - 1u) / SYSTEM_US_PER_SECOND;
  while (cycles != 0u) {
    const uint32_t chunk = (cycles > SYSTEM_DELAY_MAX_CYCLES)
                               ? SYSTEM_DELAY_MAX_CYCLES : (uint32_t)cycles;
    const uint32_t start = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - start) < chunk) {
      /* Unsigned subtraction tolerates wrap; do not mask IRQs or reset CYCCNT. */
    }
    cycles -= chunk;
  }
}

uint32_t esy_hb_system_get_tick(void) { return HAL_GetTick(); }

void esy_hb_system_tick_from_isr(void) { HAL_IncTick(); }

uint32_t esy_hb_system_time_32_us(void) { return HAL_GetTick() * 1000u; }

esy_err_t esy_hb_system_always_on_time_us(uint64_t *out_time_us) {
  if (out_time_us == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_time_us = 0u;
  return ESY_ERR_NOT_SUPPORTED;
}

uint32_t esy_hb_system_always_on_state_mask(void) {
  return ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE);
}

esy_err_t esy_hb_system_get_tick_partial_us(uint32_t tick_rate_hz,
                                                uint32_t *out_partial_tick_us) {
  uint32_t reload;
  uint32_t current;
  uint64_t elapsed_counts;
  uint64_t partial_tick_us;
  uint64_t scaled_tick_counts;
  if ((tick_rate_hz == 0u) || (out_partial_tick_us == NULL) ||
      s_ctx.tick_suspended) {
    return ESY_ERR_INVALID_ARG;
  }
  reload = SysTick->LOAD + 1u;
  current = SysTick->VAL;
  if ((reload == 0u) || (current > reload)) {
    return ESY_ERR_STATE;
  }
  elapsed_counts = reload - current;
  scaled_tick_counts = (uint64_t)reload * tick_rate_hz;
  partial_tick_us = ((elapsed_counts * 1000000u) + scaled_tick_counts - 1u) /
                    scaled_tick_counts;
  *out_partial_tick_us = (uint32_t)partial_tick_us;
  return ESY_OK;
}

esy_err_t esy_hb_system_suspend_tick(uint32_t tick_rate_hz,
                                         uint32_t *out_partial_tick_us) {
  esy_err_t ret;
  if ((out_partial_tick_us == NULL) || s_ctx.tick_suspended) {
    return ESY_ERR_INVALID_ARG;
  }
  s_ctx.saved_systick_ctrl = SysTick->CTRL;
  ret =
      esy_hb_system_get_tick_partial_us(tick_rate_hz, out_partial_tick_us);
  if (ret != ESY_OK) return ret;
  SysTick->CTRL = 0u;
  s_ctx.tick_suspended = true;
  return ESY_OK;
}

void esy_hb_system_resume_tick(void) {
  if (s_ctx.tick_suspended) {
    SysTick->VAL = 0u;
    SysTick->CTRL = s_ctx.saved_systick_ctrl;
    s_ctx.tick_suspended = false;
  }
}

bool esy_hb_system_tick_is_pending(void) {
  return (SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) != 0u;
}

void esy_hb_system_disable_irq(void) { __disable_irq(); }

void esy_hb_system_enable_irq(void) { __enable_irq(); }

uint32_t esy_hb_system_irq_save(void) {
  uint32_t saved_status = __get_PRIMASK();
  __disable_irq();
  return saved_status;
}

void esy_hb_system_irq_restore(uint32_t saved_status) {
  __set_PRIMASK(saved_status);
}

void esy_hb_system_memory_barrier(void) { __DMB(); }

void esy_hb_system_signal_event(void) { __SEV(); }

void esy_hb_system_wait_for_event(void) { __WFE(); }

void esy_hb_system_reset(void) {
  NVIC_SystemReset();
  for (;;) {
  }
}

uintptr_t esy_hb_system_get_pc(void) {
  uintptr_t pc;
  __asm volatile("mov %0, pc" : "=r"(pc));
  return pc;
}

uint8_t esy_hb_system_get_core_id(void) { return 0; }

uint8_t esy_hb_system_get_core_count(void) { return 1; }

extern bool esy_hb_system_is_in_isr(void) { return (__get_IPSR() != 0U); }
