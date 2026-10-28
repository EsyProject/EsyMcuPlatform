/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_watchdog_monitor.h
 * @brief Reusable fake implementation contract for watchdog monitor tests.
 *
 * The fake is intended for single-threaded host tests. It records public API
 * calls, returns configurable results, and creates deterministic report tokens.
 */

#ifndef FAKE_ESY_WATCHDOG_MONITOR_H
#define FAKE_ESY_WATCHDOG_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Observable state and controls maintained by the reusable fake. */
typedef struct fake_esy_watchdog_monitor_ctx_s {
  esy_err_t stop_result, deinit_result, set_alert_result;
  esy_err_t deadline_result;
  uint64_t deadline_us;
  uint32_t deadline_query_count;
  esy_err_t init_result;
  esy_err_t start_result;
  esy_err_t register_result;
  esy_err_t enable_result;
  esy_err_t get_status_result;
  esy_err_t get_task_count_result;
  bool task_alive;

  esy_watchdog_monitor_config_t last_init_config;
  bool has_last_init_config;
  const char *last_task_name;
  esy_watchdog_report_t next_report;
  esy_watchdog_report_t last_report;
  esy_watchdog_alert_cb_t alert_cb;
  void *alert_user_ctx;

  uint32_t init_count;
  uint32_t start_count;
  uint32_t stop_count;
  uint32_t deinit_count;
  uint32_t register_count;
  uint32_t registered_task_count;
  uint32_t enable_count;
  uint32_t disable_count;
  uint32_t unregister_count;
  uint32_t get_status_count;
  uint32_t report_count;
  uint32_t get_task_count_count;
  uint32_t disable_hw_feed_count;
  uint32_t set_alert_count;
} fake_esy_watchdog_monitor_ctx_t;

/** @brief Reset fake state and restore successful default results. */
void fake_esy_watchdog_monitor_reset(void);

/**
 * @brief Return the mutable fake context used for controls and assertions.
 *
 * @return Process-wide fake context.
 */
fake_esy_watchdog_monitor_ctx_t *fake_esy_watchdog_monitor_get_ctx(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_WATCHDOG_MONITOR_H */
