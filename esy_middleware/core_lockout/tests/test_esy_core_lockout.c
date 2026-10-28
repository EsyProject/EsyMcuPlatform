/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <string.h>
#include "esy_core_lockout_internal.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_system.h"

typedef enum {
  SCHEDULE_NORMAL,
  SCHEDULE_SILENT,
  SCHEDULE_CANCEL_ONLY,
  SCHEDULE_NO_COMPLETION,
} schedule_mode_t;

typedef struct {
  esy_core_lockout_ctx_t lockout;
  esy_core_lockout_core_ctx_t core;
  schedule_mode_t mode;
  uint32_t operations;
  core_lockout_worker_t *worker;
  uint32_t sequence;
  esy_activity_guard_t guard;
  esy_err_t acquire_result;
  esy_err_t release_result;
  uint32_t acquires;
  uint32_t releases;
} core_lockout_test_ctx_t;
static core_lockout_test_ctx_t s_ctx;

static void unused_task(void *arg) { (void)arg; }

static void operation(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.operations++;
}

/* Deterministic peer acknowledgements drive the requester endpoint. Worker
 * endpoint tests below exercise the real RAM function at cancellation edges.
 */
static void schedule_peer(void) {
  fake_esy_hal_system_get_ctx()->time_us += CORE_LOCKOUT_SAFE_ZONE_TIMEOUT_US / 2u;
  for (uint32_t i = 0u; i < ESY_CORE_LOCKOUT_MAX_CORES; ++i) {
    core_lockout_worker_t *worker = &s_ctx.lockout.workers[i];
    uint32_t request = atomic_load(&worker->request);
    if (request == atomic_load(&worker->completed)) continue;
    if (s_ctx.mode == SCHEDULE_SILENT) continue;
    if (s_ctx.mode == SCHEDULE_CANCEL_ONLY) {
      if (atomic_load(&worker->release) == request)
        esy_core_lockout_worker_wait(worker, request);
      continue;
    }
    atomic_store(&worker->locked, request);
    if ((s_ctx.mode != SCHEDULE_NO_COMPLETION) &&
        (atomic_load(&worker->release) == request)) {
      atomic_store(&worker->completed, request);
    }
  }
}

static esy_err_t execute(void) {
  return esy_core_lockout_execute_with_ctx(&s_ctx.core, &s_ctx.lockout, operation, &s_ctx);
}

static esy_err_t guard_acquire(void *ctx) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.acquires++;
  return s_ctx.acquire_result;
}

static esy_err_t guard_release(void *ctx) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.releases++;
  return s_ctx.release_result;
}

static void use_guard(void) {
  s_ctx.guard = (esy_activity_guard_t){&s_ctx, guard_acquire, guard_release};
  s_ctx.core.activity_guard = &s_ctx.guard;
}

static void assert_fault_blocks_retry(uint32_t operations) {
  uint32_t notifications =
      fake_esy_os_task_find(s_ctx.lockout.workers[1].task)->notify_give_count;
  uint32_t yields = fake_esy_os_task_get_yield_core_count();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.acquires);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.releases);
  TEST_ASSERT_EQUAL_UINT32(operations, s_ctx.operations);
  /* Provider recovery alone cannot make an uncertain release safe to retry. */
  s_ctx.release_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, execute());
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.acquires);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.releases);
  TEST_ASSERT_EQUAL_UINT32(operations, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(notifications,
      fake_esy_os_task_find(s_ctx.lockout.workers[1].task)->notify_give_count);
  TEST_ASSERT_EQUAL_UINT32(yields, fake_esy_os_task_get_yield_core_count());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_system_reset();
  fake_esy_os_get_state()->core_count = 2u;
  fake_esy_os_set_started(true);
  fake_esy_hal_system_get_ctx()->core_count = 2u;
  fake_esy_hal_system_get_ctx()->wait_event_hook = schedule_peer;
  s_ctx.lockout.workers_initialized = true;
  for (uint32_t i = 0u; i < ESY_CORE_LOCKOUT_MAX_CORES; ++i) {
    core_lockout_worker_t *worker = &s_ctx.lockout.workers[i];
    atomic_init(&worker->request, 0u);
    atomic_init(&worker->release, 0u);
    atomic_init(&worker->locked, 0u);
    atomic_init(&worker->completed, 0u);
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_os_task_create_static(
        "peer", unused_task, worker, sizeof(worker->task_stack_buffer), 1u,
        worker->task_stack_buffer, &worker->task_control_storage, &worker->task));
  }
}

