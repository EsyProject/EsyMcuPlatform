/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/core_lockout/esy_core_lockout_core.h"
#include "esy_middleware/core_lockout/esy_core_lockout.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_system.h"
#include "unity.h"
#include <string.h>

typedef struct {
  esy_activity_guard_t guard;
  esy_err_t acquire_error, release_error;
  uint32_t acquires, releases, operations;
  bool check_reentry;
} core_lockout_core_test_ctx_t;
static core_lockout_core_test_ctx_t s_ctx;

static void operation(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_EQUAL(1u, fake_esy_hal_system_get_ctx()->irq_status);
  ++s_ctx.operations;
  if (s_ctx.check_reentry) {
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_core_lockout_execute(operation, &s_ctx));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_core_init());
  }
}
static esy_err_t acquire(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_system_get_ctx()->irq_status);
  ++s_ctx.acquires;
  if (s_ctx.check_reentry) {
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_core_lockout_execute(operation, &s_ctx));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_set_activity_guard(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_init());
  }
  return s_ctx.acquire_error;
}
static esy_err_t release(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_system_get_ctx()->irq_status);
  ++s_ctx.releases;
  if (s_ctx.check_reentry) {
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_core_lockout_execute(operation, &s_ctx));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_core_init());
  }
  return s_ctx.release_error;
}
void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.guard = (esy_activity_guard_t){&s_ctx, acquire, release};
  fake_esy_hal_system_reset();
  fake_esy_os_reset();
  fake_esy_os_task_reset();
}
void tearDown(void) {
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL(0u, fake_esy_os_task_get_yield_core_count());
}

/* One lifetime covers core ownership and a permanent fault. The
 * physical peer count is two, but the consumer promises the peer is inactive. */
void test_core_lockout_core_guards_one_owner_without_os_and_latches_uncertain_release(void) {
  fake_esy_hal_system_ctx_t *system = fake_esy_hal_system_get_ctx();
  system->core_count = 2u;
  system->is_in_isr = true;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_core_init());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_set_activity_guard(&s_ctx.guard));
  system->is_in_isr = false;
  system->core_count = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_core_init());
  system->core_count = 2u;
  system->core_id = 2u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_core_init());
  system->core_id = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_core_lockout_set_activity_guard(&s_ctx.guard));
  TEST_ASSERT_EQUAL(ESY_OK, esy_core_lockout_set_activity_guard(&s_ctx.guard));
  TEST_ASSERT_EQUAL(ESY_OK, esy_core_lockout_core_init());
  TEST_ASSERT_EQUAL(ESY_OK, esy_core_lockout_core_init());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_init());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_set_activity_guard(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_core_lockout_execute(NULL, &s_ctx));
  system->core_id = 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_execute(operation, &s_ctx));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_core_init());
  system->core_id = 0u;
  system->is_in_isr = true;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_execute(operation, &s_ctx));
  system->is_in_isr = false;
  s_ctx.check_reentry = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_core_lockout_execute(operation, &s_ctx));
  TEST_ASSERT_EQUAL(1u, s_ctx.operations);
  TEST_ASSERT_EQUAL(1u, s_ctx.acquires);
  TEST_ASSERT_EQUAL(1u, s_ctx.releases);
  TEST_ASSERT_EQUAL(0u, system->irq_status);
  s_ctx.acquire_error = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_core_lockout_execute(operation, &s_ctx));
  TEST_ASSERT_EQUAL(1u, s_ctx.operations);
  TEST_ASSERT_EQUAL(1u, s_ctx.releases);
  s_ctx.acquire_error = ESY_OK;
  s_ctx.release_error = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_execute(operation, &s_ctx));
  TEST_ASSERT_EQUAL(2u, s_ctx.operations);
  s_ctx.release_error = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_execute(operation, &s_ctx));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_core_init());
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_core_lockout_init());
  TEST_ASSERT_EQUAL(2u, s_ctx.operations);
  TEST_ASSERT_EQUAL(3u, s_ctx.acquires);
  TEST_ASSERT_EQUAL(2u, s_ctx.releases);
}
