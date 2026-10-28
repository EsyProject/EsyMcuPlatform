/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_prio_slot_queue.h"

#include <stdint.h>
#include <string.h>

#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

void test_esy_prio_slot_queue_init_rejects_invalid_arguments(void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t slots[2];
  esy_prio_slot_queue_config_t cfg = esy_prio_slot_queue_default_cfg();

  TEST_ASSERT_NULL(cfg.slots);
  TEST_ASSERT_EQUAL_UINT16(0u, cfg.slot_count);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_prio_slot_queue_init(NULL, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_prio_slot_queue_init(&queue, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_prio_slot_queue_init(&queue, &cfg));

  cfg.slots = slots;
  cfg.slot_count = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_prio_slot_queue_init(&queue, &cfg));
}

void test_maximum_capacity_keeps_last_slot_index_nonnegative(void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t slots[ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT];
  const esy_prio_slot_queue_config_t cfg = {
      .slots = slots, .slot_count = ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT};
  const uintptr_t user_data = (uintptr_t)0xc0ffeeu;

  TEST_ASSERT_EQUAL_UINT32(32768u, ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT);
  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_init(&queue, &cfg));
  for (uint16_t i = 0u; i < INT16_MAX; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_enqueue(&queue, i, 0u));
  }
  const int16_t last = esy_prio_slot_queue_alloc_slot(&queue, user_data);
  TEST_ASSERT_EQUAL_INT16(INT16_MAX, last);
  TEST_ASSERT_EQUAL_INT16(INT16_MAX,
      esy_prio_slot_queue_find_by_user_data(&queue, user_data));
  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_enqueue(&queue,
      (uint16_t)last, UINT8_MAX));
  TEST_ASSERT_EQUAL_UINT16(ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT,
      esy_prio_slot_queue_get_depth(&queue));
  TEST_ASSERT_EQUAL_INT16(INT16_MAX, esy_prio_slot_queue_pick_next(&queue));
  TEST_ASSERT_EQUAL_INT16(-1, esy_prio_slot_queue_alloc_slot(&queue, user_data));
  esy_prio_slot_queue_mark_in_progress(&queue, (uint16_t)last);
  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_requeue(&queue,
      (uint16_t)last, UINT8_MAX));
  TEST_ASSERT_EQUAL_INT16(INT16_MAX, esy_prio_slot_queue_pick_next(&queue));
  esy_prio_slot_queue_free_slot(&queue, (uint16_t)last);
  TEST_ASSERT_EQUAL_INT16(INT16_MAX,
      esy_prio_slot_queue_alloc_slot(&queue, user_data));
  esy_prio_slot_queue_deinit(&queue);
}

void test_oversized_capacity_rejection_preserves_live_queue_and_supplied_slots(void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t live_slots[2];
  esy_prio_slot_t rejected_slots[2];
  const esy_prio_slot_queue_config_t valid = {
      .slots = live_slots, .slot_count = 2u};
  uint8_t queue_before[sizeof(queue)];
  uint8_t live_before[sizeof(live_slots)];
  uint8_t rejected_before[sizeof(rejected_slots)];
  static const uint16_t k_invalid_counts[] = {32769u, UINT16_MAX};

  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_init(&queue, &valid));
  TEST_ASSERT_EQUAL_INT16(0, esy_prio_slot_queue_alloc_slot(&queue, 42u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 0u, 3u));
  memset(rejected_slots, 0xa5, sizeof(rejected_slots));
  memcpy(queue_before, &queue, sizeof(queue));
  memcpy(live_before, live_slots, sizeof(live_slots));
  memcpy(rejected_before, rejected_slots, sizeof(rejected_slots));
  for (size_t i = 0u; i < sizeof(k_invalid_counts) / sizeof(k_invalid_counts[0]); ++i) {
    const esy_prio_slot_queue_config_t invalid = {
        .slots = rejected_slots, .slot_count = k_invalid_counts[i]};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_prio_slot_queue_init(&queue, &invalid));
    TEST_ASSERT_EQUAL_MEMORY(queue_before, &queue, sizeof(queue));
    TEST_ASSERT_EQUAL_MEMORY(live_before, live_slots, sizeof(live_slots));
    TEST_ASSERT_EQUAL_MEMORY(rejected_before, rejected_slots, sizeof(rejected_slots));
    TEST_ASSERT_EQUAL_INT16(0, esy_prio_slot_queue_pick_next(&queue));
    TEST_ASSERT_EQUAL_UINT16(1u, esy_prio_slot_queue_get_depth(&queue));
  }
}

