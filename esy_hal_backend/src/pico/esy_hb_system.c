/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_system.h"

#include "esy_common/defs/system/esy_power_defs.h"

#include "pico/time.h"
#include "pico/stdio.h"
#include "pico/rand.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/structs/systick.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"

#include "esy_hb_internal.h"

#define ESY_CORTEX_SCB_ICSR_ADDRESS       0xe000ed04u
#define ESY_CORTEX_SCB_ICSR_PENDSTSET_Msk (1u << 26u)

static const uint32_t k_short_busy_wait_limit_us = 100u;

typedef struct {
  uint32_t saved_systick_csr;
  bool tick_suspended;
} esy_hb_system_ctx_t;

static esy_hb_system_ctx_t s_ctx;

esy_err_t esy_hb_system_init(const esy_system_config_t *config) {
  if (!config) return ESY_ERR_INVALID_ARG;

  if (config->sys_clock_khz >= 312000) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(5);
  }

  if (!set_sys_clock_khz(config->sys_clock_khz, false)) {
    return ESY_ERR_NOT_SUPPORTED;
  }

  stdio_init_all();

  return ESY_OK;
}

uint32_t esy_hb_system_clk_sys_get_hz(void) {
  return clock_get_hz(clk_sys);
}

esy_err_t esy_hb_system_random_bytes(uint8_t *buffer, size_t length) {
  if (buffer == NULL && length != 0u) return ESY_ERR_INVALID_ARG;
  while (length != 0u) {
    rng_128_t random;
    get_rand_128(&random);
    const size_t count = length < sizeof(random.r) ? length : sizeof(random.r);
    for (size_t i = 0u; i < count; ++i)
      buffer[i] = (uint8_t)(random.r[i / sizeof(random.r[0])] >>
                            ((i % sizeof(random.r[0])) * 8u));
    buffer += count;
    length -= count;
  }
  return ESY_OK;
}

void esy_hb_system_delay_ms(uint32_t ms) { sleep_ms(ms); }

void esy_hb_system_delay_us(uint32_t us) {
  if (us <= k_short_busy_wait_limit_us) {
    busy_wait_us_32(us);
    return;
  }
  sleep_us(us);
}

uint32_t esy_hb_system_get_tick(void) {
  return to_ms_since_boot(get_absolute_time());
}

void esy_hb_system_tick_from_isr(void) {
  /* The hardware timer advances without a forwarded system tick. */
}

uint32_t esy_hb_system_time_32_us(void) { return time_us_32(); }

esy_err_t esy_hb_system_always_on_time_us(uint64_t *out_time_us) {
  if (out_time_us == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_time_us = time_us_64();
  return ESY_OK;
}

uint32_t esy_hb_system_always_on_state_mask(void) {
  return ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
         ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE);
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
  reload = systick_hw->rvr + 1u;
  current = systick_hw->cvr;
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
  s_ctx.saved_systick_csr = systick_hw->csr;
  ret =
      esy_hb_system_get_tick_partial_us(tick_rate_hz, out_partial_tick_us);
  if (ret != ESY_OK) return ret;
  systick_hw->csr = 0u;
  s_ctx.tick_suspended = true;
  return ESY_OK;
}

void esy_hb_system_resume_tick(void) {
  if (s_ctx.tick_suspended) {
    systick_hw->cvr = 0u;
    systick_hw->csr = s_ctx.saved_systick_csr;
    s_ctx.tick_suspended = false;
  }
}

bool esy_hb_system_tick_is_pending(void) {
  const volatile uint32_t *icsr =
      (const volatile uint32_t *)ESY_CORTEX_SCB_ICSR_ADDRESS;
  return ((*icsr & ESY_CORTEX_SCB_ICSR_PENDSTSET_Msk) != 0u);
}

void esy_hb_system_disable_irq(void) { esy_disable_irq(); }

void esy_hb_system_enable_irq(void) { esy_enable_irq(); }

// clang-format off
__attribute__((noinline, section(".time_critical.esy_hb_system")))
uint32_t esy_hb_system_irq_save(void) {
  return save_and_disable_interrupts();
}

__attribute__((noinline, section(".time_critical.esy_hb_system")))
void esy_hb_system_irq_restore(uint32_t saved_status) {
  restore_interrupts(saved_status);
}

__attribute__((noinline, section(".time_critical.esy_hb_system")))
void esy_hb_system_memory_barrier(void) {
  __dmb();
}

__attribute__((noinline, section(".time_critical.esy_hb_system")))
void esy_hb_system_signal_event(void) {
  __sev();
}

__attribute__((noinline, section(".time_critical.esy_hb_system")))
void esy_hb_system_wait_for_event(void) {
  __wfe();
}
// clang-format on

void esy_hb_system_reset(void) {
  watchdog_reboot(0u, 0u, 0u);
  for (;;) {
  }
}

uintptr_t esy_hb_system_get_pc(void) {
  uintptr_t pc;
  __asm volatile("mov %0, pc" : "=r"(pc));
  return pc;
}

uint8_t esy_hb_system_get_core_id(void) { return get_core_num(); }

uint8_t esy_hb_system_get_core_count(void) { return NUM_CORES; }

bool esy_hb_system_is_in_isr(void) { return esy_is_in_isr(); }
