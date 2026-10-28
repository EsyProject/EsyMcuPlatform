/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_ring_buffer.h"

#include <stdint.h>
#include <string.h>

#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

void test_esy_ring_buffer_init_rejects_invalid_arguments(void) {
  esy_ring_buffer_t rb;
  uint8_t storage[8];

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_ring_buffer_init(NULL, storage, sizeof(storage)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_ring_buffer_init(&rb, NULL, sizeof(storage)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_ring_buffer_init(&rb, storage, 0u));
}

void test_esy_ring_buffer_write_read_and_wraparound(void) {
  esy_ring_buffer_t rb;
  uint8_t storage[4];
  uint8_t *span = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_ring_buffer_init(&rb, storage, sizeof(storage)));
  TEST_ASSERT_TRUE(esy_ring_buffer_empty(&rb));
  TEST_ASSERT_FALSE(esy_ring_buffer_full(&rb));

  TEST_ASSERT_EQUAL_UINT32(4u, esy_ring_buffer_get_write_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_PTR(storage, span);
  memcpy(span, "abc", 3u);
  esy_ring_buffer_commit_write(&rb, 3u);

  TEST_ASSERT_EQUAL_UINT32(3u, esy_ring_buffer_used(&rb));
  TEST_ASSERT_EQUAL_UINT32(1u, esy_ring_buffer_free(&rb));

  TEST_ASSERT_EQUAL_UINT32(3u, esy_ring_buffer_get_read_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_PTR(storage, span);
  TEST_ASSERT_EQUAL_MEMORY("abc", span, 3u);
  esy_ring_buffer_commit_read(&rb, 2u);

  TEST_ASSERT_EQUAL_UINT32(1u, esy_ring_buffer_get_write_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_PTR(&storage[3], span);
  span[0] = (uint8_t)'d';
  esy_ring_buffer_commit_write(&rb, 1u);

  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_used(&rb));
  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_free(&rb));

  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_get_read_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_MEMORY("cd", span, 2u);
  esy_ring_buffer_commit_read(&rb, 2u);

  TEST_ASSERT_TRUE(esy_ring_buffer_empty(&rb));

  TEST_ASSERT_EQUAL_UINT32(4u, esy_ring_buffer_get_write_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_PTR(storage, span);
  memcpy(span, "xy", 2u);
  esy_ring_buffer_commit_write(&rb, 2u);

  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_get_read_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_PTR(storage, span);
  TEST_ASSERT_EQUAL_MEMORY("xy", span, 2u);
}

void test_esy_ring_buffer_reset_clears_buffer_state(void) {
  esy_ring_buffer_t rb;
  uint8_t storage[8];
  uint8_t *span = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_ring_buffer_init(&rb, storage, sizeof(storage)));
  TEST_ASSERT_EQUAL_UINT32(sizeof(storage),
                           esy_ring_buffer_get_write_ptr(&rb, &span));
  memset(span, 0x5a, 5u);
  esy_ring_buffer_commit_write(&rb, 5u);

  TEST_ASSERT_EQUAL_UINT32(5u, esy_ring_buffer_used(&rb));
  esy_ring_buffer_reset(&rb);
  TEST_ASSERT_EQUAL_UINT32(0u, esy_ring_buffer_used(&rb));
  TEST_ASSERT_EQUAL_UINT32(sizeof(storage), esy_ring_buffer_free(&rb));
}

void test_esy_ring_buffer_defensive_helpers_are_safe(void) {
  esy_ring_buffer_t rb;
  uint8_t storage[4];
  uint8_t *span = (uint8_t *)0x1234u;

  TEST_ASSERT_EQUAL_UINT32(0u, esy_ring_buffer_used(NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_ring_buffer_free(NULL));
  TEST_ASSERT_TRUE(esy_ring_buffer_empty(NULL));
  TEST_ASSERT_FALSE(esy_ring_buffer_full(NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_ring_buffer_get_write_ptr(NULL, &span));
  TEST_ASSERT_NULL(span);
  TEST_ASSERT_EQUAL_UINT32(0u, esy_ring_buffer_get_read_ptr(NULL, &span));
  TEST_ASSERT_NULL(span);
  esy_ring_buffer_commit_write(NULL, 1u);
  esy_ring_buffer_commit_read(NULL, 1u);
  esy_ring_buffer_reset(NULL);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_ring_buffer_init(&rb, storage, sizeof(storage)));
  esy_ring_buffer_commit_write(&rb, sizeof(storage) + 1u);
  TEST_ASSERT_EQUAL_UINT32(0u, esy_ring_buffer_used(&rb));

  TEST_ASSERT_EQUAL_UINT32(sizeof(storage),
                           esy_ring_buffer_get_write_ptr(&rb, &span));
  memset(span, 0xa5, sizeof(storage));
  esy_ring_buffer_commit_write(&rb, sizeof(storage));
  esy_ring_buffer_commit_write(&rb, 1u);
  TEST_ASSERT_EQUAL_UINT32(sizeof(storage), esy_ring_buffer_used(&rb));

  esy_ring_buffer_commit_read(&rb, sizeof(storage) + 1u);
  TEST_ASSERT_EQUAL_UINT32(sizeof(storage), esy_ring_buffer_used(&rb));
}

void test_esy_ring_buffer_rejects_non_power_of_two_capacity_without_changes(void) {
  esy_ring_buffer_t rb;
  uint8_t storage[8];
  uint8_t rejected_storage[8] = {0};
  const uint32_t invalid_capacities[] = {3u, 5u, 6u, 7u, UINT32_MAX};
  uint8_t *span = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_ring_buffer_init(&rb, storage, sizeof(storage)));
  TEST_ASSERT_EQUAL_UINT32(8u, esy_ring_buffer_get_write_ptr(&rb, &span));
  memcpy(span, "ab", 2u);
  esy_ring_buffer_commit_write(&rb, 2u);

  for (size_t i = 0u;
       i < sizeof(invalid_capacities) / sizeof(invalid_capacities[0]); ++i) {
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
        esy_ring_buffer_init(&rb, rejected_storage, invalid_capacities[i]));
    TEST_ASSERT_EQUAL_PTR(storage, rb.buffer);
    TEST_ASSERT_EQUAL_UINT32(sizeof(storage), rb.capacity);
    TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_used(&rb));
    TEST_ASSERT_EQUAL_UINT32(6u, esy_ring_buffer_free(&rb));
    TEST_ASSERT_EACH_EQUAL_UINT8(0u, rejected_storage, sizeof(rejected_storage));
  }

  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_get_read_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_MEMORY("ab", span, 2u);
}

