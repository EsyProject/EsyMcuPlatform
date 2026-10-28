/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <setjmp.h>
#include <string.h>
#include "esy_core_lockout_internal.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_system.h"

typedef struct {
  uint32_t operations;
  uint32_t waits;
  jmp_buf stop;
} core_lockout_init_test_ctx_t;
static core_lockout_init_test_ctx_t s_ctx;

static void operation(void *arg) { (void)arg; s_ctx.operations++; }
static void stop_after_iteration(void) {
  if (++s_ctx.waits > 1u) longjmp(s_ctx.stop, 1);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_system_reset();
}
void tearDown(void) {}

/* One process-lifetime scenario: production init owns permanent workers and
 * intentionally has no test-only reset or deinit entry point.
 */
void test_core_lockout_init_fails_closed_retries_partial_creation_and_is_idempotent(void) {
  fake_esy_hal_system_ctx_t *platform = fake_esy_hal_system_get_ctx();
  platform->is_in_isr = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_init());
  platform->is_in_isr = false;
  platform->core_count = 3u;
  fake_esy_os_get_state()->core_count = 3u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_core_lockout_init());
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_execute(operation, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_core_init());

  platform->core_count = 2u;
  fake_esy_os_get_state()->core_count = 2u;
  fake_esy_os_task_set_create_result(ESY_ERR_NO_MEM);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NO_MEM, esy_core_lockout_init());
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_execute(operation, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_core_init());
  fake_esy_os_task_set_create_result(ESY_OK);
  fake_esy_os_task_set_create_static_failure(3u, ESY_ERR_NO_MEM);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NO_MEM, esy_core_lockout_init());
  fake_esy_os_task_entry_t *first = fake_esy_os_task_get_last_created();
  TEST_ASSERT_EQUAL_UINT8(1u, first->core_affinity);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_execute(operation, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_core_init());
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_delete_count());

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_init());
  TEST_ASSERT_EQUAL_UINT32(4u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_UINT8(2u, fake_esy_os_task_get_last_created()->core_affinity);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_init());
  TEST_ASSERT_EQUAL_UINT32(4u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_execute(operation, NULL));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.operations);

  /* A request canceled before the task reads it must still be acknowledged.
   * A duplicate notification must not enter the RAM wait again.
   */
  core_lockout_worker_t *worker = first->arg;
  atomic_store(&worker->request, 1u);
  atomic_store(&worker->release, 1u);
  fake_esy_os_task_set_current(first->task_handle);
  fake_esy_os_task_set_notify_take_hook(stop_after_iteration);
  if (setjmp(s_ctx.stop) == 0) first->entry(first->arg);
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&worker->completed));
  TEST_ASSERT_EQUAL_UINT32(0u, platform->irq_status);
  uint32_t signals = platform->signal_event_count;
  s_ctx.waits = 0u;
  if (setjmp(s_ctx.stop) == 0) first->entry(first->arg);
  TEST_ASSERT_EQUAL_UINT32(signals, platform->signal_event_count);
}