void tearDown(void) {}

void test_core_lockout_executes_once_and_waits_for_completion(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&s_ctx.lockout.workers[1].completed));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
}

void test_core_lockout_timeout_drains_late_worker_before_retry(void) {
  s_ctx.mode = SCHEDULE_CANCEL_ONLY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, execute());
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&s_ctx.lockout.workers[1].completed));
  s_ctx.mode = SCHEDULE_NORMAL;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.operations);
}

void test_core_lockout_unacknowledged_cancel_blocks_both_cores_until_drained(void) {
  s_ctx.mode = SCHEDULE_SILENT;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, execute());
  core_lockout_worker_t *worker = &s_ctx.lockout.workers[1];
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&worker->release));
  TEST_ASSERT_EQUAL_UINT32(0u, atomic_load(&worker->completed));
  TEST_ASSERT_EQUAL_UINT64(2u * CORE_LOCKOUT_SAFE_ZONE_TIMEOUT_US,
                          fake_esy_hal_system_get_ctx()->time_us);
  fake_esy_hal_system_get_ctx()->core_id = 1u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, execute());
  TEST_ASSERT_EQUAL_UINT32(2u,
      fake_esy_os_task_find(worker->task)->notify_give_count);
  TEST_ASSERT_EQUAL_UINT32(2u, fake_esy_os_task_get_yield_core_count());
  esy_core_lockout_worker_wait(worker, 1u);
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&worker->completed));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
  s_ctx.mode = SCHEDULE_NORMAL;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.operations);
}

static void cancel_after_worker_observes_request(void) {
  atomic_store(&s_ctx.worker->release, s_ctx.sequence);
}

void test_core_lockout_cancellation_between_request_read_and_lock_is_not_lost(void) {
  s_ctx.worker = &s_ctx.lockout.workers[1];
  s_ctx.sequence = 7u;
  atomic_store(&s_ctx.worker->request, s_ctx.sequence);
  fake_esy_hal_system_get_ctx()->irq_save_hook =
      cancel_after_worker_observes_request;
  esy_core_lockout_worker_wait(s_ctx.worker, s_ctx.sequence);
  TEST_ASSERT_EQUAL_UINT32(7u, atomic_load(&s_ctx.worker->release));
  TEST_ASSERT_EQUAL_UINT32(7u, atomic_load(&s_ctx.worker->completed));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->wait_event_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_ctx()->irq_status);
}

static void release_locked_worker(void) {
  TEST_ASSERT_EQUAL_UINT32(s_ctx.sequence, atomic_load(&s_ctx.worker->locked));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_system_get_ctx()->irq_status);
  TEST_ASSERT_NOT_EQUAL(s_ctx.sequence, atomic_load(&s_ctx.worker->completed));
  atomic_store(&s_ctx.worker->release, s_ctx.sequence);
}

void test_core_lockout_worker_restores_prior_irq_state_after_release(void) {
  s_ctx.worker = &s_ctx.lockout.workers[1];
  s_ctx.sequence = 9u;
  fake_esy_hal_system_get_ctx()->irq_status = 1u;
  fake_esy_hal_system_get_ctx()->wait_event_hook = release_locked_worker;
  esy_core_lockout_worker_wait(s_ctx.worker, s_ctx.sequence);
  TEST_ASSERT_EQUAL_UINT32(9u, atomic_load(&s_ctx.worker->completed));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_system_get_ctx()->irq_status);
}

void test_core_lockout_release_timeout_preserves_executed_operation_and_pending_worker(void) {
  s_ctx.mode = SCHEDULE_NO_COMPLETION;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, execute());
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.operations);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, execute());
  esy_core_lockout_worker_wait(&s_ctx.lockout.workers[1], 1u);
  s_ctx.mode = SCHEDULE_NORMAL;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.operations);
}

