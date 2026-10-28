/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_block_pool.h"

#include <stddef.h> /* uintptr_t */
#include <stdint.h>

static bool ptr_in_pool(const esy_block_pool_t *pool, const uint8_t *block) {
  if (!pool || !block || !pool->buffer || pool->block_size == 0u ||
      pool->block_count == 0u) {
    return false;
  }

  uintptr_t base = (uintptr_t)pool->buffer;
  uintptr_t end =
      base + (uintptr_t)pool->block_size * (uintptr_t)pool->block_count;
  uintptr_t value = (uintptr_t)block;
  return (value >= base) && (value < end);
}

static uint32_t ptr_to_index_fast(const esy_block_pool_t *pool,
                                  const uint8_t *block) {
  uintptr_t diff = (uintptr_t)block - (uintptr_t)pool->buffer;
  return (uint32_t)(diff / pool->block_size);
}

esy_err_t esy_block_pool_init(esy_block_pool_t *pool, uint8_t *buffer,
                              uint32_t block_size, uint32_t block_count,
                              uint32_t *index_stack) {
  if (!pool || !buffer || !index_stack) {
    return ESY_ERR_INVALID_ARG;
  }
  if (block_size == 0u || block_count == 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  pool->buffer = buffer;
  pool->block_size = block_size;
  pool->block_count = block_count;
  pool->index_stack = index_stack;

  /* Initialize the free stack as LIFO [N-1, N-2, ..., 0]. */
  pool->top = (int32_t)block_count - 1;
  for (uint32_t i = 0; i < block_count; ++i) {
    pool->index_stack[i] = (block_count - 1u) - i;
  }

  return ESY_OK;
}

uint8_t *esy_block_pool_acquire(esy_block_pool_t *pool) {
  if (!pool || !pool->buffer || !pool->index_stack || pool->block_size == 0u) {
    return NULL;
  }
  if (pool->top < 0) {
    return NULL;
  }

  uint32_t idx = pool->index_stack[pool->top--];
  return pool->buffer + (uintptr_t)idx * (uintptr_t)pool->block_size;
}

void esy_block_pool_release(esy_block_pool_t *pool, uint8_t *block) {
  if (!pool || !block || !pool->index_stack || pool->block_size == 0u) {
    return;
  }
  if ((uint32_t)(pool->top + 1) >= pool->block_count) {
    return;
  }

  if (!ptr_in_pool(pool, block)) {
    return;
  }
  uintptr_t offset = (uintptr_t)block - (uintptr_t)pool->buffer;
  if ((offset % pool->block_size) != 0u) {
    return;
  }

  uint32_t idx = ptr_to_index_fast(pool, block);
  pool->index_stack[++pool->top] = idx;
}

uint32_t esy_block_pool_used(const esy_block_pool_t *pool) {
  if (!pool) {
    return 0u;
  }

  uint32_t free_cnt = (uint32_t)(pool->top + 1);
  if (free_cnt > pool->block_count) {
    free_cnt = pool->block_count;
  }
  return pool->block_count - free_cnt;
}

uint32_t esy_block_pool_free(const esy_block_pool_t *pool) {
  if (!pool) {
    return 0u;
  }

  uint32_t free_cnt = (uint32_t)(pool->top + 1);
  if (free_cnt > pool->block_count) {
    free_cnt = pool->block_count;
  }
  return free_cnt;
}

bool esy_block_pool_empty(const esy_block_pool_t *pool) {
  if (!pool) {
    return true;
  }
  return (pool->top < 0);
}

bool esy_block_pool_full(const esy_block_pool_t *pool) {
  if (!pool) {
    return false;
  }
  return ((uint32_t)(pool->top + 1) == pool->block_count);
}

void esy_block_pool_reset(esy_block_pool_t *pool) {
  if (!pool || !pool->index_stack) {
    return;
  }

  pool->top = (int32_t)pool->block_count - 1;
  for (uint32_t i = 0; i < pool->block_count; ++i) {
    pool->index_stack[i] = (pool->block_count - 1u) - i;
  }
}

esy_err_t esy_block_pool_ptr_to_index(const esy_block_pool_t *pool,
                                      const uint8_t *block, uint32_t *out_idx) {
  if (!pool || !block || !out_idx || pool->block_size == 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!ptr_in_pool(pool, block)) {
    return ESY_ERR_INVALID_ARG;
  }

  uintptr_t offset = (uintptr_t)block - (uintptr_t)pool->buffer;
  if ((offset % pool->block_size) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  *out_idx = ptr_to_index_fast(pool, block);
  return ESY_OK;
}

esy_err_t esy_block_pool_index_to_ptr(const esy_block_pool_t *pool,
                                      uint32_t index, uint8_t **out_ptr) {
  if (out_ptr) {
    *out_ptr = NULL;
  }
  if (!pool || !out_ptr || !pool->buffer || pool->block_size == 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (index >= pool->block_count) {
    return ESY_ERR_INVALID_ARG;
  }

  *out_ptr = pool->buffer + (uintptr_t)index * (uintptr_t)pool->block_size;
  return ESY_OK;
}
