/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_os_task.h
 * @brief Reusable fake implementation contract for esy_os_task tests.
 */

#ifndef FAKE_ESY_OS_TASK_H
#define FAKE_ESY_OS_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_middleware/os/esy_os_task.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FAKE_ESY_OS_TASK_MAX_TASKS
#define FAKE_ESY_OS_TASK_MAX_TASKS 8u
#endif

/**
 * @brief Observable fake task entry, serialized by the test owner.
 *
 * Task and ISR notification APIs share one default notification value/state.
 * Scheduling hooks run synchronously on the test thread; this fake does not
 * provide native blocking or concurrent host-thread synchronization.
 */
typedef struct fake_esy_os_task_entry_s {
  bool used;
  bool created_static;
  bool deleted;
  bool parked;

  esy_os_task_t *task_handle;
  const char *name;
  esy_os_task_func_t entry;
  void *arg;
  uint32_t priority;
  uint32_t stack_size;
  uint32_t stack_size_bytes;
  void *stack_buffer;
  esy_os_task_static_storage_t *static_storage;

  uint32_t notify_value; /**< Shared value, also used as the take count. */
  bool notify_pending; /**< Shared notification state, independent of value. */
  uint32_t notify_give_count; /**< Give calls, including ISR calls, since reset. */
  uint32_t notify_give_from_isr_count; /**< ISR subset of the give call count. */
  uint32_t notify_take_timeout_ms;
  bool notify_take_clear_count;

  int32_t stack_remaining_words;
  uint8_t core_affinity;
} fake_esy_os_task_entry_t;

fake_esy_os_task_entry_t *fake_esy_os_task_get_table(void);
uint32_t fake_esy_os_task_get_table_size(void);
fake_esy_os_task_entry_t *fake_esy_os_task_find(esy_os_task_t *task);
fake_esy_os_task_entry_t *fake_esy_os_task_get_last_created(void);
fake_esy_os_task_entry_t *fake_esy_os_task_get_last_deleted(void);

void fake_esy_os_task_reset(void);
/** @brief Configure cooperative parking support for lifecycle tests. */
void fake_esy_os_task_set_park_supported(bool supported);
/** @brief Invoke a test hook after the current task becomes parked. */
void fake_esy_os_task_set_park_hook(void (*hook)(void));
/** @brief Invoke a one-shot scheduling hook after leaving the outer critical section. */
void fake_esy_os_task_set_critical_exit_hook(void (*hook)(void));
void fake_esy_os_task_set_create_result(esy_err_t ret);
/** @brief Fail one numbered static-create call after reset (zero disables). */
void fake_esy_os_task_set_create_static_failure(uint32_t call, esy_err_t result);
uint32_t fake_esy_os_task_get_create_static_count(void);
uint32_t fake_esy_os_task_get_delete_count(void);
/** @brief Number of explicit peer-core scheduling requests since reset. */
uint32_t fake_esy_os_task_get_yield_core_count(void);

void fake_esy_os_task_set_current(esy_os_task_t *task);
esy_os_task_t *fake_esy_os_task_get_current(void);

uint32_t fake_esy_os_task_get_critical_nesting(void);
void fake_esy_os_task_set_stack_remaining(esy_os_task_t *task, int32_t words);
uint32_t fake_esy_os_task_get_total_delay_ms(void);

/** @brief Set an optional synchronous scheduling hook after task notification.
 * Hooks run on the calling test thread and are cleared by reset. Tests must
 * avoid recursive invocation of the same hooked operation. NULL disables it.
 */
void fake_esy_os_task_set_notify_give_hook(void (*hook)(esy_os_task_t *task));
/**
 * @brief Set a synchronous hook before wait/take inspects the shared slot.
 * Value-based wait applies its entry-clear mask before the hook when no
 * notification was pending. Hooks may publish a task or ISR notification to
 * model an arrival while waiting. Avoid recursively waiting in the hook.
 */
void fake_esy_os_task_set_notify_take_hook(void (*hook)(void));
/** @brief Set a hook after delay accounting, for clock or scheduler simulation. */
void fake_esy_os_task_set_delay_hook(void (*hook)(uint32_t ms));

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_OS_TASK_H */