void test_esy_prio_slot_queue_alloc_enqueue_pick_and_free(void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t slots[3];
  esy_prio_slot_queue_config_t cfg = {.slots = slots, .slot_count = 3u};
  int16_t low;
  int16_t high;
  int16_t medium;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_init(&queue, &cfg));
  TEST_ASSERT_TRUE(esy_prio_slot_queue_is_initialized(&queue));
  TEST_ASSERT_EQUAL_UINT16(0u, esy_prio_slot_queue_get_depth(&queue));

  low = esy_prio_slot_queue_alloc_slot(&queue, (uintptr_t)11u);
  high = esy_prio_slot_queue_alloc_slot(&queue, (uintptr_t)22u);
  medium = esy_prio_slot_queue_alloc_slot(&queue, (uintptr_t)33u);
  TEST_ASSERT_EQUAL_INT16(0, low);
  TEST_ASSERT_EQUAL_INT16(1, high);
  TEST_ASSERT_EQUAL_INT16(2, medium);
  TEST_ASSERT_EQUAL_INT16(
      -1, esy_prio_slot_queue_alloc_slot(&queue, (uintptr_t)44u));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_prio_slot_queue_enqueue(&queue, (uint16_t)low, 1u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_prio_slot_queue_enqueue(&queue, (uint16_t)high, 9u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_prio_slot_queue_enqueue(&queue, (uint16_t)medium, 5u));
  TEST_ASSERT_EQUAL_UINT16(3u, esy_prio_slot_queue_get_depth(&queue));
  TEST_ASSERT_EQUAL_INT16(high, esy_prio_slot_queue_pick_next(&queue));

  esy_prio_slot_queue_mark_in_progress(&queue, (uint16_t)high);
  TEST_ASSERT_EQUAL_UINT16(2u, esy_prio_slot_queue_get_depth(&queue));
  TEST_ASSERT_EQUAL(ESY_PRIO_SLOT_STATE_IN_PROGRESS,
                    esy_prio_slot_queue_get_slot_state(&queue, (uint16_t)high));
  TEST_ASSERT_EQUAL_UINT((uintptr_t)22u, esy_prio_slot_queue_get_slot_user_data(
                                             &queue, (uint16_t)high));
  TEST_ASSERT_EQUAL_INT16(
      high, esy_prio_slot_queue_find_by_user_data(&queue, (uintptr_t)22u));

  esy_prio_slot_queue_free_slot(&queue, (uint16_t)high);
  TEST_ASSERT_EQUAL(ESY_PRIO_SLOT_STATE_FREE,
                    esy_prio_slot_queue_get_slot_state(&queue, (uint16_t)high));
}

void test_esy_prio_slot_queue_fifo_within_same_priority_and_requeue(void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t slots[2];
  esy_prio_slot_queue_config_t cfg = {.slots = slots, .slot_count = 2u};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_init(&queue, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 0u, 4u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 1u, 4u));
  TEST_ASSERT_EQUAL_INT16(0, esy_prio_slot_queue_pick_next(&queue));

  esy_prio_slot_queue_mark_in_progress(&queue, 0u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_requeue(&queue, 0u, 4u));
  TEST_ASSERT_EQUAL_INT16(1, esy_prio_slot_queue_pick_next(&queue));
}

