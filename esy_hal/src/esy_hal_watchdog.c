/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_watchdog.h"

#include "esy_hal_backend/esy_hb_watchdog.h"

esy_err_t esy_hal_watchdog_init(uint32_t timeout_ms, bool pause_on_debug) {
  return esy_hb_watchdog_init(timeout_ms, pause_on_debug);
}

esy_err_t esy_hal_watchdog_deinit(void) {
  return esy_hb_watchdog_deinit();
}

esy_err_t esy_hal_watchdog_get_timeout_lower_bound_us(uint64_t *out_timeout_us) {
  return esy_hb_watchdog_get_timeout_lower_bound_us(out_timeout_us);
}

void esy_hal_watchdog_feed(void) { esy_hb_watchdog_feed(); }

void esy_hal_watchdog_enable(void) { esy_hb_watchdog_enable(); }

bool esy_hal_watchdog_was_reset(void) {
  return esy_hb_watchdog_was_reset();
}

void esy_hal_watchdog_clear_reset(void) {
  esy_hb_watchdog_clear_reset();
}
