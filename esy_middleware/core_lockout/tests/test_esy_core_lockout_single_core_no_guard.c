/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/core_lockout/esy_core_lockout.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_system.h"

typedef struct { uint32_t operations; } no_guard_test_ctx_t;
static no_guard_test_ctx_t s_ctx;
static void operation(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.operations++;
}
void setUp(void) {
  s_ctx.operations = 0u;
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_system_reset();
}
void tearDown(void) {}

void test_single_core_null_guard_executes_once_without_workers(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_set_activity_guard(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_init());
  fake_esy_os_set_started(true);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_execute(operation, &s_ctx));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_init());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_set_activity_guard(NULL));
}