void test_fifo_survives_middle_head_tail_removal_and_slot_reuse(void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t slots[4];
  esy_prio_slot_queue_config_t cfg = {.slots = slots, .slot_count = 4u};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_init(&queue, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 3u, 7u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 2u, 7u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 0u, 7u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 1u, 7u));
  esy_prio_slot_queue_free_slot(&queue, 2u); /* Middle. */
  esy_prio_slot_queue_free_slot(&queue, 1u); /* Tail. */
  esy_prio_slot_queue_free_slot(&queue, 1u); /* Repeated free is harmless. */
  TEST_ASSERT_EQUAL_INT16(3, esy_prio_slot_queue_pick_next(&queue));
  esy_prio_slot_queue_mark_in_progress(&queue, 3u); /* Head. */
  esy_prio_slot_queue_mark_in_progress(&queue, 3u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_requeue(&queue, 3u, 7u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 2u, 7u));
  TEST_ASSERT_EQUAL_INT16(0, esy_prio_slot_queue_pick_next(&queue));
  esy_prio_slot_queue_free_slot(&queue, 0u);
  TEST_ASSERT_EQUAL_INT16(3, esy_prio_slot_queue_pick_next(&queue));
  esy_prio_slot_queue_free_slot(&queue, 3u);
  TEST_ASSERT_EQUAL_INT16(2, esy_prio_slot_queue_pick_next(&queue));
  esy_prio_slot_queue_free_slot(&queue, 2u);
  TEST_ASSERT_EQUAL_INT16(-1, esy_prio_slot_queue_pick_next(&queue));
  TEST_ASSERT_EQUAL_UINT16(0u, esy_prio_slot_queue_get_depth(&queue));
  esy_prio_slot_queue_deinit(&queue);
  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_init(&queue, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 1u, 7u));
  TEST_ASSERT_EQUAL_INT16(1, esy_prio_slot_queue_pick_next(&queue));
}

void test_esy_prio_slot_queue_rejects_invalid_state_transitions(void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t slots[1];
  esy_prio_slot_queue_config_t cfg = {.slots = slots, .slot_count = 1u};

  TEST_ASSERT_EQUAL_INT16(-1, esy_prio_slot_queue_pick_next(NULL));
  TEST_ASSERT_FALSE(esy_prio_slot_queue_is_initialized(NULL));
  TEST_ASSERT_EQUAL_UINT16(0u, esy_prio_slot_queue_get_depth(NULL));
  TEST_ASSERT_EQUAL(ESY_PRIO_SLOT_STATE_FREE,
                    esy_prio_slot_queue_get_slot_state(NULL, 0u));
  TEST_ASSERT_EQUAL_UINT((uintptr_t)0u,
                         esy_prio_slot_queue_get_slot_user_data(NULL, 0u));
  TEST_ASSERT_EQUAL_INT16(
      -1, esy_prio_slot_queue_find_by_user_data(NULL, (uintptr_t)1u));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_init(&queue, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_enqueue(&queue, 0u, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_prio_slot_queue_enqueue(&queue, 0u, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_prio_slot_queue_enqueue(&queue, 1u, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_prio_slot_queue_requeue(&queue, 0u, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_prio_slot_queue_requeue(&queue, 1u, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_prio_slot_queue_requeue(NULL, 0u, 1u));
  TEST_ASSERT_EQUAL_INT16(
      -1, esy_prio_slot_queue_find_by_user_data(&queue, (uintptr_t)9u));

  esy_prio_slot_queue_deinit(&queue);
  TEST_ASSERT_FALSE(esy_prio_slot_queue_is_initialized(&queue));

  esy_prio_slot_queue_deinit(NULL);
  esy_prio_slot_queue_free_slot(NULL, 0u);
  esy_prio_slot_queue_free_slot(&queue, 0u);
  esy_prio_slot_queue_mark_in_progress(NULL, 0u);
  esy_prio_slot_queue_mark_in_progress(&queue, 0u);
}

void test_esy_prio_slot_queue_ignores_invalid_indices_for_void_operations(
    void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t slots[1];
  esy_prio_slot_queue_config_t cfg = {.slots = slots, .slot_count = 1u};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_prio_slot_queue_init(&queue, &cfg));

  esy_prio_slot_queue_free_slot(&queue, 1u);
  esy_prio_slot_queue_mark_in_progress(&queue, 1u);
  TEST_ASSERT_EQUAL_UINT16(0u, esy_prio_slot_queue_get_depth(&queue));
  TEST_ASSERT_EQUAL(ESY_PRIO_SLOT_STATE_FREE,
                    esy_prio_slot_queue_get_slot_state(&queue, 1u));
  TEST_ASSERT_EQUAL_UINT((uintptr_t)0u,
                         esy_prio_slot_queue_get_slot_user_data(&queue, 1u));
}
