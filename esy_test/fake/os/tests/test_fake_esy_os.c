/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "fake_esy_os_task.h"
#include "fake_esy_os_timer.h"
#include "fake_esy_os_queue.h"

void setUp(void) {fake_esy_os_task_reset(); fake_esy_os_timer_reset(); fake_esy_os_queue_reset();}
void tearDown(void) {}
static void callback(void *arg) {++*(unsigned *)arg;}
static void task_entry(void *arg) {(void)arg;}
void test_notification_actions_preserve_and_replace_values(void) {
  esy_os_task_static_storage_t storage;
  _Alignas(ESY_OS_TASK_STACK_ALIGN) uint8_t stack[256];
  esy_os_task_t *task;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_create_static("task", task_entry, NULL, sizeof(stack), 1, stack, &storage, &task));
  fake_esy_os_task_set_current(task);
  TEST_ASSERT_TRUE(esy_os_task_is_current(task));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(task, 4, ESY_OS_NOTIFY_SET_BITS));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(task, 2, ESY_OS_NOTIFY_SET_BITS));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(task, 0, ESY_OS_NOTIFY_INCREMENT));
  TEST_ASSERT_EQUAL_UINT32(7, fake_esy_os_task_find(task)->notify_value);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_task_notify(task, 99, ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE));
  TEST_ASSERT_EQUAL_UINT32(7, fake_esy_os_task_find(task)->notify_value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(task, 99, ESY_OS_NOTIFY_NO_ACTION));
  uint32_t value;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_wait(0xffffffffu, 3, &value, 0));
  TEST_ASSERT_EQUAL_UINT32(7, value);
  TEST_ASSERT_EQUAL_UINT32(4, fake_esy_os_task_find(task)->notify_value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(task, 99, ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE));
  bool yield = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_from_isr(task, 8, ESY_OS_NOTIFY_SET_WITH_OVERWRITE, &yield));
  TEST_ASSERT_FALSE(yield);
  TEST_ASSERT_EQUAL_UINT32(8, fake_esy_os_task_find(task)->notify_value);
  esy_os_task_notify_clear();
  TEST_ASSERT_EQUAL_UINT32(8, fake_esy_os_task_find(task)->notify_value);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_notify(task, 0, (esy_os_notify_action_t)99));
  esy_os_tick_t tick = esy_os_task_get_tick();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_delay_until(&tick, 10));
  TEST_ASSERT_EQUAL_UINT64(10, esy_os_task_get_tick());
  tick = UINT32_MAX - 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_delay_until(&tick, 10));
  TEST_ASSERT_EQUAL_UINT64(7, tick);
  tick = UINT64_C(0x100000000);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_delay_until(&tick, 1));
}
void test_timer_busy_cleanup_retains_callback_and_success_retires_once(void) {
  esy_os_timer_static_storage_t storage;
  esy_os_timer_t *timer = (void *)1;
  unsigned calls = 0;
  fake_esy_os_timer_set_create_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_timer_create_static(NULL, 1, false, &calls, callback, &storage, &timer));
  TEST_ASSERT_NULL(timer);
  fake_esy_os_timer_set_create_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_create_static(NULL, 1, false, &calls, callback, &storage, &timer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_start(timer));
  fake_esy_os_timer_set_cleanup_result(ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_stop(timer));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_delete(timer));
  fake_esy_os_timer_fire(timer);
  TEST_ASSERT_EQUAL_UINT32(1, calls);
  fake_esy_os_timer_set_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_stop(timer));
  fake_esy_os_timer_advance_daemon();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_stop(timer));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_delete(timer));
  fake_esy_os_timer_advance_daemon();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_delete(timer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_delete(timer));
  fake_esy_os_timer_fire(timer);
  TEST_ASSERT_EQUAL_UINT32(1, calls);
  TEST_ASSERT_EQUAL_UINT32(1, fake_esy_os_timer_get_delete_count());
}

