/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file snapshot_test_support.h
 * @brief Module-local native snapshot fixture for OS wrapper tests.
 */
#ifndef SNAPSHOT_TEST_SUPPORT_H
#define SNAPSHOT_TEST_SUPPORT_H
#include "task.h"
#include "esy_middleware/os/esy_os_task.h"

typedef struct {
  esy_os_task_snapshot_storage_t scratch;
  esy_os_task_status_t items[ESY_OS_TASK_SYSTEM_STATE_MAX];
  TaskStatus_t native[ESY_OS_TASK_SYSTEM_STATE_MAX];
  uint32_t count;
  uint32_t total;
  uint32_t total_high;
  uint32_t native_count;
  uint32_t calls;
  TaskStatus_t *received_storage;
  UBaseType_t received_capacity;
  uint32_t suspend_depth;
  uint32_t suspend_calls;
  uint32_t resume_calls;
  uint32_t outer_depth_at_snapshot;
  bool mutate_name_on_resume;
  char native_name[ESY_OS_TASK_NAME_BYTES + 8u];
} snapshot_test_support_ctx_t;
/** @brief Reset the native snapshot fixture. */
void snapshot_test_support_reset(void);
/** @brief Mutable test-only fixture state. */
snapshot_test_support_ctx_t *snapshot_test_support_get(void);
#endif
