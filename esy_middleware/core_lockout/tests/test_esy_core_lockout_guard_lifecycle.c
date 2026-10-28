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
  uint32_t acquires;
  uint32_t releases;
  esy_err_t release_result;
} guard_owner_t;

typedef struct {
  guard_owner_t first;
  guard_owner_t selected;
  esy_activity_guard_t first_guard;
  esy_activity_guard_t selected_guard;
  uint32_t operations;
} guard_lifecycle_test_ctx_t;
static guard_lifecycle_test_ctx_t s_ctx;

static esy_err_t acquire(void *ctx) {
  guard_owner_t *owner = ctx;
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  owner->acquires++;
  return ESY_OK;
}

static esy_err_t release(void *ctx) {
  guard_owner_t *owner = ctx;
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  owner->releases++;
  return owner->release_result;
}

static void operation(void *arg) {
  (void)arg;
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.operations++;
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.first_guard = (esy_activity_guard_t){&s_ctx.first, acquire, release};
  s_ctx.selected_guard = (esy_activity_guard_t){&s_ctx.selected, acquire, release};
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_system_reset();
}
void tearDown(void) {}

/* Borrowed descriptors and provider contexts live for this process lifetime.
 * One public singleton lifecycle needs no test-only reset entry point.
 */
void test_core_lockout_guard_replacement_validation_and_fault_lifetime(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_core_lockout_set_activity_guard(&s_ctx.first_guard));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_set_activity_guard(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_core_lockout_set_activity_guard(&s_ctx.selected_guard));

  esy_activity_guard_t invalid = {NULL, acquire, release};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
      esy_core_lockout_set_activity_guard(&invalid));
  invalid = (esy_activity_guard_t){&s_ctx.first, NULL, release};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
      esy_core_lockout_set_activity_guard(&invalid));
  invalid = (esy_activity_guard_t){&s_ctx.first, acquire, NULL};
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
      esy_core_lockout_set_activity_guard(&invalid));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_init());
  fake_esy_os_set_started(true);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_core_lockout_execute(operation, NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.first.acquires);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.first.releases);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.selected.acquires);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.selected.releases);

  s_ctx.selected.release_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_execute(operation, NULL));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.operations);
  s_ctx.selected.release_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_init());
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_set_activity_guard(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
      esy_core_lockout_set_activity_guard(&s_ctx.first_guard));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_core_lockout_execute(operation, NULL));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.selected.acquires);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.selected.releases);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
}
