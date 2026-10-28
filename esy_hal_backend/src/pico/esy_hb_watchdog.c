/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_watchdog.h"

#include <stddef.h>

#include "hardware/watchdog.h"

#define ESY_HB_WATCHDOG_DEFAULT_TIMEOUT_MS 1000u

static const uint32_t k_watchdog_max_timeout_ms = 8000u;

typedef struct {
  uint32_t timeout_ms;
  bool pause_on_debug;
  bool configured;
  bool enabled;
  bool reset_acknowledged;
} esy_hb_watchdog_ctx_t;

/* One task owns lifecycle and reset acknowledgement; feeding is serialized
 * with lifecycle by that owner. Reset status belongs to this boot epoch. */
static esy_hb_watchdog_ctx_t s_ctx;

esy_err_t esy_hb_watchdog_init(uint32_t timeout_ms, bool pause_on_debug) {
  if (timeout_ms == 0u || timeout_ms > k_watchdog_max_timeout_ms) return ESY_ERR_INVALID_ARG;

  watchdog_enable(timeout_ms, pause_on_debug);
  s_ctx.timeout_ms = timeout_ms;
  s_ctx.pause_on_debug = pause_on_debug;
  s_ctx.configured = true;
  s_ctx.enabled = true;

  return ESY_OK;
}

esy_err_t esy_hb_watchdog_deinit(void) {
  watchdog_disable();
  s_ctx.enabled = false;
  return ESY_OK;
}

void esy_hb_watchdog_feed(void) {
  if (s_ctx.enabled) watchdog_update();
}

void esy_hb_watchdog_enable(void) {
  if (!s_ctx.configured) {
    s_ctx.timeout_ms = ESY_HB_WATCHDOG_DEFAULT_TIMEOUT_MS;
    s_ctx.pause_on_debug = false;
    s_ctx.configured = true;
  }
  watchdog_enable(s_ctx.timeout_ms, s_ctx.pause_on_debug);
  s_ctx.enabled = true;
}

bool esy_hb_watchdog_was_reset(void) {
  return !s_ctx.reset_acknowledged && watchdog_caused_reboot();
}

void esy_hb_watchdog_clear_reset(void) {
  /* RP2 reset-cause registers are read-only. Acknowledge the public status in
   * software without starting, feeding or reconfiguring the live counter. */
  s_ctx.reset_acknowledged = true;
}

esy_err_t esy_hb_watchdog_get_timeout_lower_bound_us(uint64_t *out_timeout_us) {
  if (out_timeout_us == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.enabled) return ESY_ERR_STATE;
  /* No board-independent minimum in an arbitrary monotonic clock domain has
   * been certified for this port. Nominal oscillator rates are insufficient. */
  return ESY_ERR_NOT_SUPPORTED;
}
