/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_watchdog.h"

#include <string.h>

typedef struct {
  fake_esy_hal_watchdog_state_t state;
  esy_err_t init_result;
} fake_esy_hal_watchdog_ctx_t;
static fake_esy_hal_watchdog_ctx_t s_ctx;

fake_esy_hal_watchdog_state_t *fake_esy_hal_watchdog_get_state(void) {
  return &s_ctx.state;
}

void fake_esy_hal_watchdog_reset(void) {
  memset(&s_ctx.state, 0, sizeof(s_ctx.state));
  s_ctx.state.timeout_ms = 1000u;
  s_ctx.init_result = ESY_OK;
}

void fake_esy_hal_watchdog_set_init_result(esy_err_t ret) {
  s_ctx.init_result = ret;
}

void fake_esy_hal_watchdog_set_reset_flag(bool flag) {
  s_ctx.state.was_reset_flag = flag;
}

esy_err_t esy_hal_watchdog_init(uint32_t timeout_ms, bool pause_on_debug) {
  s_ctx.state.init_count++;
  if (s_ctx.init_result != ESY_OK) {
    return s_ctx.init_result;
  }

  s_ctx.state.initialized = true;
  s_ctx.state.enabled = true;
  s_ctx.state.timeout_lower_bound_us = (uint64_t)timeout_ms * 1000u;
  s_ctx.state.timeout_ms = timeout_ms;
  s_ctx.state.pause_on_debug = pause_on_debug;
  return ESY_OK;
}

esy_err_t esy_hal_watchdog_deinit(void) {
  s_ctx.state.deinit_count++;
  if (s_ctx.state.deinit_result != ESY_OK) return s_ctx.state.deinit_result;
  s_ctx.state.initialized = false;
  s_ctx.state.enabled = false;
  s_ctx.state.feed_count = 0u;
  return ESY_OK;
}

void esy_hal_watchdog_feed(void) {
  if (s_ctx.state.enabled) s_ctx.state.feed_count++;
  if (s_ctx.state.feed_hook) s_ctx.state.feed_hook();
}

esy_err_t esy_hal_watchdog_get_timeout_lower_bound_us(uint64_t *out_timeout_us) {
  if (out_timeout_us == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.state.initialized) return ESY_ERR_STATE;
  if (s_ctx.state.bound_result != ESY_OK) return s_ctx.state.bound_result;
  *out_timeout_us = s_ctx.state.timeout_lower_bound_us;
  return ESY_OK;
}

void esy_hal_watchdog_enable(void) {
  if (!s_ctx.state.initialized) {
    s_ctx.state.initialized = true;
    s_ctx.state.timeout_ms = 1000u;
    s_ctx.state.pause_on_debug = false;
  }
  s_ctx.state.enabled = true;
  s_ctx.state.enable_count++;
}

bool esy_hal_watchdog_was_reset(void) { return s_ctx.state.was_reset_flag; }

void esy_hal_watchdog_clear_reset(void) {
  s_ctx.state.was_reset_flag = false;
  s_ctx.state.clear_called = true;
}

__attribute__((constructor)) static void fake_esy_hal_watchdog_ctor(void) {
  fake_esy_hal_watchdog_reset();
}
