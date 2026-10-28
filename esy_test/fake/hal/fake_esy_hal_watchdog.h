/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_hal_watchdog.h
 * @brief Reusable fake implementation contract for HAL watchdog tests.
 */

#ifndef FAKE_ESY_HAL_WATCHDOG_H
#define FAKE_ESY_HAL_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_hal/esy_hal_watchdog.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Observable fake watchdog state.
 */
typedef struct fake_esy_hal_watchdog_state_s {
  bool initialized;
  bool enabled;
  uint32_t timeout_ms;
  bool pause_on_debug;
  esy_err_t deinit_result;
  esy_err_t bound_result;
  uint64_t timeout_lower_bound_us;
  void (*feed_hook)(void);
  uint32_t init_count;
  uint32_t deinit_count;
  uint32_t enable_count;
  uint32_t feed_count;
  bool was_reset_flag;
  bool clear_called;
} fake_esy_hal_watchdog_state_t;

fake_esy_hal_watchdog_state_t *fake_esy_hal_watchdog_get_state(void);
void fake_esy_hal_watchdog_reset(void);
void fake_esy_hal_watchdog_set_init_result(esy_err_t ret);
void fake_esy_hal_watchdog_set_reset_flag(bool flag);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_WATCHDOG_H */
