/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "snapshot_test_support.h"
#include <string.h>

static snapshot_test_support_ctx_t s_ctx;
void snapshot_test_support_reset(void) { memset(&s_ctx, 0, sizeof(s_ctx)); }
snapshot_test_support_ctx_t *snapshot_test_support_get(void) { return &s_ctx; }

void vTaskSuspendAll(void) {
  ++s_ctx.suspend_depth;
  ++s_ctx.suspend_calls;
}

int xTaskResumeAll(void) {
  --s_ctx.suspend_depth;
  ++s_ctx.resume_calls;
  if (s_ctx.suspend_depth == 0u && s_ctx.mutate_name_on_resume) {
    strcpy(s_ctx.native_name, "reused");
  }
  return 0;
}

UBaseType_t uxTaskGetSystemState(TaskStatus_t *items, UBaseType_t capacity,
                                configRUN_TIME_COUNTER_TYPE *total) {
  ++s_ctx.calls;
  s_ctx.received_storage = items;
  s_ctx.received_capacity = capacity;
  s_ctx.outer_depth_at_snapshot = s_ctx.suspend_depth;
  vTaskSuspendAll();
  UBaseType_t count = 0u;
  if (capacity >= s_ctx.native_count) {
    memcpy(items, s_ctx.native, sizeof(*items) * s_ctx.native_count);
    *total = (configRUN_TIME_COUNTER_TYPE)(((uint64_t)s_ctx.total_high << 32) | 123u);
    count = s_ctx.native_count;
  }
  (void)xTaskResumeAll();
  return count;
}
