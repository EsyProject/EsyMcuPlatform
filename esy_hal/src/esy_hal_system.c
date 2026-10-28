/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_system.h"

#include "esy_hal_backend/esy_hb_system.h"
#include "esy_common/defs/system/esy_system_defs.h"
#include "esy_board/esy_board.h"

esy_err_t esy_hal_system_init(void) {
  esy_system_config_t config = {0};

  esy_board_init();
  if (!esy_board_get_system_config(&config)) {
    return ESY_ERR_FAIL;
  }

  return esy_hb_system_init(&config);
}

uint32_t esy_hal_system_clk_sys_get_hz(void) {
  return esy_hb_system_clk_sys_get_hz();
}

esy_err_t esy_hal_system_random_bytes(uint8_t *buffer, size_t length) {
  if (buffer == NULL && length != 0u) return ESY_ERR_INVALID_ARG;
  if (length == 0u) return ESY_OK;
  return esy_hb_system_random_bytes(buffer, length);
}

void esy_hal_system_delay_ms(uint32_t ms) {
  esy_hb_system_delay_ms(ms);
}

void esy_hal_system_delay_us(uint32_t us) {
  esy_hb_system_delay_us(us);
}

uint32_t esy_hal_system_get_tick(void) {
  return esy_hb_system_get_tick();
}

void esy_hal_system_tick_from_isr(void) {
  esy_hb_system_tick_from_isr();
}

uint32_t esy_hal_system_time_32_us(void) {
  return esy_hb_system_time_32_us();
}

esy_err_t esy_hal_system_always_on_time_us(uint64_t *out_time_us) {
  return esy_hb_system_always_on_time_us(out_time_us);
}

uint32_t esy_hal_system_always_on_state_mask(void) {
  return esy_hb_system_always_on_state_mask();
}

esy_err_t esy_hal_system_get_tick_partial_us(
    uint32_t tick_rate_hz, uint32_t *out_partial_tick_us) {
  return esy_hb_system_get_tick_partial_us(tick_rate_hz,
                                               out_partial_tick_us);
}

esy_err_t esy_hal_system_suspend_tick(uint32_t tick_rate_hz,
                                           uint32_t *out_partial_tick_us) {
  return esy_hb_system_suspend_tick(tick_rate_hz, out_partial_tick_us);
}

void esy_hal_system_resume_tick(void) { esy_hb_system_resume_tick(); }

bool esy_hal_system_tick_is_pending(void) {
  return esy_hb_system_tick_is_pending();
}

extern uint8_t esy_hal_system_get_core_id(void) {
  return esy_hb_system_get_core_id();
}

extern uint8_t esy_hal_system_get_core_count(void) {
  return esy_hb_system_get_core_count();
}

extern bool esy_hal_system_is_in_isr(void) {
  return esy_hb_system_is_in_isr();
}

ESY_HAL_RAM_FUNC uint32_t esy_hal_system_irq_save(void) {
  return esy_hb_system_irq_save();
}

ESY_HAL_RAM_FUNC void esy_hal_system_irq_restore(
    uint32_t saved_status) {
  esy_hb_system_irq_restore(saved_status);
}

ESY_HAL_RAM_FUNC void esy_hal_system_memory_barrier(void) {
  esy_hb_system_memory_barrier();
}

ESY_HAL_RAM_FUNC void esy_hal_system_signal_event(void) {
  esy_hb_system_signal_event();
}

ESY_HAL_RAM_FUNC void esy_hal_system_wait_for_event(void) {
  esy_hb_system_wait_for_event();
}

void esy_hal_system_reset(void) { esy_hb_system_reset(); }

uintptr_t esy_hal_system_get_pc(void) { return esy_hb_system_get_pc(); }
