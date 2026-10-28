/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_BACKEND_OS_INTERNAL_H
#define ESY_LOGGER_BACKEND_OS_INTERNAL_H
#include "esy_logger_backend_runtime_internal.h"
#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/os/esy_os_task.h"
static inline bool esy_logger_task_resources_valid(const void *stack, uint32_t bytes,
    const esy_os_task_static_storage_t *control) {
  return stack && control && bytes > 0u && bytes <= 2048u &&
      (uintptr_t)stack % ESY_OS_TASK_STACK_ALIGN == 0u;
}
static inline esy_err_t esy_logger_delete_worker(void *task) {
  /* Before scheduler startup no worker can access its context or stack. */
  if (!esy_os_started()) {
    esy_os_task_delete(task);
    return ESY_OK;
  }
  if (esy_os_task_is_current(task)) return ESY_ERR_STATE;
  esy_err_t err = esy_os_task_notify_give(task);
  if (err != ESY_OK) return err;
  return esy_os_task_delete_parked(task);
}
static inline esy_err_t esy_logger_notify_worker(void *task) {
  if (esy_hal_system_is_in_isr()) {
    bool yield = false;
    esy_err_t err = esy_os_task_notify_give_from_isr(task, &yield);
    esy_os_yield_from_isr(yield);
    return err;
  }
  return esy_os_task_notify_give(task);
}
static inline esy_logger_backend_runtime_t esy_logger_os_runtime(void) {
  esy_logger_backend_runtime_t runtime = {
    .lock = esy_os_task_enter_crit_auto, .unlock = esy_os_task_exit_crit_auto,
    .start = NULL, .stop = esy_logger_delete_worker, .notify = esy_logger_notify_worker,
  };
  return runtime;
}
#endif /* ESY_LOGGER_BACKEND_OS_INTERNAL_H */
