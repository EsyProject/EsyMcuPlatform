/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_os_task.h"
#include "unity.h"

#include <string.h>

typedef struct {
  esy_os_task_t *tasks[2];
  esy_os_task_static_storage_t storage[2];
  _Alignas(ESY_OS_TASK_STACK_ALIGN) uint8_t stacks[2][256];
  uint32_t value_before_arrival;
} notification_test_ctx_t;

static notification_test_ctx_t s_ctx;

static void task_entry(void *arg) { (void)arg; }

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_os_task_reset();
  for (unsigned i = 0u; i < 2u; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_create_static(
        "notification", task_entry, NULL, sizeof(s_ctx.stacks[i]), 1u,
        s_ctx.stacks[i], &s_ctx.storage[i], &s_ctx.tasks[i]));
  }
  fake_esy_os_task_set_current(s_ctx.tasks[0]);
}

void tearDown(void) { fake_esy_os_task_set_notify_take_hook(NULL); }

void test_value_increment_is_consumed_by_take_without_inventing_give_calls(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(
      s_ctx.tasks[0], 99u, ESY_OS_NOTIFY_INCREMENT));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(true, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_value);
  TEST_ASSERT_FALSE(entry->notify_pending);
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_give_count);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_task_notify_take(false, 0u));
}

void test_give_and_wait_share_value_but_leave_instrumentation_unchanged(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  uint32_t value = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_give(s_ctx.tasks[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_give(s_ctx.tasks[0]));
  /* A pending notification skips entry clearing; the output precedes exit clearing. */
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_wait(UINT32_MAX, 2u, &value, 0u));
  TEST_ASSERT_EQUAL_UINT32(2u, value);
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_value);
  TEST_ASSERT_FALSE(entry->notify_pending);
  TEST_ASSERT_EQUAL_UINT32(2u, entry->notify_give_count);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_task_notify_take(true, 0u));
  TEST_ASSERT_EQUAL_UINT32(2u, entry->notify_give_count);
}

void test_take_decrements_value_and_clears_pending_even_when_a_count_remains(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(
      s_ctx.tasks[0], 3u, ESY_OS_NOTIFY_SET_WITH_OVERWRITE));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(false, 0u));
  TEST_ASSERT_EQUAL_UINT32(2u, entry->notify_value);
  TEST_ASSERT_FALSE(entry->notify_pending);
  /* State is clear although value is nonzero, so overwrite must now succeed. */
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(
      s_ctx.tasks[0], 5u, ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(false, 0u));
  TEST_ASSERT_EQUAL_UINT32(4u, entry->notify_value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(true, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_value);
}

void test_clear_drops_only_state_and_wait_failure_preserves_output_and_count(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  uint32_t value = 42u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(
      s_ctx.tasks[0], 0xf3u, ESY_OS_NOTIFY_SET_WITH_OVERWRITE));
  esy_os_task_notify_clear();
  TEST_ASSERT_FALSE(entry->notify_pending);
  TEST_ASSERT_EQUAL_UINT32(0xf3u, entry->notify_value);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_task_notify_wait(0xf0u, UINT32_MAX, &value, 0u));
  TEST_ASSERT_EQUAL_UINT32(42u, value);
  TEST_ASSERT_EQUAL_UINT32(3u, entry->notify_value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(true, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_value);
}

static void arrive_from_isr(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  s_ctx.value_before_arrival = entry->notify_value;
  bool yield = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_give_from_isr(s_ctx.tasks[0], &yield));
  TEST_ASSERT_FALSE(yield);
}

void test_wait_entry_clear_precedes_isr_arrival_and_exit_clear_follows_snapshot(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  uint32_t value = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(
      s_ctx.tasks[0], 0xf0u, ESY_OS_NOTIFY_SET_WITH_OVERWRITE));
  esy_os_task_notify_clear();
  fake_esy_os_task_set_notify_take_hook(arrive_from_isr);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_wait(0xf0u, 1u, &value, 10u));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.value_before_arrival);
  TEST_ASSERT_EQUAL_UINT32(1u, value);
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_value);
  TEST_ASSERT_FALSE(entry->notify_pending);
  TEST_ASSERT_EQUAL_UINT32(1u, entry->notify_give_count);
  TEST_ASSERT_EQUAL_UINT32(1u, entry->notify_give_from_isr_count);
}

