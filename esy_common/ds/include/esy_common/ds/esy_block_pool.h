/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_DS_ESY_BLOCK_POOL_H
#define ESY_COMMON_DS_ESY_BLOCK_POOL_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_block_pool.h
 * @brief Fixed-size block memory pool (no OS dependency).
 *
 * ## Overview
 * This module implements a simple fixed-size block allocator backed by
 * caller-provided memory.
 *
 * The pool is intended for embedded / deterministic environments:
 * - No dynamic allocation inside the module.
 * - O(1) acquire/release.
 * - Minimal runtime overhead.
 *
 * ## Memory model
 * - The caller provides a contiguous buffer of size `block_size * block_count`.
 * - The caller provides an index stack array of length `block_count`, used as a
 *   free-block LIFO stack.
 *
 * ## Concurrency
 * This module is **NOT** thread-safe and does not provide internal locking.
 * If the pool is accessed concurrently (tasks/ISRs), the caller must protect
 * operations with a critical section.
 *
 * ## Validity requirements
 * - `block_size` and `block_count` must be non-zero.
 * - `buffer` must have at least `block_size * block_count` bytes.
 * - `index_stack` must have `block_count` entries.
 * - A released block must have been returned by the same pool and must still be
 *   acquired at the time of release.
 * - Duplicate release is a caller contract violation. It is not fully detected,
 *   because doing so would require O(n) bookkeeping and break the pool's O(1)
 *   acquire/release contract.
 */

/**
 * @brief Fixed-size block pool state.
 *
 * Notes:
 * - This is a POD-style structure intentionally exposed to users.
 * - The pool does not own the memory; `buffer` and `index_stack` are owned by
 * the caller and must remain valid for the lifetime of the pool.
 */
typedef struct esy_block_pool_s {
  /** @brief Backing memory base address (contiguous). */
  uint8_t *buffer;

  /** @brief Size of each block in bytes (must be > 0). */
  uint32_t block_size;

  /** @brief Number of blocks (must be > 0). */
  uint32_t block_count;

  /**
   * @brief Free-index stack (length must equal `block_count`).
   *
   * The stack stores indices in the range [0, block_count-1].
   * The pool uses LIFO semantics for better cache locality and simplicity.
   */
  uint32_t *index_stack;

  /**
   * @brief Stack top pointer.
   *
   * - `top == -1` means the free stack is empty (pool exhausted).
   * - `top == (int32_t)block_count - 1` means the pool is full (all blocks
   * free).
   */
  int32_t top;
} esy_block_pool_t;

/**
 * @brief Initialize a block pool (does not allocate memory).
 *
 * Initialization fills the free-index stack so that all blocks are initially
 * free; payload bytes are not cleared. The pool object and index stack may
 * contain arbitrary initial bytes. No existing pool state is read on success.
 * Repeated init replaces the binding and marks every supplied block free.
 * Before init/reset, serialize all users and end all outstanding block borrows;
 * an old borrowed block must not be accessed or released afterward. The caller
 * retains both backing regions; init/reset performs no external cleanup.
 * Invalid arguments leave the pool and both supplied regions unchanged. After
 * a failed first init, retry init successfully before any other pool operation;
 * a rejected reinit preserves the previously initialized pool for continued use.
 *
 * @param[in,out] pool        Pool object to initialize.
 * @param[in]     buffer      Contiguous backing memory, size >= block_size *
 * block_count.
 * @param[in]     block_size  Size of each block in bytes (>0). Align to 4/8
 * bytes if possible.
 * @param[in]     block_count Number of blocks (>0).
 * @param[in]     index_stack Free-index stack array, length == block_count.
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  Invalid arguments.
 */
extern esy_err_t esy_block_pool_init(esy_block_pool_t *pool, uint8_t *buffer,
                                     uint32_t block_size, uint32_t block_count,
                                     uint32_t *index_stack);

/**
 * @brief Acquire one block from the pool.
 *
 * @param[in,out] pool Pool.
 * @return Pointer to the beginning of the acquired block; NULL if no free
 * blocks exist.
 *
 * @note Not thread-safe. Protect with a critical section if used concurrently.
 */
extern uint8_t *esy_block_pool_acquire(esy_block_pool_t *pool);

/**
 * @brief Release one block back to the pool.
 *
 * @param[in,out] pool  Pool.
 * @param[in]     block Block pointer to release (must originate from this
 * pool).
 *
 * @note Not thread-safe. Protect with a critical section if used concurrently.
 */
extern void esy_block_pool_release(esy_block_pool_t *pool, uint8_t *block);

/**
 * @brief Get number of used blocks.
 *
 * @param[in] pool Pool.
 * @return Number of blocks currently acquired (in-use).
 */
extern uint32_t esy_block_pool_used(const esy_block_pool_t *pool);

/**
 * @brief Get number of free blocks.
 *
 * @param[in] pool Pool.
 * @return Number of blocks currently available for acquire.
 */
extern uint32_t esy_block_pool_free(const esy_block_pool_t *pool);

/**
 * @brief Check whether the pool is empty (no free blocks).
 *
 * @param[in] pool Pool.
 * @retval true  Pool exhausted (acquire would return NULL).
 * @retval false Pool has at least one free block.
 */
extern bool esy_block_pool_empty(const esy_block_pool_t *pool);

/**
 * @brief Check whether the pool is full (all blocks are free).
 *
 * @param[in] pool Pool.
 * @retval true  All blocks are free (nothing currently acquired).
 * @retval false At least one block is in use.
 */
extern bool esy_block_pool_full(const esy_block_pool_t *pool);

/**
 * @brief Reset the pool (mark all blocks as free).
 *
 * This does not clear the backing memory; it only restores the free-index
 * stack. Requires a successfully initialized pool. Stop all users and end every
 * block borrow first; no old block may be accessed or released after reset.
 * The binding and caller-owned storage remain in place. NULL is a no-op.
 *
 * @param[in,out] pool Pool.
 */
extern void esy_block_pool_reset(esy_block_pool_t *pool);

/**
 * @brief Convert a block pointer to an index within the pool.
 *
 * @param[in]  pool    Pool.
 * @param[in]  block   Block pointer.
 * @param[out] out_idx Output index.
 * @retval ESY_OK               Success (pointer is within pool range and
 * properly aligned).
 * @retval ESY_ERR_INVALID_ARG  Failure (pointer is out of range, misaligned, or
 * args invalid).
 */
extern esy_err_t esy_block_pool_ptr_to_index(const esy_block_pool_t *pool,
                                             const uint8_t *block,
                                             uint32_t *out_idx);

/**
 * @brief Convert an index within the pool to a block pointer.
 *
 * @param[in]  pool    Pool.
 * @param[in]  index   Block index (0..block_count-1).
 * @param[out] out_ptr Output block pointer.
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  Failure (index out of range or args invalid).
 */
extern esy_err_t esy_block_pool_index_to_ptr(const esy_block_pool_t *pool,
                                             uint32_t index, uint8_t **out_ptr);

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_DS_ESY_BLOCK_POOL_H */