void test_core_lockout_sequences_wrap_and_survive_repeated_timeouts(void) {
  core_lockout_worker_t *worker = &s_ctx.lockout.workers[1];
  atomic_store(&worker->request, UINT32_MAX);
  atomic_store(&worker->release, UINT32_MAX);
  atomic_store(&worker->locked, UINT32_MAX);
  atomic_store(&worker->completed, UINT32_MAX);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  TEST_ASSERT_EQUAL_UINT32(0u, atomic_load(&worker->completed));
  /* Exercise both cancellation orderings repeatedly, including clock wrap. */
  fake_esy_hal_system_get_ctx()->time_us = UINT32_MAX - 100u;
  for (uint32_t i = 0u; i < 64u; ++i) {
    s_ctx.mode = SCHEDULE_CANCEL_ONLY;
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, execute());
    s_ctx.mode = SCHEDULE_NORMAL;
    TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  }
  TEST_ASSERT_EQUAL_UINT32(65u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(128u, atomic_load(&worker->completed));
}

void test_core_lockout_rejects_unready_invalid_and_isr_execution(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
      esy_core_lockout_execute_with_ctx(&s_ctx.core, NULL, operation, &s_ctx));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
      esy_core_lockout_execute_with_ctx(&s_ctx.core, &s_ctx.lockout, NULL, &s_ctx));
  s_ctx.lockout.workers_initialized = false;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, execute());
  s_ctx.lockout.workers_initialized = true;
  fake_esy_hal_system_get_ctx()->is_in_isr = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, execute());
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.operations);
}

void test_core_lockout_before_scheduler_does_not_notify_workers(void) {
  fake_esy_os_set_started(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  TEST_ASSERT_EQUAL_UINT32(0u,
      fake_esy_os_task_find(s_ctx.lockout.workers[1].task)->notify_give_count);
}

void test_core_lockout_guard_acquire_failure_does_not_release_or_poison_retry(void) {
  use_guard();
  s_ctx.acquire_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, execute());
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.releases);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.operations);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_yield_core_count());
  s_ctx.acquire_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.acquires);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.releases);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.operations);
}

void test_core_lockout_guard_cleanup_success_preserves_lockout_error_and_retry(void) {
  use_guard();
  s_ctx.mode = SCHEDULE_CANCEL_ONLY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, execute());
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.releases);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.operations);
  s_ctx.mode = SCHEDULE_NORMAL;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, execute());
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.acquires);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.releases);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.operations);
}

void test_core_lockout_guard_cleanup_failure_overrides_lockout_timeout_and_latches(void) {
  use_guard();
  s_ctx.mode = SCHEDULE_SILENT;
  s_ctx.release_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, execute());
  core_lockout_worker_t *worker = &s_ctx.lockout.workers[1];
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&worker->release));
  TEST_ASSERT_EQUAL_UINT32(0u, atomic_load(&worker->completed));
  assert_fault_blocks_retry(0u);
  esy_core_lockout_worker_wait(worker, 1u);
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&worker->completed));
  assert_fault_blocks_retry(0u);
}

void test_core_lockout_guard_release_failure_after_mutation_blocks_reexecution(void) {
  use_guard();
  s_ctx.release_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, execute());
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&s_ctx.lockout.workers[1].completed));
  assert_fault_blocks_retry(1u);
}

void test_core_lockout_guard_failure_overrides_peer_release_timeout_without_resetting_worker(void) {
  use_guard();
  s_ctx.mode = SCHEDULE_NO_COMPLETION;
  s_ctx.release_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, execute());
  core_lockout_worker_t *worker = &s_ctx.lockout.workers[1];
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&worker->request));
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&worker->release));
  TEST_ASSERT_EQUAL_UINT32(0u, atomic_load(&worker->completed));
  assert_fault_blocks_retry(1u);
  esy_core_lockout_worker_wait(worker, 1u);
  TEST_ASSERT_EQUAL_UINT32(1u, atomic_load(&worker->completed));
  assert_fault_blocks_retry(1u);
}