typedef struct {
  esy_os_timer_t *timer;
  esy_os_timer_static_storage_t storage;
  bool deleting;
  unsigned calls;
} timer_cleanup_fixture_t;

static void cleanup_in_callback(void *arg) {
  timer_cleanup_fixture_t *fixture = arg;
  ++fixture->calls;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, fixture->deleting
      ? esy_os_timer_delete(fixture->timer) : esy_os_timer_stop(fixture->timer));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, fixture->deleting
      ? esy_os_timer_stop(fixture->timer) : esy_os_timer_delete(fixture->timer));
  fake_esy_os_timer_advance_daemon();
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, fixture->deleting
      ? esy_os_timer_delete(fixture->timer) : esy_os_timer_stop(fixture->timer));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_start(fixture->timer));
  esy_os_timer_t *replacement = (void *)1;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_os_timer_create_static(NULL, 1u, true,
      fixture, cleanup_in_callback, &fixture->storage, &replacement));
  TEST_ASSERT_NULL(replacement);
  TEST_ASSERT_TRUE(fake_esy_os_timer_find(fixture->timer)->used);
  TEST_ASSERT_FALSE(fake_esy_os_timer_find(fixture->timer)->cleanup_complete);
}

void test_callback_stop_needs_daemon_fence_before_rearm_and_delete(void) {
  timer_cleanup_fixture_t fixture = {0};
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_create_static(NULL, 1u, true, &fixture,
      cleanup_in_callback, &fixture.storage, &fixture.timer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_start(fixture.timer));
  fake_esy_os_timer_fire(fixture.timer);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.calls);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_stop(fixture.timer));
  fake_esy_os_timer_advance_daemon();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_stop(fixture.timer));
  fake_esy_os_timer_fire(fixture.timer);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_start(fixture.timer));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_os_timer_delete(fixture.timer));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_os_timer_stop(fixture.timer));
  fake_esy_os_timer_advance_daemon();
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_delete(fixture.timer));
}

void test_callback_delete_retains_context_and_storage_until_successful_retry(void) {
  timer_cleanup_fixture_t fixture = {.deleting = true};
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_create_static(NULL, 1u, true, &fixture,
      cleanup_in_callback, &fixture.storage, &fixture.timer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_start(fixture.timer));
  fake_esy_os_timer_fire(fixture.timer);
  fake_esy_os_timer_entry_t *entry = fake_esy_os_timer_find(fixture.timer);
  TEST_ASSERT_EQUAL_PTR(&fixture, entry->arg);
  TEST_ASSERT_TRUE(entry->used);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_timer_get_delete_count());
  fake_esy_os_timer_advance_daemon();
  fake_esy_os_timer_fire(fixture.timer);
  TEST_ASSERT_EQUAL_UINT32(1u, fixture.calls);
  TEST_ASSERT_TRUE(entry->used);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_delete(fixture.timer));
  TEST_ASSERT_NULL(entry->arg);
  TEST_ASSERT_FALSE(entry->used);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_delete(fixture.timer));
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_timer_get_delete_count());
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_timer_create_static(NULL, 1u, true, &fixture,
      cleanup_in_callback, &fixture.storage, &fixture.timer));
}

void test_queue_failed_create_clears_output_and_allows_retry(void) {
  esy_os_queue_static_storage_t storage;
  uint32_t buffer[2], input = 42, output;
  esy_os_queue_t *queue = (void *)1;
  fake_esy_os_queue_set_create_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_queue_create_static(sizeof(input), 2, buffer, &storage, &queue));
  TEST_ASSERT_NULL(queue);
  fake_esy_os_queue_set_create_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_queue_create_static(sizeof(input), 2, buffer, &storage, &queue));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_queue_send(queue, &input, 0));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_queue_receive(queue, &output, 0));
  TEST_ASSERT_EQUAL_UINT32(input, output);
  esy_os_queue_delete(queue);
  esy_os_queue_delete(queue);
}
