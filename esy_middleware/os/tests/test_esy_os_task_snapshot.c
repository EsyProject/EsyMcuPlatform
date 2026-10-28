/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <string.h>
#include "task.h"
#include "esy_middleware/os/esy_os_task.h"

#include "snapshot_test_support.h"

void setUp(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  snapshot_test_support_reset();
  ctx->native_count = 1u;
  ctx->count = 99u;
  ctx->total = 99u;
}
void tearDown(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->suspend_depth);
  TEST_ASSERT_EQUAL_UINT32(ctx->suspend_calls, ctx->resume_calls);
}

#if configUSE_TRACE_FACILITY == 1 && \
    !(defined(ESY_PLATFORM_ESP32) && \
      (!defined(CONFIG_FREERTOS_UNICORE) || CONFIG_FREERTOS_UNICORE != 1))
void test_snapshot_maps_native_entries_using_caller_storage(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  ctx->native_count = ESY_OS_TASK_SYSTEM_STATE_MAX;
  for (unsigned i = 0; i < ctx->native_count; ++i) {
    ctx->native[i].pcTaskName = "worker";
    ctx->native[i].xHandle = &ctx->native[i];
    ctx->native[i].xTaskNumber = i;
    ctx->native[i].eCurrentState = (eTaskState)(i % 6u);
    ctx->native[i].uxCurrentPriority = 5u;
    ctx->native[i].uxBasePriority = 3u;
    ctx->native[i].ulRunTimeCounter = 41u;
    ctx->native[i].usStackHighWaterMark = 17u;
  }
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_task_get_system_state(
      ctx->items, ESY_OS_TASK_SYSTEM_STATE_MAX, &ctx->count, &ctx->total,
      &ctx->scratch));
  TEST_ASSERT_EQUAL_PTR(ctx->scratch.native.bytes, ctx->received_storage);
  TEST_ASSERT_EQUAL_UINT32(ESY_OS_TASK_SYSTEM_STATE_MAX, ctx->received_capacity);
  TEST_ASSERT_EQUAL_UINT32(ctx->native_count, ctx->count);
  TEST_ASSERT_EQUAL_UINT32(123u, ctx->total);
  for (unsigned i = 0; i < ctx->count; ++i) {
    TEST_ASSERT_EQUAL_PTR(ctx->native[i].xHandle, ctx->items[i].handle);
    TEST_ASSERT_EQUAL_STRING("worker", ctx->items[i].name);
    TEST_ASSERT_EQUAL_INT(i % 6u, ctx->items[i].state);
    TEST_ASSERT_EQUAL_UINT32(i, ctx->items[i].task_number);
    TEST_ASSERT_EQUAL_UINT32(5u, ctx->items[i].current_priority);
    TEST_ASSERT_EQUAL_UINT32(3u, ctx->items[i].base_priority);
    TEST_ASSERT_EQUAL_UINT32(41u, ctx->items[i].runtime_counter);
    TEST_ASSERT_EQUAL_UINT32(17u, ctx->items[i].stack_high_water_mark);
  }
}

void test_snapshot_rejects_invalid_capacity_and_alignment_without_native_call(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  const uint32_t capacities[] = {0u, ESY_OS_TASK_SYSTEM_STATE_MAX + 1u, UINT32_MAX};
  for (unsigned i = 0u; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_os_task_get_system_state(
        ctx->items, capacities[i], &ctx->count, &ctx->total, &ctx->scratch));
    TEST_ASSERT_EQUAL_UINT32(0u, ctx->count);
    TEST_ASSERT_EQUAL_UINT32(0u, ctx->total);
  }
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_os_task_get_system_state(
      NULL, 1u, NULL, NULL, &ctx->scratch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_os_task_get_system_state(
      ctx->items, 1u, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_os_task_get_system_state(
      ctx->items, 1u, NULL, NULL,
      (esy_os_task_snapshot_storage_t *)((uint8_t *)&ctx->scratch + 1u)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_os_task_get_system_state(
      (esy_os_task_status_t *)((uint8_t *)ctx->items + 1u), 1u,
      NULL, NULL, &ctx->scratch));
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->calls);
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->suspend_calls);
}