void test_isr_value_notification_and_task_give_are_consumed_by_the_same_take(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  bool yield = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_from_isr(
      s_ctx.tasks[0], 2u, ESY_OS_NOTIFY_SET_BITS, &yield));
  TEST_ASSERT_FALSE(yield);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_give(s_ctx.tasks[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(false, 0u));
  TEST_ASSERT_EQUAL_UINT32(2u, entry->notify_value);
  TEST_ASSERT_FALSE(entry->notify_pending);
  TEST_ASSERT_EQUAL_UINT32(1u, entry->notify_give_count);
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_give_from_isr_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(true, 0u));
  TEST_ASSERT_EQUAL_UINT32(1u, entry->notify_give_count);
}

void test_no_action_wakes_wait_with_zero_value_but_cannot_supply_a_take_count(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  uint32_t value = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(
      s_ctx.tasks[0], 7u, ESY_OS_NOTIFY_NO_ACTION));
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_wait(0u, 0u, &value, 0u));
  TEST_ASSERT_EQUAL_UINT32(0u, value);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(
      s_ctx.tasks[0], 7u, ESY_OS_NOTIFY_NO_ACTION));
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_task_notify_take(false, 0u));
  TEST_ASSERT_FALSE(entry->notify_pending);
}

void test_pending_give_rejects_non_overwrite_and_increment_wraps_like_native_value(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_give(s_ctx.tasks[0]));
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_task_notify(
      s_ctx.tasks[0], 99u, ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE));
  TEST_ASSERT_EQUAL_UINT32(1u, entry->notify_value);
  TEST_ASSERT_TRUE(entry->notify_pending);
  for (unsigned isr = 0u; isr < 2u; ++isr) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(
        s_ctx.tasks[0], UINT32_MAX, ESY_OS_NOTIFY_SET_WITH_OVERWRITE));
    TEST_ASSERT_EQUAL(ESY_OK, isr ? esy_os_task_notify_give_from_isr(s_ctx.tasks[0], NULL)
        : esy_os_task_notify(s_ctx.tasks[0], 0u, ESY_OS_NOTIFY_INCREMENT));
    TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_value);
    TEST_ASSERT_TRUE(entry->notify_pending);
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_os_task_notify_take(true, 0u));
    TEST_ASSERT_FALSE(entry->notify_pending);
  }
  TEST_ASSERT_EQUAL_UINT32(2u, entry->notify_give_count);
  TEST_ASSERT_EQUAL_UINT32(1u, entry->notify_give_from_isr_count);
}

void test_take_can_observe_isr_arrival_without_affecting_another_tasks_slot(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  fake_esy_os_task_entry_t *other = fake_esy_os_task_find(s_ctx.tasks[1]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(s_ctx.tasks[1], 0x40u, ESY_OS_NOTIFY_SET_BITS));
  fake_esy_os_task_set_notify_take_hook(arrive_from_isr);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify_take(true, 25u));
  TEST_ASSERT_TRUE(entry->notify_take_clear_count);
  TEST_ASSERT_EQUAL_UINT32(25u, entry->notify_take_timeout_ms);
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_value);
  TEST_ASSERT_FALSE(entry->notify_pending);
  TEST_ASSERT_EQUAL_UINT32(0x40u, other->notify_value);
  TEST_ASSERT_TRUE(other->notify_pending);
  TEST_ASSERT_EQUAL_UINT32(0u, other->notify_give_count);
}

void test_invalid_notification_calls_leave_shared_slot_and_statistics_unchanged(void) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.tasks[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_os_task_notify(s_ctx.tasks[0], 8u, ESY_OS_NOTIFY_SET_BITS));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_notify(
      s_ctx.tasks[0], 0u, (esy_os_notify_action_t)99));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_notify_give(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_notify_give_from_isr(NULL, NULL));
  fake_esy_os_task_set_current(NULL);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_notify_take(true, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_os_task_notify_wait(0u, 0u, NULL, 0u));
  TEST_ASSERT_EQUAL_UINT32(8u, entry->notify_value);
  TEST_ASSERT_TRUE(entry->notify_pending);
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_give_count);
  TEST_ASSERT_EQUAL_UINT32(0u, entry->notify_give_from_isr_count);
  fake_esy_os_task_reset();
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_table()[0].notify_value);
  TEST_ASSERT_FALSE(fake_esy_os_task_get_table()[0].notify_pending);
}