void test_esy_ring_buffer_single_byte_capacity_supports_full_and_empty(void) {
  esy_ring_buffer_t rb;
  uint8_t storage[1];
  uint8_t *span = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_ring_buffer_init(&rb, storage, sizeof(storage)));
  for (uint8_t value = 0u; value < 3u; ++value) {
    TEST_ASSERT_TRUE(esy_ring_buffer_empty(&rb));
    TEST_ASSERT_EQUAL_UINT32(1u, esy_ring_buffer_get_write_ptr(&rb, &span));
    span[0] = value;
    esy_ring_buffer_commit_write(&rb, 1u);
    TEST_ASSERT_TRUE(esy_ring_buffer_full(&rb));
    TEST_ASSERT_EQUAL_UINT32(0u, esy_ring_buffer_get_write_ptr(&rb, &span));
    TEST_ASSERT_NULL(span);
    TEST_ASSERT_EQUAL_UINT32(1u, esy_ring_buffer_get_read_ptr(&rb, &span));
    TEST_ASSERT_EQUAL_UINT8(value, span[0]);
    esy_ring_buffer_commit_read(&rb, 1u);
  }
  TEST_ASSERT_TRUE(esy_ring_buffer_empty(&rb));
}

void test_esy_ring_buffer_preserves_fifo_across_uint32_counter_wraparound(void) {
  esy_ring_buffer_t rb;
  uint8_t storage[4];
  uint8_t *span = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_ring_buffer_init(&rb, storage, sizeof(storage)));
  /* Seed a reachable empty-ring state while both endpoints are stopped. */
  atomic_store_explicit(&rb.head, UINT32_MAX - 1u, memory_order_relaxed);
  atomic_store_explicit(&rb.tail, UINT32_MAX - 1u, memory_order_relaxed);

  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_get_write_ptr(&rb, &span));
  memcpy(span, "AB", 2u);
  esy_ring_buffer_commit_write(&rb, 2u);
  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_get_write_ptr(&rb, &span));
  memcpy(span, "CD", 2u);
  esy_ring_buffer_commit_write(&rb, 2u);
  TEST_ASSERT_TRUE(esy_ring_buffer_full(&rb));
  TEST_ASSERT_EQUAL_UINT32(4u, esy_ring_buffer_used(&rb));

  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_get_read_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_MEMORY("AB", span, 2u);
  esy_ring_buffer_commit_read(&rb, 2u);
  TEST_ASSERT_EQUAL_UINT32(2u, esy_ring_buffer_get_read_ptr(&rb, &span));
  TEST_ASSERT_EQUAL_MEMORY("CD", span, 2u);
  esy_ring_buffer_commit_read(&rb, 2u);
  TEST_ASSERT_TRUE(esy_ring_buffer_empty(&rb));
  TEST_ASSERT_EQUAL_UINT32(4u, esy_ring_buffer_free(&rb));
}
