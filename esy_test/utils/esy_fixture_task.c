/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_fixture_task.h"

#include "fake_esy_os_queue.h"
#include "fake_esy_os_task.h"

#define ESY_FIXTURE_TASK_STACK_BYTES 1024u

typedef struct {
  uint8_t task_stacks[FAKE_ESY_OS_TASK_MAX_TASKS][ESY_FIXTURE_TASK_STACK_BYTES];
  esy_os_task_static_storage_t task_storage[FAKE_ESY_OS_TASK_MAX_TASKS];
  uint32_t next;
} esy_fixture_task_ctx_t;

static esy_fixture_task_ctx_t s_ctx;

void esy_fixture_reset_os_task_and_queue(void) {
  fake_esy_os_task_reset();
  fake_esy_os_queue_reset();
  s_ctx.next = 0u;
}

esy_err_t esy_fixture_ensure_task(const char *task_name,
                                  esy_os_task_func_t entry,
                                  void *arg,
                                  uint32_t stack_size,
                                  uint32_t priority,
                                  esy_os_task_t **inout_task) {
  if ((task_name == NULL) || (entry == NULL) || (inout_task == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  if (*inout_task != NULL) {
    return ESY_OK;
  }

  if ((stack_size == 0u) || (stack_size > ESY_FIXTURE_TASK_STACK_BYTES) ||
      (s_ctx.next >= FAKE_ESY_OS_TASK_MAX_TASKS)) {
    return ESY_ERR_INVALID_ARG;
  }

  uint32_t slot = s_ctx.next;
  esy_err_t ret = esy_os_task_create_static(task_name,
                                   entry,
                                   arg,
                                   stack_size,
                                   priority,
                                   s_ctx.task_stacks[slot],
                                   &s_ctx.task_storage[slot],
                                   inout_task);
  if (ret == ESY_OK) {
    s_ctx.next++;
  }
  return ret;
}

esy_err_t esy_fixture_ensure_worker_task(const char *task_name,
                                         esy_os_task_t **inout_task) {
  return esy_fixture_ensure_task(task_name,
                                 (esy_os_task_func_t)0x1,
                                 NULL,
                                 256u,
                                 1u,
                                 inout_task);
}

bool esy_fixture_get_task_notify_give_count(esy_os_task_t *task,
                                            uint32_t *out_count) {
  fake_esy_os_task_entry_t *table;
  uint32_t table_size;
  uint32_t i;

  if ((task == NULL) || (out_count == NULL)) {
    return false;
  }

  table = fake_esy_os_task_get_table();
  table_size = fake_esy_os_task_get_table_size();

  for (i = 0u; i < table_size; ++i) {
    if (table[i].used && (table[i].task_handle == task)) {
      *out_count = table[i].notify_give_count;
      return true;
    }
  }

  return false;
}
