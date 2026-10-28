/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_watchdog.h"

#include <stddef.h>

#include <stdint.h>

#include "stm32f4xx_hal.h"

#define ESY_HB_WATCHDOG_DEFAULT_TIMEOUT_MS 1000u

#ifndef ENABLE_ESY_QEMU
typedef struct {
  IWDG_HandleTypeDef hiwdg;
  bool initialized;
} esy_hb_watchdog_ctx_t;

/* One serialized owner controls configuration, feeding and lifecycle. IWDG
 * cannot be stopped after successful initialization. */
static esy_hb_watchdog_ctx_t s_ctx;
#endif

static const uint32_t k_watchdog_max_timeout_ms = 8190u;

esy_err_t esy_hb_watchdog_init(uint32_t timeout_ms, bool pause_on_debug) {
  if (timeout_ms == 0 || timeout_ms > k_watchdog_max_timeout_ms) {
    return ESY_ERR_INVALID_ARG;
  }

#ifdef ENABLE_ESY_QEMU
  (void)pause_on_debug;
  return ESY_OK;
#else
  __HAL_RCC_LSI_ENABLE();
  while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == RESET) {
  }

  uint32_t lsi_freq = 32000;
  uint32_t prescaler = IWDG_PRESCALER_64;
  uint64_t reload_64 = ((uint64_t)timeout_ms * (uint64_t)lsi_freq) / (64ULL * 1000ULL);
  uint32_t reload = (reload_64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)reload_64;

  if (reload > 0x0FFF) reload = 0x0FFF;

  s_ctx.hiwdg.Instance = IWDG;
  s_ctx.hiwdg.Init.Prescaler = prescaler;
  s_ctx.hiwdg.Init.Reload = reload;

  if (HAL_IWDG_Init(&s_ctx.hiwdg) != HAL_OK) return ESY_ERR_IO;
  s_ctx.initialized = true;

#if defined(DBGMCU_APB1_FZ_DBG_IWDG_STOP)
  if (pause_on_debug) {
    __HAL_DBGMCU_FREEZE_IWDG();
  } else {
    __HAL_DBGMCU_UNFREEZE_IWDG();
  }
#else
  (void)pause_on_debug;
#endif

  return ESY_OK;
#endif
}

esy_err_t esy_hb_watchdog_deinit(void) {
  /* IWDG cannot be stopped once enabled. Do not claim cleanup succeeded. */
#ifdef ENABLE_ESY_QEMU
  return ESY_OK;
#else
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

void esy_hb_watchdog_feed(void) {
#ifndef ENABLE_ESY_QEMU
  if (s_ctx.hiwdg.Instance != NULL) HAL_IWDG_Refresh(&s_ctx.hiwdg);
#endif
}

void esy_hb_watchdog_enable(void) {
#ifndef ENABLE_ESY_QEMU
  if (!s_ctx.initialized) {
    (void)esy_hb_watchdog_init(ESY_HB_WATCHDOG_DEFAULT_TIMEOUT_MS, false);
  }
#endif
}

bool esy_hb_watchdog_was_reset(void) {
#ifdef ENABLE_ESY_QEMU
  return false;
#else
  return (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET);
#endif
}

void esy_hb_watchdog_clear_reset(void) {
#ifndef ENABLE_ESY_QEMU
  __HAL_RCC_CLEAR_RESET_FLAGS();
#endif
}

esy_err_t esy_hb_watchdog_get_timeout_lower_bound_us(uint64_t *out_timeout_us) {
  if (out_timeout_us == NULL) return ESY_ERR_INVALID_ARG;
  /* No board-independent minimum in an arbitrary monotonic clock domain has
   * been certified for this port. Nominal oscillator rates are insufficient. */
  return ESY_ERR_NOT_SUPPORTED;
}
