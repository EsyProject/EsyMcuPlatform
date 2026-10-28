/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake_esy_hal_system.h"
#include "esy_hal/esy_hal_system.h"

#include <stddef.h>
#include <string.h>

static fake_esy_hal_system_ctx_t s_ctx;

fake_esy_hal_system_ctx_t *fake_esy_hal_system_get_ctx(void) {
  return &s_ctx;
}

void fake_esy_hal_system_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.core_count = 1u;
  s_ctx.clk_sys_hz = 125000000u;
  s_ctx.always_on_state_mask = 1u;
  s_ctx.reset_hook = NULL;
}

void fake_esy_hal_system_clear_reset_requests(void) {
  s_ctx.reset_count = 0u;
}

uint32_t fake_esy_hal_system_get_reset_count(void) {
  return s_ctx.reset_count;
}

void fake_esy_hal_system_set_reset_hook(
    fake_esy_hal_system_reset_hook_t hook) {
  s_ctx.reset_hook = hook;
}

void fake_esy_hal_system_set_tick(uint32_t now_ms) {
  s_ctx.tick_ms = now_ms;
  s_ctx.time_us = (uint64_t)now_ms * 1000u;
}

void fake_esy_hal_system_advance_tick(uint32_t delta_ms) {
  s_ctx.tick_ms += delta_ms;
  s_ctx.time_us += (uint64_t)delta_ms * 1000u;
}

esy_err_t esy_hal_system_init(void) {
  s_ctx.inited = true;
  return ESY_OK;
}

esy_err_t esy_hal_system_random_bytes(uint8_t *buffer, size_t length) {
  if (buffer == NULL && length != 0u) return ESY_ERR_INVALID_ARG;
  if (length == 0u) return ESY_OK;
  ++s_ctx.random_count;
  if (s_ctx.random_result != ESY_OK) return s_ctx.random_result;
  ++s_ctx.random_sequence;
  for (size_t i = 0u; i < length; ++i)
    buffer[i] = (uint8_t)(s_ctx.random_sequence >> ((i % 8u) * 8u));
  return ESY_OK;
}

uint32_t esy_hal_system_clk_sys_get_hz(void) { return s_ctx.clk_sys_hz; }

void esy_hal_system_delay_ms(uint32_t ms) {
  s_ctx.last_delay_ms = ms;
  s_ctx.tick_ms += ms;
  s_ctx.time_us += (uint64_t)ms * 1000u;
}

void esy_hal_system_delay_us(uint32_t us) {
  s_ctx.last_delay_us = us;
  s_ctx.time_us += us;
  s_ctx.tick_ms = s_ctx.time_us / 1000u;
}

uint32_t esy_hal_system_get_tick(void) { return s_ctx.tick_ms; }

void esy_hal_system_tick_from_isr(void) {
  fake_esy_hal_system_advance_tick(1u);
}

uint32_t esy_hal_system_time_32_us(void) {
  return (uint32_t)s_ctx.time_us;
}

esy_err_t esy_hal_system_always_on_time_us(uint64_t *out_time_us) {
  if (out_time_us == NULL) return ESY_ERR_INVALID_ARG;
  *out_time_us = s_ctx.time_us;
  return ESY_OK;
}

uint32_t esy_hal_system_always_on_state_mask(void) {
  return s_ctx.always_on_state_mask;
}

esy_err_t esy_hal_system_get_tick_partial_us(
    uint32_t tick_rate_hz, uint32_t *out_partial_tick_us) {
  if ((tick_rate_hz == 0u) || (out_partial_tick_us == NULL) ||
      s_ctx.tick_suspended) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_partial_tick_us = s_ctx.partial_tick_us;
  return ESY_OK;
}

esy_err_t esy_hal_system_suspend_tick(uint32_t tick_rate_hz,
                                           uint32_t *out_partial_tick_us) {
  if ((tick_rate_hz == 0u) || (out_partial_tick_us == NULL) ||
      s_ctx.tick_suspended) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_partial_tick_us = s_ctx.partial_tick_us;
  s_ctx.tick_suspended = true;
  return ESY_OK;
}

void esy_hal_system_resume_tick(void) { s_ctx.tick_suspended = false; }

bool esy_hal_system_tick_is_pending(void) { return s_ctx.tick_pending; }

uint8_t esy_hal_system_get_core_id(void) { return s_ctx.core_id; }

uint8_t esy_hal_system_get_core_count(void) { return s_ctx.core_count; }

bool esy_hal_system_is_in_isr(void) { return s_ctx.is_in_isr; }

void esy_hal_system_reset(void) {
  s_ctx.reset_count++;
  if (s_ctx.reset_hook != NULL) {
    s_ctx.reset_hook();
  }
}

uint32_t esy_hal_system_irq_save(void) {
  uint32_t saved = s_ctx.irq_status;
  s_ctx.irq_status = 1u;
  if (s_ctx.irq_save_hook != NULL) s_ctx.irq_save_hook();
  return saved;
}

void esy_hal_system_irq_restore(uint32_t saved_status) {
  s_ctx.irq_status = saved_status;
}

void esy_hal_system_memory_barrier(void) {}

void esy_hal_system_signal_event(void) { s_ctx.signal_event_count++; }

void esy_hal_system_wait_for_event(void) {
  s_ctx.wait_event_count++;
  if (s_ctx.wait_event_hook != NULL) s_ctx.wait_event_hook();
}

uintptr_t esy_hal_system_get_pc(void) { return s_ctx.pc; }
