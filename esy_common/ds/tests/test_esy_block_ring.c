/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_block_ring.h"

#include <stdint.h>
#include <string.h>

#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

void test_esy_block_ring_init_rejects_invalid_arguments(void) {
  esy_block_ring_t ring;
  uint8_t storage[16];

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_ring_init(NULL, storage, 4u, 4u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_ring_init(&ring, NULL, 4u, 4u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_ring_init(&ring, storage, 0u, 4u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_ring_init(&ring, storage, 4u, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_block_ring_init(&ring, storage, 4u, 3u));
}

void test_esy_block_ring_write_read_and_wraparound(void) {
  esy_block_ring_t ring;
  uint8_t storage[16];
  uint8_t *block = NULL;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_block_ring_init(&ring, storage, 4u, 4u));
  TEST_ASSERT_TRUE(esy_block_ring_empty(&ring));
  TEST_ASSERT_FALSE(esy_block_ring_full(&ring));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_block_ring_used(&ring));
  TEST_ASSERT_EQUAL_UINT32(3u, esy_block_ring_free(&ring));

  block = esy_block_ring_get_write_block(&ring);
  TEST_ASSERT_EQUAL_PTR(&storage[0], block);
  memcpy(block, "abcd", 4u);
  esy_block_ring_commit_write(&ring);

  block = esy_block_ring_get_write_block(&ring);
  TEST_ASSERT_EQUAL_PTR(&storage[4], block);
  memcpy(block, "efgh", 4u);
  esy_block_ring_commit_write(&ring);

  block = esy_block_ring_get_write_block(&ring);
  TEST_ASSERT_EQUAL_PTR(&storage[8], block);
  memcpy(block, "ijkl", 4u);
  esy_block_ring_commit_write(&ring);

  TEST_ASSERT_TRUE(esy_block_ring_full(&ring));
  TEST_ASSERT_NULL(esy_block_ring_get_write_block(&ring));
  TEST_ASSERT_EQUAL_UINT32(3u, esy_block_ring_used(&ring));

  block = esy_block_ring_get_read_block(&ring);
  TEST_ASSERT_EQUAL_MEMORY("abcd", block, 4u);
  esy_block_ring_commit_read(&ring);

  block = esy_block_ring_get_write_block(&ring);
  TEST_ASSERT_EQUAL_PTR(&storage[12], block);
  memcpy(block, "mnop", 4u);
  esy_block_ring_commit_write(&ring);

  TEST_ASSERT_EQUAL_MEMORY("efgh", esy_block_ring_get_read_block(&ring), 4u);
  esy_block_ring_commit_read(&ring);
  TEST_ASSERT_EQUAL_MEMORY("ijkl", esy_block_ring_get_read_block(&ring), 4u);
  esy_block_ring_commit_read(&ring);
  TEST_ASSERT_EQUAL_MEMORY("mnop", esy_block_ring_get_read_block(&ring), 4u);
  esy_block_ring_commit_read(&ring);

  TEST_ASSERT_TRUE(esy_block_ring_empty(&ring));
}

void test_esy_block_ring_defensive_helpers_are_safe(void) {
  esy_block_ring_t ring;
  uint8_t storage[8];

  TEST_ASSERT_NULL(esy_block_ring_get_write_block(NULL));
  TEST_ASSERT_NULL(esy_block_ring_get_read_block(NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_block_ring_used(NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, esy_block_ring_free(NULL));
  TEST_ASSERT_FALSE(esy_block_ring_full(NULL));
  TEST_ASSERT_TRUE(esy_block_ring_empty(NULL));
  esy_block_ring_commit_write(NULL);
  esy_block_ring_commit_read(NULL);
  esy_block_ring_reset(NULL);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_block_ring_init(&ring, storage, 4u, 2u));
  esy_block_ring_commit_read(&ring);
  TEST_ASSERT_TRUE(esy_block_ring_empty(&ring));
  TEST_ASSERT_NOT_NULL(esy_block_ring_get_write_block(&ring));
  esy_block_ring_commit_write(&ring);
  TEST_ASSERT_EQUAL_UINT32(1u, esy_block_ring_used(&ring));
  esy_block_ring_commit_write(&ring);
  TEST_ASSERT_TRUE(esy_block_ring_full(&ring));
}

void test_esy_block_ring_reset_clears_state(void) {
  esy_block_ring_t ring;
  uint8_t storage[8];

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_block_ring_init(&ring, storage, 4u, 2u));
  TEST_ASSERT_NOT_NULL(esy_block_ring_get_write_block(&ring));
  esy_block_ring_commit_write(&ring);
  TEST_ASSERT_FALSE(esy_block_ring_empty(&ring));

  esy_block_ring_reset(&ring);
  TEST_ASSERT_TRUE(esy_block_ring_empty(&ring));
  TEST_ASSERT_EQUAL_UINT32(1u, esy_block_ring_free(&ring));
}
