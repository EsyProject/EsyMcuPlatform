/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_multicore.h"
#include <string.h>

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) {
  platform_test_reset();
}

void tearDown(void) {}

static void secondary_entry(void) {}

void test_multicore_queues_share_immutable_ops_and_zero_after_deinit(void) {
  esy_multicore_queue_t first = {0};
  esy_multicore_queue_t second = {0};
  uint8_t first_storage[8];
  uint8_t second_storage[8];
  uint32_t item = 1u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_multicore_queue_init(
      &first, sizeof(item), 1u, first_storage, sizeof(first_storage)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_multicore_queue_init(
      &second, sizeof(item), 1u, second_storage, sizeof(second_storage)));
  TEST_ASSERT_NOT_NULL(first.ops);
  TEST_ASSERT_EQUAL_PTR(first.ops, second.ops);
  esy_hal_multicore_queue_deinit(&first);
  TEST_ASSERT_NULL(first.ops);
  TEST_ASSERT_FALSE(esy_hal_multicore_queue_try_send(&first, &item));
  TEST_ASSERT_TRUE(esy_hal_multicore_queue_try_send(&second, &item));
  esy_hal_multicore_queue_deinit(&second);
}

void test_esy_hal_multicore_start_core_validates_entry(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_multicore_start_core(1u, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_multicore_start_core(1u, secondary_entry));

  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_start_calls);
  TEST_ASSERT_EQUAL_UINT8(1u, platform_test_state()->last_core_id);
}

void test_esy_hal_multicore_queue_binds_ops_after_driver_init(void) {
  esy_multicore_queue_t queue = {0};
  uint32_t item = 7u;
  uint8_t storage[20] = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_multicore_queue_init(NULL, sizeof(item), 1u,
                                                        storage, sizeof(storage)));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_multicore_queue_init(&queue, sizeof(item), 4u,
                                                        storage, sizeof(storage)));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_NOT_NULL(queue.ops->vt_queue_send_blocking);
  TEST_ASSERT_NOT_NULL(queue.ops->vt_queue_try_send);
  TEST_ASSERT_NOT_NULL(queue.ops->vt_queue_receive_blocking);
  TEST_ASSERT_NOT_NULL(queue.ops->vt_queue_try_receive);

  esy_hal_multicore_queue_send_blocking(&queue, &item);
  TEST_ASSERT_TRUE(esy_hal_multicore_queue_try_send(&queue, &item));
  TEST_ASSERT_TRUE(esy_hal_multicore_queue_try_receive(&queue, &item));
  TEST_ASSERT_EQUAL_UINT32(2u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_read_calls);
}

void test_multicore_queue_rejects_short_storage_and_invalid_sizes_before_dispatch(void) {
  esy_multicore_queue_t queue;
  esy_multicore_queue_t before;
  uint8_t storage[20];
  memset(&queue, 0xa5, sizeof(queue));
  memcpy(&before, &queue, sizeof(queue));

  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_hal_multicore_queue_init(
      &queue, 4u, 4u, storage, 19u));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_hal_multicore_queue_init(
      &queue, 4u, 1u, storage, 4u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_multicore_queue_init(
      &queue, UINT16_MAX + 1u, 1u, storage, sizeof(storage)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_multicore_queue_init(
      &queue, 1u, UINT16_MAX + 1u, storage, sizeof(storage)));
  TEST_ASSERT_EQUAL_MEMORY(&before, &queue, sizeof(queue));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
}

