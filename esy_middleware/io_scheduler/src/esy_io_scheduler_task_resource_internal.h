/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_IO_SCHEDULER_TASK_RESOURCE_INTERNAL_H
#define ESY_IO_SCHEDULER_TASK_RESOURCE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_io_scheduler_core_internal.h"
#include "esy_middleware/os/esy_os_task.h"

static inline bool esy_io_scheduler_task_stack_resource_is_valid(
    const void *task_stack_buffer,
    size_t task_stack_buffer_size,
    uint32_t stack_size_bytes) {
  if (!task_stack_buffer || stack_size_bytes == 0u ||
      task_stack_buffer_size < (size_t)stack_size_bytes) {
    return false;
  }

  return (((uintptr_t)task_stack_buffer % ESY_OS_TASK_STACK_ALIGN) == 0u);
}

#define ESY_IO_SCHEDULER_SHUTDOWN_WAIT_MS (100u)
#define ESY_IO_SCHEDULER_SHUTDOWN_POLL_MS (1u)

/* The lifecycle owner stops producers before requesting cooperative shutdown. */
static inline esy_err_t esy_io_scheduler_delete_parked(esy_os_task_t *task) {
  if (!task) return ESY_OK;
  if (esy_os_task_is_current(task)) return ESY_ERR_STATE;
  (void)esy_os_task_notify_give(task);
  for (uint32_t i = 0u; i < ESY_IO_SCHEDULER_SHUTDOWN_WAIT_MS; ++i) {
    esy_err_t err = esy_os_task_delete_parked(task);
    if (err != ESY_ERR_BUSY) return err;
    esy_os_task_delay_ms(ESY_IO_SCHEDULER_SHUTDOWN_POLL_MS);
  }
  return esy_os_task_delete_parked(task);
}

#endif /* ESY_IO_SCHEDULER_TASK_RESOURCE_INTERNAL_H */