void test_snapshot_reports_insufficient_capacity_without_partial_output(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  ctx->native_count = 2u;
  ctx->items[0].task_number = 55u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NO_MEM, esy_os_task_get_system_state(
      ctx->items, 1u, &ctx->count, &ctx->total, &ctx->scratch));
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->count);
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->total);
  TEST_ASSERT_EQUAL_UINT32(55u, ctx->items[0].task_number);
  TEST_ASSERT_EQUAL_UINT32(2u, ctx->resume_calls);
}

void test_snapshot_copies_names_before_resume_and_survives_scratch_reuse(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  strcpy(ctx->native_name, "original");
  ctx->native[0].pcTaskName = ctx->native_name;
  ctx->mutate_name_on_resume = true;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_task_get_system_state(
      ctx->items, 1u, &ctx->count, &ctx->total, &ctx->scratch));
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->outer_depth_at_snapshot);
  TEST_ASSERT_EQUAL_STRING("reused", ctx->native_name);
  TEST_ASSERT_EQUAL_STRING("original", ctx->items[0].name);
  memset(&ctx->scratch, 0xA5, sizeof(ctx->scratch));
  memset(ctx->native, 0, sizeof(ctx->native));
  memset(ctx->native_name, 0, sizeof(ctx->native_name));
  TEST_ASSERT_EQUAL_STRING("original", ctx->items[0].name);
}

void test_snapshot_truncates_names_and_terminates_missing_names(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  ctx->native_count = 2u;
  memset(ctx->native_name, 'N', sizeof(ctx->native_name));
  ctx->native_name[sizeof(ctx->native_name) - 1u] = '\0';
  ctx->native[0].pcTaskName = ctx->native_name;
  ctx->native[1].pcTaskName = NULL;
  memset(ctx->items, 0xA5, sizeof(ctx->items));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_task_get_system_state(
      ctx->items, 2u, NULL, NULL, &ctx->scratch));
  TEST_ASSERT_EQUAL_UINT32(ESY_OS_TASK_NAME_BYTES - 1u,
                         strlen(ctx->items[0].name));
  TEST_ASSERT_EQUAL_MEMORY(ctx->native_name, ctx->items[0].name,
                           ESY_OS_TASK_NAME_BYTES - 1u);
  TEST_ASSERT_EQUAL_STRING("", ctx->items[1].name);
}

void test_snapshot_preserves_callers_existing_scheduler_suspension(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  vTaskSuspendAll();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_task_get_system_state(
      ctx->items, 1u, NULL, NULL, &ctx->scratch));
  TEST_ASSERT_EQUAL_UINT32(2u, ctx->outer_depth_at_snapshot);
  TEST_ASSERT_EQUAL_UINT32(1u, ctx->suspend_depth);
  (void)xTaskResumeAll();
}

void test_snapshot_reuses_workspace_and_accepts_optional_counters(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_task_get_system_state(
      ctx->items, 1u, NULL, NULL, &ctx->scratch));
  ctx->native[0].xTaskNumber = 9u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_task_get_system_state(
      ctx->items, 1u, NULL, NULL, &ctx->scratch));
  TEST_ASSERT_EQUAL_UINT32(9u, ctx->items[0].task_number);
  TEST_ASSERT_EQUAL_UINT32(2u, ctx->calls);
}

#else
void test_snapshot_reports_unsupported_without_touching_items(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  ctx->items[0].task_number = 55u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_os_task_get_system_state(
      ctx->items, 1u, &ctx->count, &ctx->total, &ctx->scratch));
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->count);
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->total);
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->calls);
  TEST_ASSERT_EQUAL_UINT32(0u, ctx->suspend_calls);
  TEST_ASSERT_EQUAL_UINT32(55u, ctx->items[0].task_number);
}
#endif
