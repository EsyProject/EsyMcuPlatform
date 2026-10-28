/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_watchdog.h"

#include <stddef.h>

#include <stdint.h>

#include "hal/wdt_hal.h"
#include "esp_system.h"

typedef struct {
  bool initialized;
  bool reset_acknowledged;
  wdt_hal_context_t rtc_wdt_ctx;
} esy_hb_watchdog_ctx_t;

static esy_hb_watchdog_ctx_t s_ctx = {
    .rtc_wdt_ctx = {.inst = WDT_RWDT, .rwdt_dev = &RTCCNTL},
};

static const uint32_t k_watchdog_cycles_per_ms = 150u;

esy_err_t esy_hb_watchdog_init(uint32_t timeout_ms, bool pause_on_debug) {
    (void)pause_on_debug;

    if (timeout_ms == 0 || timeout_ms > UINT32_MAX / k_watchdog_cycles_per_ms) {
        return ESY_ERR_INVALID_ARG;
    }

    if (s_ctx.initialized) {
        return ESY_ERR_STATE;
    }

    wdt_hal_write_protect_disable(&s_ctx.rtc_wdt_ctx);
    wdt_hal_disable(&s_ctx.rtc_wdt_ctx);

    uint64_t rtc_cycles_64 = (uint64_t)timeout_ms * k_watchdog_cycles_per_ms;
    uint32_t rtc_cycles = (rtc_cycles_64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)rtc_cycles_64;
    wdt_hal_config_stage(&s_ctx.rtc_wdt_ctx, WDT_STAGE0, rtc_cycles,
                         WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_write_protect_enable(&s_ctx.rtc_wdt_ctx);

    wdt_hal_write_protect_disable(&s_ctx.rtc_wdt_ctx);
    wdt_hal_feed(&s_ctx.rtc_wdt_ctx);
    wdt_hal_enable(&s_ctx.rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&s_ctx.rtc_wdt_ctx);
    s_ctx.initialized = true;

    return ESY_OK;
}

esy_err_t esy_hb_watchdog_deinit(void) {
    if (!s_ctx.initialized) {
        return ESY_OK;
    }

    wdt_hal_write_protect_disable(&s_ctx.rtc_wdt_ctx);
    wdt_hal_disable(&s_ctx.rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&s_ctx.rtc_wdt_ctx);
    s_ctx.initialized = false;
    return ESY_OK;
}

void esy_hb_watchdog_feed(void) {
    if (!s_ctx.initialized) {
        return;
    }

    wdt_hal_write_protect_disable(&s_ctx.rtc_wdt_ctx);
    wdt_hal_feed(&s_ctx.rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&s_ctx.rtc_wdt_ctx);
}

void esy_hb_watchdog_enable(void) {
    if (!s_ctx.initialized) {
        esy_hb_watchdog_init(1000, false);
    }

    wdt_hal_write_protect_disable(&s_ctx.rtc_wdt_ctx);
    wdt_hal_enable(&s_ctx.rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&s_ctx.rtc_wdt_ctx);
}

bool esy_hb_watchdog_was_reset(void) {
    esp_reset_reason_t reset_reason = esp_reset_reason();

    return !s_ctx.reset_acknowledged && (reset_reason == ESP_RST_WDT);
}

void esy_hb_watchdog_clear_reset(void) {
    /* The SDK reset reason is fixed for this boot. Acknowledge it without
     * touching the watchdog configuration, counter or diagnostic register. */
    s_ctx.reset_acknowledged = true;
}

esy_err_t esy_hb_watchdog_get_timeout_lower_bound_us(uint64_t *out_timeout_us) {
  if (out_timeout_us == NULL) return ESY_ERR_INVALID_ARG;
  /* No board-independent minimum in an arbitrary monotonic clock domain has
   * been certified for this port. Nominal oscillator rates are insufficient. */
  return ESY_ERR_NOT_SUPPORTED;
}
