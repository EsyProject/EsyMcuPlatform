/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <string.h>
#include "esy_middleware/core_lockout/esy_core_lockout.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_system.h"

typedef struct {
  esy_activity_guard_t guard;
  esy_err_t acquire_result;
  uint32_t acquires;
  uint32_t releases;
  uint32_t operations;
  uint32_t phase;
} single_core_test_ctx_t;
static single_core_test_ctx_t s_ctx;

static esy_err_t acquire(void *ctx) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.phase);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.acquires++;
  s_ctx.phase = 1u;
  return s_ctx.acquire_result;
}

static void operation(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.phase);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.operations++;
  s_ctx.phase = 2u;
}

static esy_err_t release(void *ctx) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.phase);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.releases++;
  s_ctx.phase = 3u;
  return ESY_OK;
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.guard = (esy_activity_guard_t){&s_ctx, acquire, release};
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_system_reset();
}
void tearDown(void) {}

/* Two physical cores with one scheduled core require no peer workers.
 * The unscheduled peer is kept inactive by the consumer for this lifetime.
 */
void test_single_core_init_guards_operations_without_workers_or_flash_dependency(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_execute(operation, &s_ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_set_activity_guard(&s_ctx.guard));
  fake_esy_hal_system_get_ctx()->core_count = 2u;
  fake_esy_os_get_state()->core_count = 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_init());
  fake_esy_os_set_started(true);
  for (uint32_t i = 0u; i < 2u; ++i) {
    s_ctx.phase = 0u;
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_execute(operation, &s_ctx));
    TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.phase);
  }
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.acquires);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.releases);

  s_ctx.phase = 0u;
  s_ctx.acquire_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_core_lockout_execute(operation, &s_ctx));
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.acquires);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.releases);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_init());
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_set_activity_guard(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_core_lockout_set_activity_guard(&s_ctx.guard));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_yield_core_count());
}
