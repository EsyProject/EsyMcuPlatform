/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_low_power.h"

#include "esy_hb_esp32_context.h"

#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "hal/wdt_hal.h"
#include "sdkconfig.h"

typedef struct {
  bool timer_owned;
} esy_hb_low_power_ctx_t;

/* A single system task owns explicit sleep and all native wake configuration.
 * The SDK, rather than the generic atomic idle path, owns its cross-core, timer
 * and clock locks. No IRQ or callback reads this context. */
static esy_hb_low_power_ctx_t s_ctx;

static esy_err_t provider_error(esp_err_t error) {
  switch (error) {
    case ESP_OK: return ESY_OK;
    case ESP_ERR_INVALID_ARG: return ESY_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return ESY_ERR_STATE;
    case ESP_ERR_NOT_SUPPORTED: return ESY_ERR_NOT_SUPPORTED;
    default: return ESY_ERR_IO;
  }
}

static esy_err_t check_provider_context(void) {
  if (!esy_hb_esp32_can_block()) return ESY_ERR_STATE;
#if CONFIG_PM_ENABLE
  esp_pm_config_t configuration;
  const esp_err_t error = esp_pm_get_configuration(&configuration);
  if (error != ESP_OK) return provider_error(error);
  if (configuration.light_sleep_enable) return ESY_ERR_STATE;
#endif
  /* A nominal RTC rate cannot certify the monitor's remaining hardware budget.
   * Do not silently disable/feed an enabled watchdog or alter its owner state. */
  wdt_hal_context_t watchdog = RWDT_HAL_CONTEXT_DEFAULT();
  return wdt_hal_is_enabled(&watchdog) ? ESY_ERR_BUSY : ESY_OK;
}

static esy_err_t retire_timer(void) {
  const esp_err_t error = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    return provider_error(error);
  s_ctx.timer_owned = false;
  return ESY_OK;
}

esy_err_t esy_hb_power_sleep(const esy_power_sleep_config_t *config,
                             esy_power_sleep_result_t *out_result) {
  if (config == NULL || out_result == NULL ||
      config->state != ESY_POWER_STATE_RETENTION || config->duration_us == 0u)
    return ESY_ERR_INVALID_ARG;
  *out_result = (esy_power_sleep_result_t){0};
  if (!esy_hb_esp32_can_block()) return ESY_ERR_STATE;
  /* Retain ownership on cleanup failure. A retry cannot accidentally resleep. */
  if (s_ctx.timer_owned) return retire_timer();
  esy_err_t result = check_provider_context();
  if (result != ESY_OK) return result;
  const int64_t before_us = esp_timer_get_time();
  if (before_us < 0) return ESY_ERR_IO;
  esp_err_t error = esp_sleep_enable_timer_wakeup(config->duration_us);
  if (error != ESP_OK) return provider_error(error);
  s_ctx.timer_owned = true;

  /* This is an ordinary, schedulable task call. The SDK may take locks and use
   * IPC; entering here after masking IRQs or parking its peer can deadlock.
   * Native PM locks do not protect manual sleep: the caller's system barrier
   * must already exclude radio, Flash/OTA, driver activity and OS-time waits. */
  error = esp_light_sleep_start();
  /* The pinned SDK aliases these sleep results to generic argument/state
   * errors. Interpret them only at the sleep entry call, not at setup/cleanup. */
  result = error == ESP_ERR_SLEEP_REJECT ||
           error == ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION
      ? ESY_ERR_BUSY : provider_error(error);
  out_result->entered = error == ESP_OK;
  if (out_result->entered) {
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    out_result->timer_wakeup = cause == ESP_SLEEP_WAKEUP_TIMER;
    out_result->gpio_wakeup = cause == ESP_SLEEP_WAKEUP_GPIO;
  }
  const int64_t after_us = esp_timer_get_time();
  if (after_us < before_us) {
    result = ESY_ERR_IO;
  } else {
    out_result->elapsed_us = (uint64_t)(after_us - before_us);
  }
  const esy_err_t cleanup = retire_timer();
  return cleanup == ESY_OK ? result : cleanup;
}

esy_err_t esy_hb_low_power_get_capabilities(
    esy_hb_low_power_capabilities_t *out_capabilities) {
  if (out_capabilities == NULL) return ESY_ERR_INVALID_ARG;
  /* Explicit task sleep does not qualify any generic atomic idle state. */
  *out_capabilities = (esy_hb_low_power_capabilities_t){0};
  return ESY_OK;
}

esy_err_t esy_hb_low_power_prepare(esy_hb_low_power_mode_t mode) {
  return (uint32_t)mode >= (uint32_t)ESY_HB_LOW_POWER_MODE_COUNT
             ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_low_power_enter(esy_hb_low_power_mode_t mode,
                                 esy_hb_low_power_result_t *out_result) {
  if (out_result == NULL) return ESY_ERR_INVALID_ARG;
  *out_result = (esy_hb_low_power_result_t){0};
  return (uint32_t)mode >= (uint32_t)ESY_HB_LOW_POWER_MODE_COUNT
             ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_low_power_cancel(esy_hb_low_power_mode_t mode) {
  return (uint32_t)mode >= (uint32_t)ESY_HB_LOW_POWER_MODE_COUNT
             ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_shutdown(esy_hb_shutdown_mode_t mode) {
  if (mode != ESY_HB_SHUTDOWN_MODE_STANDBY) return ESY_ERR_INVALID_ARG;
  if (s_ctx.timer_owned) return ESY_ERR_BUSY;
  const esy_err_t result = check_provider_context();
  if (result != ESY_OK) return result;
  /* Preserve the exclusive owner's validated native wake configuration.
   * Deep-sleep is reset-class; no live manager/task state is resumed. */
  esp_deep_sleep_start();
  return ESY_ERR_STATE;
}
