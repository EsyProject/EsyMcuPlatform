/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_system.h"

#include "esy_common/defs/system/esy_power_defs.h"

#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_clk_tree.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esy_middleware/os/esy_os_context.h"
#include "soc/rtc.h"

typedef struct {
  bool us_tick_enabled;
} esy_hb_system_ctx_t;

static esy_hb_system_ctx_t s_ctx;

esy_err_t esy_hb_system_init(const esy_system_config_t *config) {
  if (!config) {
    return ESY_ERR_INVALID_ARG;
  }

  s_ctx.us_tick_enabled = config->enable_us_tick;
  return ESY_OK;
}

uint32_t esy_hb_system_clk_sys_get_hz(void) {
  uint32_t cpu_freq_hz;
  esp_err_t ret = esp_clk_tree_src_get_freq_hz(
      SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_freq_hz);
  if (ret != ESP_OK) {
    return 240000000;
  }
  return cpu_freq_hz;
}

esy_err_t esy_hb_system_random_bytes(uint8_t *buffer, size_t length) {
  if (buffer == NULL && length != 0u) return ESY_ERR_INVALID_ARG;
  if (length == 0u) return ESY_OK;
  esp_fill_random(buffer, length);
  return ESY_OK;
}

void esy_hb_system_delay_ms(uint32_t ms) {
  /* Hardware delay is independent of scheduler state and tick frequency.
   * Chunking also prevents conversion overflow for long delays. */
  while (ms != 0u) {
    const uint32_t chunk = ms > 1000u ? 1000u : ms;
    esp_rom_delay_us(chunk * 1000u);
    ms -= chunk;
  }
}

void esy_hb_system_delay_us(uint32_t us) { esp_rom_delay_us(us); }

uint32_t esy_hb_system_get_tick(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

void esy_hb_system_tick_from_isr(void) {
  /* The native provider and hardware timer own their timebases. */
}

uint32_t esy_hb_system_time_32_us(void) {
  if (s_ctx.us_tick_enabled) {
    return (uint32_t)esp_timer_get_time();
  }
  return esy_hb_system_get_tick() * 1000;
}

esy_err_t esy_hb_system_always_on_time_us(uint64_t *out_time_us) {
  if (out_time_us == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_time_us = (uint64_t)esp_timer_get_time();
  return ESY_OK;
}

uint32_t esy_hb_system_always_on_state_mask(void) {
  return ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
         ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE);
}

esy_err_t esy_hb_system_get_tick_partial_us(uint32_t tick_rate_hz,
                                                uint32_t *out_partial_tick_us) {
  if ((tick_rate_hz == 0u) || (out_partial_tick_us == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_partial_tick_us = 0u;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_system_suspend_tick(uint32_t tick_rate_hz,
                                         uint32_t *out_partial_tick_us) {
  (void)tick_rate_hz;
  if (out_partial_tick_us == NULL) return ESY_ERR_INVALID_ARG;
  *out_partial_tick_us = 0u;
  return ESY_ERR_NOT_SUPPORTED;
}

void esy_hb_system_resume_tick(void) {}

bool esy_hb_system_tick_is_pending(void) { return false; }

void esy_hb_system_disable_irq(void) { esy_os_context_irq_disable(); }

void esy_hb_system_enable_irq(void) { esy_os_context_irq_enable(); }

uint32_t esy_hb_system_irq_save(void) {
  /* Local-core ISR/task nesting requires the SDK's previous-level token.
   * This masks interrupts only; cross-core objects need separate ownership. */
  return esy_os_context_irq_save();
}

void esy_hb_system_irq_restore(uint32_t saved_status) {
  esy_os_context_irq_restore(saved_status);
}

void esy_hb_system_memory_barrier(void) { __sync_synchronize(); }

void esy_hb_system_signal_event(void) {}

void esy_hb_system_wait_for_event(void) {}

void esy_hb_system_reset(void) { esp_restart(); }

uintptr_t esy_hb_system_get_pc(void) {
  uintptr_t pc;
  __asm volatile("mov %0, a0" : "=r"(pc));
  return pc;
}

uint8_t esy_hb_system_get_core_id(void) {
  return (uint8_t)esp_cpu_get_core_id();
}

uint8_t esy_hb_system_get_core_count(void) {
  return esy_os_context_core_count();
}

bool esy_hb_system_is_in_isr(void) { return esy_os_context_is_in_isr(); }
