/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_block_pool.h"

#include <stdint.h>

#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

void test_esy_block_pool_init_rejects_invalid_arguments(void) {
  esy_block_pool_t pool;
  uint8_t storage[16];
  uint32_t stack[4];

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_init(NULL, storage, 4u, 4u, stack));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_init(&pool, NULL, 4u, 4u, stack));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_init(&pool, storage, 0u, 4u, stack));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_init(&pool, storage, 4u, 0u, stack));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_init(&pool, storage, 4u, 4u, NULL));
}

void test_esy_block_pool_defensive_helpers_are_safe(void) {
  esy_block_pool_t pool = {0};
  uint8_t *ptr = (uint8_t *)0x1234u;
  uint32_t index = 0u;

  TEST_ASSERT_NULL(esy_block_pool_acquire(NULL));
  TEST_ASSERT_NULL(esy_block_pool_acquire(&pool));
  esy_block_pool_release(NULL, NULL);
  esy_block_pool_reset(NULL);
  esy_block_pool_reset(&pool);

  TEST_ASSERT_EQUAL_UINT32(0u, esy_block_pool_used(NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_block_pool_free(NULL));
  TEST_ASSERT_TRUE(esy_block_pool_empty(NULL));
  TEST_ASSERT_FALSE(esy_block_pool_full(NULL));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_ptr_to_index(NULL, NULL, &index));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_index_to_ptr(NULL, 0u, &ptr));
  TEST_ASSERT_NULL(ptr);
}

void test_esy_block_pool_acquire_release_and_index_mapping(void) {
  esy_block_pool_t pool;
  uint8_t storage[16];
  uint32_t stack[4];
  uint8_t *blocks[4];
  uint32_t index = 0u;
  uint8_t *ptr = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_block_pool_init(&pool, storage, 4u, 4u, stack));
  TEST_ASSERT_TRUE(esy_block_pool_full(&pool));
  TEST_ASSERT_FALSE(esy_block_pool_empty(&pool));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_block_pool_used(&pool));
  TEST_ASSERT_EQUAL_UINT32(4u, esy_block_pool_free(&pool));

  blocks[0] = esy_block_pool_acquire(&pool);
  blocks[1] = esy_block_pool_acquire(&pool);
  blocks[2] = esy_block_pool_acquire(&pool);
  blocks[3] = esy_block_pool_acquire(&pool);

  TEST_ASSERT_EQUAL_PTR(&storage[0], blocks[0]);
  TEST_ASSERT_EQUAL_PTR(&storage[4], blocks[1]);
  TEST_ASSERT_EQUAL_PTR(&storage[8], blocks[2]);
  TEST_ASSERT_EQUAL_PTR(&storage[12], blocks[3]);
  TEST_ASSERT_NULL(esy_block_pool_acquire(&pool));
  TEST_ASSERT_TRUE(esy_block_pool_empty(&pool));
  TEST_ASSERT_EQUAL_UINT32(4u, esy_block_pool_used(&pool));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_block_pool_ptr_to_index(&pool, blocks[2], &index));
  TEST_ASSERT_EQUAL_UINT32(2u, index);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_block_pool_index_to_ptr(&pool, 2u, &ptr));
  TEST_ASSERT_EQUAL_PTR(blocks[2], ptr);

  esy_block_pool_release(&pool, blocks[2]);
  TEST_ASSERT_EQUAL_UINT32(3u, esy_block_pool_used(&pool));
  TEST_ASSERT_EQUAL_PTR(blocks[2], esy_block_pool_acquire(&pool));
}

void test_esy_block_pool_rejects_invalid_release_and_mapping(void) {
  esy_block_pool_t pool;
  uint8_t storage[16];
  uint8_t foreign[4];
  uint32_t stack[4];
  uint32_t index = 0u;
  uint8_t *ptr = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_block_pool_init(&pool, storage, 4u, 4u, stack));

  esy_block_pool_release(NULL, storage);
  esy_block_pool_release(&pool, NULL);
  esy_block_pool_release(&pool, foreign);
  esy_block_pool_release(&pool, &storage[1]);
  TEST_ASSERT_EQUAL_UINT32(4u, esy_block_pool_free(&pool));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_block_pool_ptr_to_index(
                                                   &pool, &storage[1], &index));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_ptr_to_index(&pool, foreign, &index));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_pool_index_to_ptr(&pool, 4u, &ptr));
  TEST_ASSERT_NULL(ptr);
}

void test_esy_block_pool_reset_restores_free_stack(void) {
  esy_block_pool_t pool;
  uint8_t storage[8];
  uint32_t stack[2];

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_block_pool_init(&pool, storage, 4u, 2u, stack));
  TEST_ASSERT_NOT_NULL(esy_block_pool_acquire(&pool));
  TEST_ASSERT_EQUAL_UINT32(1u, esy_block_pool_used(&pool));

  esy_block_pool_reset(&pool);
  TEST_ASSERT_TRUE(esy_block_pool_full(&pool));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_block_pool_used(&pool));
}

void test_esy_block_pool_release_to_full_pool_is_ignored(void) {
  esy_block_pool_t pool;
  uint8_t storage[8];
  uint32_t stack[2];
  uint8_t *block = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_block_pool_init(&pool, storage, 4u, 2u, stack));
  block = esy_block_pool_acquire(&pool);
  TEST_ASSERT_NOT_NULL(block);

  esy_block_pool_release(&pool, block);
  TEST_ASSERT_TRUE(esy_block_pool_full(&pool));

  esy_block_pool_release(&pool, block);
  TEST_ASSERT_TRUE(esy_block_pool_full(&pool));
  TEST_ASSERT_EQUAL_UINT32(2u, esy_block_pool_free(&pool));
}
