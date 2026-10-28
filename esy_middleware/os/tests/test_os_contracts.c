/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/os/esy_os_task.h"
#include "esy_middleware/os/esy_os_queue.h"
#include "esy_middleware/os/esy_os_mutex.h"
#include "esy_middleware/os/esy_os_semaphore.h"
#include "esy_middleware/os/esy_os_timer.h"
#include "esy_middleware/os/esy_os_work.h"
#include "esy_middleware/os/esy_os_stream_buffer.h"
#include "esy_middleware/os/esy_os_msg_buffer.h"
#include "native_test_support.h"
#include "queue_test_support.h"
#include "snapshot_test_support.h"
#include <string.h>

typedef struct {
  esy_os_work_queue_static_storage_t queue_storage;
  esy_os_work_static_storage_t work_storage;
  esy_os_delayed_work_static_storage_t delayed_storage;
  esy_os_work_queue_t *wq;
  esy_os_work_t *work;
  esy_os_delayed_work_t *delayed;
  _Alignas(ESY_OS_TASK_STACK_ALIGN) uint8_t stack[512];
  uint8_t queue_buffer[ESY_OS_WORK_QUEUE_BUFFER_BYTES(2)];
  unsigned calls;
  esy_err_t self_flush, self_destroy;
} os_contract_test_ctx_t;
static os_contract_test_ctx_t s_ctx;
void setUp(void) {
  memset(&s_ctx, 0xa5, sizeof(s_ctx));
  s_ctx.calls = 0;
  native_test_support_reset();
  queue_test_support_reset();
  queue_test_support_get_ctx()->emulate_storage = true;
  snapshot_test_support_reset();
}
void tearDown(void) {TEST_ASSERT_EQUAL_UINT32(0, native_test_support_get()->critical_depth);}
static void empty_task(void *arg) {(void)arg;}
static void timer_callback(void *arg) {++*(unsigned *)arg;}
static void handler(esy_os_work_t *work, void *arg) {
  (void)arg;
  TEST_ASSERT_EQUAL_UINT32(0, native_test_support_get()->critical_depth);
  TEST_ASSERT_TRUE(esy_os_work_is_running(work));
  ++s_ctx.calls;
  s_ctx.self_flush = esy_os_work_flush(work);
  s_ctx.self_destroy = esy_os_work_queue_destroy(s_ctx.wq);
}
static esy_os_work_queue_resources_t resources(void) {
  return (esy_os_work_queue_resources_t){
    .storage = &s_ctx.queue_storage,
    .task_stack_buffer = s_ctx.stack,
    .task_stack_buffer_size = sizeof(s_ctx.stack),
    .queue_buffer = s_ctx.queue_buffer,
    .queue_buffer_size = sizeof(s_ctx.queue_buffer)};
}
static void create_queue_and_work(void) {
  esy_os_work_queue_resources_t r = resources();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_queue_create_static("worker", 1, 2, &r, &s_ctx.wq));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_create_static(handler, NULL, &s_ctx.work_storage, &s_ctx.work));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_create_static(handler, NULL, &s_ctx.delayed_storage, &s_ctx.delayed));
}
static void progress_worker_and_daemon(void) {native_drain_commands(); native_run_worker();}
void test_task_first_init_failure_retry_delete_and_delay_width(void) {
  esy_os_task_static_storage_t storage;
  memset(&storage, 0xa5, sizeof(storage));
  esy_os_task_t *task = (void *)1;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_create_static(NULL, NULL, NULL, sizeof(s_ctx.stack), 1, s_ctx.stack, &storage, &task));
  TEST_ASSERT_NULL(task);
  native_test_support_get()->create_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_task_create_static(NULL, empty_task, NULL, sizeof(s_ctx.stack), 1, s_ctx.stack, &storage, &task));
  TEST_ASSERT_NULL(task);
  native_test_support_get()->create_fail = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_create_static(NULL, empty_task, NULL, sizeof(s_ctx.stack), 1, s_ctx.stack, &storage, &task));
  TEST_ASSERT_EQUAL(42, esy_os_task_get_stack_remaining(task));
  TEST_ASSERT_FALSE(esy_os_task_is_current(task));
  native_test_support_get()->current = esy_os_task_get_handle(task);
  TEST_ASSERT_TRUE(esy_os_task_is_current(task));
  esy_os_task_delay_ms(1);
  TEST_ASSERT_GREATER_THAN_UINT64(0, native_test_support_get()->ticks);
  esy_os_tick_t tick = esy_os_task_get_tick();
  struct {esy_os_tick_t tick; uint64_t guard;} guarded = {tick, UINT64_C(0x123456789abcdef0)};
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_delay_until(&guarded.tick, 1));
  TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x123456789abcdef0), guarded.guard);
  TEST_ASSERT_EQUAL_UINT64((TickType_t)(tick + native_test_support_get()->period), guarded.tick);
  guarded.tick = portMAX_DELAY;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_delay_until(&guarded.tick, 10));
  TEST_ASSERT_EQUAL_UINT64((TickType_t)(portMAX_DELAY + native_test_support_get()->period), guarded.tick);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_delay_until(NULL, 1));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_delay_until(&tick, 0));
  esy_os_task_delete(task); esy_os_task_delete(task);
  TEST_ASSERT_NULL(esy_os_task_get_handle(task));
  TEST_ASSERT_EQUAL_UINT32(1, native_test_support_get()->deletes);
}
void test_task_notifications_map_public_actions_and_infinite_wait(void) {
  esy_os_task_static_storage_t storage;
  esy_os_task_t *task;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_create_static("test", empty_task, NULL, sizeof(s_ctx.stack), 1, s_ctx.stack, &storage, &task));
  bool yield = true;
  for (int action = ESY_OS_NOTIFY_NO_ACTION; action <= ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE; ++action) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(task, 1, (esy_os_notify_action_t)action));
    TEST_ASSERT_EQUAL(action, native_test_support_get()->action);
    TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_from_isr(task, 1, (esy_os_notify_action_t)action, &yield));
    TEST_ASSERT_TRUE(yield);
  }
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_notify(task, 0, (esy_os_notify_action_t)99));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_notify_from_isr(NULL, 0, ESY_OS_NOTIFY_INCREMENT, &yield));
  TEST_ASSERT_FALSE(yield);
  uint32_t value;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_wait(0, 0, &value, UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT64(portMAX_DELAY, native_test_support_get()->ticks);
  TEST_ASSERT_EQUAL(42, value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(true, UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT64(portMAX_DELAY, native_test_support_get()->ticks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_give(task));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_give_from_isr(task, &yield));
  esy_os_task_notify_clear();
  esy_os_task_set_core_affinity(task, 1);
  esy_os_task_yield_core(0);
}
void test_task_notifications_reject_invalid_actions_before_native_calls(void) {
  esy_os_task_static_storage_t storage;
  esy_os_task_t *task;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_create_static("test", empty_task, NULL,
      sizeof(s_ctx.stack), 1, s_ctx.stack, &storage, &task));
  static const esy_os_notify_action_t k_invalid_actions[] = {
      (esy_os_notify_action_t)-1,
      (esy_os_notify_action_t)(ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE + 1),
      (esy_os_notify_action_t)UINT32_MAX};
  for (size_t i = 0; i < sizeof(k_invalid_actions) / sizeof(k_invalid_actions[0]); ++i) {
    native_test_support_get()->action = eSetBits;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_os_task_notify(task, 0, k_invalid_actions[i]));
    TEST_ASSERT_EQUAL(eSetBits, native_test_support_get()->action);
    bool yield = true;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_os_task_notify_from_isr(task, 0, k_invalid_actions[i], &yield));
    TEST_ASSERT_FALSE(yield);
    TEST_ASSERT_EQUAL(eSetBits, native_test_support_get()->action);
  }
}
void test_task_and_isr_critical_sections_use_the_same_esp32_lock(void) {
  uint32_t key = esy_os_task_enter_crit_auto(); esy_os_task_exit_crit_auto(key);
  native_test_support_get()->isr = true;
  key = esy_os_task_enter_crit_auto(); esy_os_task_exit_crit_auto(key);
  TEST_ASSERT_EQUAL_UINT32(1, native_test_support_get()->isr_entries);
#ifdef ESY_PLATFORM_ESP32
  TEST_ASSERT_NOT_NULL(native_test_support_get()->task_mux);
  TEST_ASSERT_EQUAL_PTR(native_test_support_get()->task_mux, native_test_support_get()->isr_mux);
#endif
  native_test_support_get()->isr = false;
}
void test_queue_infinite_wait_and_positive_rounding(void) {
  esy_os_queue_static_storage_t storage;
  uint32_t data[2], item = 123, got = 0;
  esy_os_queue_t *queue;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_queue_create_static(sizeof(item), 2, data, &storage, &queue));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_queue_send(queue, &item, UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT64(portMAX_DELAY, queue_test_support_get_ctx()->send_ticks_to_wait);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_queue_receive(queue, &got, 1));
  TEST_ASSERT_EQUAL_UINT32(item, got);
  TEST_ASSERT_GREATER_THAN_UINT64(0, queue_test_support_get_ctx()->receive_ticks_to_wait);
  esy_os_queue_delete(queue); esy_os_queue_delete(queue);
  TEST_ASSERT_EQUAL_UINT32(1, queue_test_support_get_ctx()->delete_calls);
}
void test_mutex_and_semaphore_static_lifecycle_and_failures(void) {
  esy_os_mutex_static_storage_t mutex_storage;
  esy_os_semaphore_static_storage_t sem_storage;
  esy_os_mutex_t *mutex;
  esy_os_semaphore_t *sem;
  memset(&mutex_storage, 0xa5, sizeof(mutex_storage));
  memset(&sem_storage, 0xa5, sizeof(sem_storage));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_mutex_create_static(NULL, &mutex)); TEST_ASSERT_NULL(mutex);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_semaphore_create_static(0, 0, &sem_storage, &sem)); TEST_ASSERT_NULL(sem);
  native_test_support_get()->create_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_mutex_create_static(&mutex_storage, &mutex)); TEST_ASSERT_NULL(mutex);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_semaphore_create_static(2, 1, &sem_storage, &sem)); TEST_ASSERT_NULL(sem);
  native_test_support_get()->create_fail = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_mutex_create_static(&mutex_storage, &mutex));
  esy_os_mutex_lock(mutex); TEST_ASSERT_EQUAL_UINT64(portMAX_DELAY, native_test_support_get()->ticks);
  esy_os_mutex_unlock(mutex); esy_os_mutex_delete(mutex); esy_os_mutex_delete(mutex);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_semaphore_create_static(2, 1, &sem_storage, &sem));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_semaphore_take(sem));
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_semaphore_take(sem));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_semaphore_give(sem));
  bool yield;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_semaphore_take_from_isr(sem, &yield)); TEST_ASSERT_TRUE(yield);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_semaphore_give_from_isr(sem, &yield)); TEST_ASSERT_TRUE(yield);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_semaphore_give(sem));
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_semaphore_give(sem));
  esy_os_semaphore_delete(sem); esy_os_semaphore_delete(sem);
  TEST_ASSERT_EQUAL_UINT32(2, native_test_support_get()->deletes);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_semaphore_take(sem));
}
void test_timer_cleanup_requires_command_and_daemon_fence(void) {
  esy_os_timer_static_storage_t storage;
  memset(&storage, 0xa5, sizeof(storage));
  esy_os_timer_t *timer;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_timer_create_static(NULL, 0, false, &s_ctx.calls, timer_callback, &storage, &timer)); TEST_ASSERT_NULL(timer);
  native_test_support_get()->create_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_timer_create_static(NULL, 1, false, &s_ctx.calls, timer_callback, &storage, &timer)); TEST_ASSERT_NULL(timer);
  native_test_support_get()->create_fail = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_create_static(NULL, 1, false, &s_ctx.calls, timer_callback, &storage, &timer));
  TEST_ASSERT_GREATER_THAN_UINT64(0, native_test_support_get()->period);
  native_test_support_get()->command_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_start(timer));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_stop(timer));
  native_test_support_get()->command_fail = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_start(timer)); native_drain_commands(); native_fire_timer();
  TEST_ASSERT_EQUAL_UINT32(1, s_ctx.calls);
  native_test_support_get()->fence_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_stop(timer)); native_drain_commands();
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_start(timer));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_os_timer_delete(timer));
  native_test_support_get()->fence_fail = false;
  unsigned commands = native_test_support_get()->command_count;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_stop(timer));
  TEST_ASSERT_EQUAL_UINT32(commands, native_test_support_get()->command_count);
  native_drain_commands(); TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_stop(timer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_start(timer)); native_drain_commands();
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_delete(timer)); native_drain_commands();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_delete(timer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_delete(timer));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_timer_start(timer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_create_static(NULL, 1, false, &s_ctx.calls, timer_callback, &storage, &timer));
}
void test_work_resource_size_and_partial_create_cleanup(void) {
  esy_os_work_queue_resources_t r = resources();
  r.queue_buffer_size = sizeof(void *);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_work_queue_create_static(NULL, 1, 2, &r, &s_ctx.wq)); TEST_ASSERT_NULL(s_ctx.wq);
  r = resources();
  native_test_support_get()->create_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_work_queue_create_static(NULL, 1, 2, &r, &s_ctx.wq)); TEST_ASSERT_NULL(s_ctx.wq);
  TEST_ASSERT_EQUAL_UINT32(1, queue_test_support_get_ctx()->delete_calls);
  native_test_support_get()->create_fail = false;
  create_queue_and_work();
}
void test_work_idle_cancel_does_not_cancel_next_submission(void) {
  create_queue_and_work();
  esy_os_work_cancel(s_ctx.work);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_submit(s_ctx.wq, s_ctx.work));
  TEST_ASSERT_TRUE(esy_os_work_is_pending(s_ctx.work));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_os_work_submit(s_ctx.wq, s_ctx.work));
  native_run_worker();
  TEST_ASSERT_EQUAL_UINT32(1, s_ctx.calls);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.self_flush);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.self_destroy);
  TEST_ASSERT_FALSE(esy_os_work_is_pending(s_ctx.work));
  TEST_ASSERT_FALSE(esy_os_work_is_running(s_ctx.work));
}
void test_work_isr_submission_propagates_yield_without_performing_it(void) {
  create_queue_and_work();
  native_test_support_get()->isr = true;
  queue_test_support_get_ctx()->higher_priority_task_woken = pdTRUE;
  bool yield = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_submit_from_isr(s_ctx.wq, s_ctx.work, &yield));
  TEST_ASSERT_TRUE(yield);
  TEST_ASSERT_EQUAL_UINT32(0, native_test_support_get()->yields);
  TEST_ASSERT_GREATER_THAN_UINT32(0, native_test_support_get()->isr_entries);
  native_test_support_get()->isr = false;
  native_run_worker(); TEST_ASSERT_EQUAL_UINT32(1, s_ctx.calls);
}
void test_work_cancel_sync_and_flush_observe_completion_during_unlock(void) {
  create_queue_and_work();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_submit(s_ctx.wq, s_ctx.work));
  native_test_support_get()->on_unlock = native_run_worker;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_cancel_sync(s_ctx.work));
  TEST_ASSERT_EQUAL_UINT32(0, s_ctx.calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_submit(s_ctx.wq, s_ctx.work));
  native_test_support_get()->on_delay = native_run_worker;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_flush(s_ctx.work));
  TEST_ASSERT_EQUAL_UINT32(1, s_ctx.calls);
}
static void check_closed_then_progress(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_os_work_submit(s_ctx.wq, s_ctx.work));
  native_run_worker();
}
void test_work_destroy_closes_admission_drains_and_deletes_once(void) {
  create_queue_and_work();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_queue_set_core_affinity(s_ctx.wq, 1));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_submit(s_ctx.wq, s_ctx.work));
  native_test_support_get()->on_delay = check_closed_then_progress;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_queue_destroy(s_ctx.wq));
  TEST_ASSERT_EQUAL_UINT32(1, s_ctx.calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_queue_destroy(s_ctx.wq));
  TEST_ASSERT_EQUAL_UINT32(1, native_test_support_get()->deletes);
  TEST_ASSERT_EQUAL_UINT32(1, queue_test_support_get_ctx()->delete_calls);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_os_work_submit(s_ctx.wq, s_ctx.work));
}
void test_delayed_reschedule_zero_fences_old_timer_and_runs_once(void) {
  create_queue_and_work();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_submit(s_ctx.wq, s_ctx.delayed, 5));
  native_drain_commands(); TEST_ASSERT_TRUE(esy_os_delayed_work_is_active(s_ctx.delayed));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_work_queue_destroy(s_ctx.wq));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_delayed_work_reschedule(s_ctx.delayed, 0));
  native_fire_timer(); /* A due callback before deletion must not submit again. */
  native_drain_commands();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_reschedule(s_ctx.delayed, 0));
  native_run_worker(); native_fire_timer();
  TEST_ASSERT_EQUAL_UINT32(1, s_ctx.calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_last_result(s_ctx.delayed));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_cancel(s_ctx.delayed));
  native_test_support_get()->on_delay = progress_worker_and_daemon;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_queue_destroy(s_ctx.wq));
}
void test_delayed_start_failure_rolls_back_active_and_can_be_cleaned(void) {
  create_queue_and_work(); native_test_support_get()->command_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_delayed_work_submit(s_ctx.wq, s_ctx.delayed, 10));
  TEST_ASSERT_FALSE(esy_os_delayed_work_is_active(s_ctx.delayed));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_delayed_work_last_result(s_ctx.delayed));
  native_test_support_get()->command_fail = false;
  native_test_support_get()->on_delay = progress_worker_and_daemon;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_cancel_sync(s_ctx.delayed));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_work_queue_destroy(s_ctx.wq));
}
void test_delayed_callback_reports_full_queue_and_cancel_releases_binding(void) {
  create_queue_and_work();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_submit(s_ctx.wq, s_ctx.delayed, 10));
  native_drain_commands(); queue_test_support_get_ctx()->send_result = pdFALSE;
  native_fire_timer();
  TEST_ASSERT_FALSE(esy_os_delayed_work_is_active(s_ctx.delayed));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_delayed_work_last_result(s_ctx.delayed));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_delayed_work_cancel(s_ctx.delayed));
  native_drain_commands();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_cancel(s_ctx.delayed));
  queue_test_support_get_ctx()->send_result = pdPASS;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_submit(s_ctx.wq, s_ctx.delayed, 0));
  native_test_support_get()->on_delay = progress_worker_and_daemon;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_delayed_work_cancel_sync(s_ctx.delayed));
  TEST_ASSERT_EQUAL_UINT32(0, s_ctx.calls);
}
void test_message_buffer_preserves_oversized_message_and_reports_empty(void) {
  esy_os_msg_buffer_static_storage_t storage;
  memset(&storage, 0xa5, sizeof(storage));
  uint8_t data[40], output[20]; size_t copied = 99; bool yield;
  esy_os_msg_buffer_t *buffer;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_msg_buffer_create_static(1, data, &storage, &buffer)); TEST_ASSERT_NULL(buffer);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_msg_buffer_create_static(sizeof(data), data, &storage, &buffer));
  TEST_ASSERT_EQUAL_UINT32(sizeof(data)-1, esy_os_msg_buffer_capacity(buffer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_msg_buffer_send(buffer, "hello", 6, UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT64(portMAX_DELAY, native_test_support_get()->ticks);
  TEST_ASSERT_EQUAL_UINT32(6, esy_os_msg_buffer_next_msg_size(buffer));
  TEST_ASSERT_EQUAL_UINT32(6+sizeof(size_t), esy_os_msg_buffer_bytes_used(buffer));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_os_msg_buffer_receive(buffer, output, 2, &copied, 0));
  TEST_ASSERT_EQUAL_UINT32(0, copied); TEST_ASSERT_EQUAL_UINT32(6, esy_os_msg_buffer_next_msg_size(buffer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_msg_buffer_receive(buffer, output, sizeof(output), &copied, 1));
  TEST_ASSERT_EQUAL_STRING("hello", output);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_os_msg_buffer_receive(buffer, output, sizeof(output), &copied, 0));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_os_msg_buffer_receive_from_isr(buffer, output, sizeof(output), &copied, &yield));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_msg_buffer_send_from_isr(buffer, "hello", 6, &yield));
  native_test_support_get()->reset_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_msg_buffer_reset(buffer));
  TEST_ASSERT_EQUAL_UINT32(6, esy_os_msg_buffer_next_msg_size(buffer));
  native_test_support_get()->reset_fail = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_msg_buffer_reset(buffer));
  TEST_ASSERT_EQUAL_UINT32(sizeof(data)-1, esy_os_msg_buffer_space_free(buffer));
  esy_os_msg_buffer_delete(buffer); esy_os_msg_buffer_delete(buffer);
  TEST_ASSERT_EQUAL_UINT32(1, native_test_support_get()->deletes);
}
void test_stream_buffer_partial_io_and_reset_failure(void) {
  esy_os_stream_buffer_static_storage_t storage;
  uint8_t data[5], output[10]; size_t count; bool yield;
  esy_os_stream_buffer_t *buffer;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_stream_buffer_create_static(1, data, &storage, &buffer)); TEST_ASSERT_NULL(buffer);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_stream_buffer_create_static(sizeof(data), data, &storage, &buffer));
  TEST_ASSERT_EQUAL_UINT32(4, esy_os_stream_buffer_capacity(buffer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_stream_buffer_send(buffer, "hello", 6, &count, UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT32(4, count); TEST_ASSERT_EQUAL_UINT64(portMAX_DELAY, native_test_support_get()->ticks);
  TEST_ASSERT_EQUAL_UINT32(4, esy_os_stream_buffer_bytes_used(buffer));
  TEST_ASSERT_EQUAL_UINT32(0, esy_os_stream_buffer_space_free(buffer));
  native_test_support_get()->reset_fail = true;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_stream_buffer_reset(buffer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_stream_buffer_receive_from_isr(buffer, output, sizeof(output), &count, &yield)); TEST_ASSERT_EQUAL_UINT32(4, count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_stream_buffer_send_from_isr(buffer, "x", 1, &count, &yield));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_stream_buffer_receive(buffer, output, 1, &count, 1));
  TEST_ASSERT_GREATER_THAN_UINT64(0, native_test_support_get()->ticks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_stream_buffer_set_trigger_level(buffer, 1));
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_stream_buffer_set_trigger_level(buffer, 99));
  native_test_support_get()->reset_fail = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_stream_buffer_reset(buffer));
  esy_os_stream_buffer_delete(buffer); esy_os_stream_buffer_delete(buffer);
  TEST_ASSERT_EQUAL_UINT32(1, native_test_support_get()->deletes);
}
#if !defined(ESY_PLATFORM_ESP32)
#if configUSE_TRACE_FACILITY == 1
void test_snapshot_native_counter_width_is_narrowed_without_overwriting_stack(void) {
  snapshot_test_support_ctx_t *ctx = snapshot_test_support_get();
  ctx->native_count = 1;
  ctx->total_high = 0xfeedu;
  ctx->native[0].ulRunTimeCounter = (configRUN_TIME_COUNTER_TYPE)UINT64_C(0x112233440000007b);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_get_system_state(ctx->items, ESY_OS_TASK_SYSTEM_STATE_MAX, &ctx->count, &ctx->total, &ctx->scratch));
  TEST_ASSERT_EQUAL_UINT32(123, ctx->total);
  TEST_ASSERT_EQUAL_UINT32(123, ctx->items[0].runtime_counter);
}
#endif
#endif

void test_cooperative_task_parking_checks_native_state_before_deletion(void) {
  esy_os_task_static_storage_t storage;
  ESY_ALIGNED_STORAGE(stack, 512u);
  esy_os_task_t *task;
  TEST_ASSERT_TRUE(esy_os_task_park_supported());
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_delete_parked(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_create_static("park", empty_task, NULL,
      sizeof(stack.bytes), 1u, stack.bytes, &storage, &task));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_task_delete_parked(task));
  native_test_support_get()->current = esy_os_task_get_handle(task);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_os_task_delete_parked(task));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_park());
  native_test_support_get()->current = NULL;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_delete_parked(task));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_delete_parked(task));
  TEST_ASSERT_EQUAL_UINT32(1u, native_test_support_get()->deletes);
}
