/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_os_timer.h
 * @brief Reusable fake implementation contract for esy_os_timer tests.
 */

#ifndef FAKE_ESY_OS_TIMER_H
#define FAKE_ESY_OS_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_middleware/os/esy_os_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FAKE_ESY_OS_TIMER_MAX_TIMERS
#define FAKE_ESY_OS_TIMER_MAX_TIMERS 8u
#endif

/**
 * @brief Observable fake timer entry.
 */
typedef struct fake_esy_os_timer_entry_s {
  bool used;
  bool created_static;
  bool started;
  bool stopped;
  bool deleted;
  bool dispatching; /**< A callback currently borrows arg/storage. */
  bool cleanup_pending; /**< A stop/delete command awaits its fence and retry. */
  bool cleanup_deleting; /**< Pending operation is delete rather than stop. */
  bool cleanup_complete; /**< The fake daemon has passed the cleanup fence. */

  esy_os_timer_t *timer_handle;
  const char *name;
  uint32_t period_ms;
  bool auto_reload;
  void *arg;
  esy_os_timer_callback_t callback;
  esy_os_timer_static_storage_t *static_storage;

  uint32_t start_count;
  uint32_t stop_count;
} fake_esy_os_timer_entry_t;

fake_esy_os_timer_entry_t *fake_esy_os_timer_get_table(void);
uint32_t fake_esy_os_timer_get_table_size(void);
fake_esy_os_timer_entry_t *fake_esy_os_timer_find(esy_os_timer_t *timer);
fake_esy_os_timer_entry_t *fake_esy_os_timer_get_last_created(void);
fake_esy_os_timer_entry_t *fake_esy_os_timer_get_last_deleted(void);

void fake_esy_os_timer_reset(void);
void fake_esy_os_timer_set_create_result(esy_err_t ret);
void fake_esy_os_timer_set_start_result(esy_err_t ret);
/** @brief Inject command-admission status; errors retain all ownership. */
void fake_esy_os_timer_set_cleanup_result(esy_err_t result);
/** @brief Advance queued cleanup commands and fences outside timer callbacks.
 * Cleanup first returns BUSY. Call this between retries; only a later matching
 * stop/delete returning OK permits reuse. No progress occurs inside any callback.
 * This deterministic single-threaded fake does not model native scheduling. */
void fake_esy_os_timer_advance_daemon(void);
uint32_t fake_esy_os_timer_get_create_static_count(void);
uint32_t fake_esy_os_timer_get_delete_count(void);

void fake_esy_os_timer_fire(esy_os_timer_t *timer);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_OS_TIMER_H */
