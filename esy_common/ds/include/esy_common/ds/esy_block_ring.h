/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_DS_ESY_BLOCK_RING_H
#define ESY_COMMON_DS_ESY_BLOCK_RING_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
#include <atomic>
#define ESY_BLOCK_RING_ALIGNAS(n) alignas(n)
#define ESY_BLOCK_RING_ATOMIC_UINT32 std::atomic<uint32_t>
#else
#include <stdatomic.h> /* C11 atomics */
#include <stdalign.h>  /* C11 alignment: alignas */
#define ESY_BLOCK_RING_ALIGNAS(n) alignas(n)
#define ESY_BLOCK_RING_ATOMIC_UINT32 _Atomic uint32_t
#endif

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_block_ring.h
 * @brief Fixed-size block ring buffer (SPSC: single-producer /
 * single-consumer).
 *
 * ## Overview
 * This module implements an SPSC ring of fixed-size blocks backed by
 * caller-provided memory.
 *
 * Typical usage:
 * - Producer calls get_write_block() -> writes one block -> commit_write().
 * - Consumer calls get_read_block()  -> reads one block  -> commit_read().
 *
 * ## Concurrency model (SPSC)
 * - Only the producer thread/core may call:
 *   - esy_block_ring_get_write_block()
 *   - esy_block_ring_commit_write()
 * - Only the consumer thread/core may call:
 *   - esy_block_ring_get_read_block()
 *   - esy_block_ring_commit_read()
 * - Both sides may call these read-only helpers at any time:
 *   - esy_block_ring_used(), esy_block_ring_free()
 *   - esy_block_ring_empty(), esy_block_ring_full()
 * - esy_block_ring_init() and esy_block_ring_reset() must only be called when
 *   neither endpoint is active.
 *
 * If you violate the SPSC restriction (e.g., multiple producers), the behavior
 * is undefined.
 *
 * ## Requirements
 * - block_count must be a power-of-two.
 * - One slot is reserved to distinguish full from empty, so usable capacity is
 *   `block_count - 1`.
 * - buffer capacity must be at least block_size * block_count bytes.
 *
 * ## Cache and false sharing
 * head and tail are placed on separate cache lines (when supported) to reduce
 * false sharing on SMP / multi-core MCUs.
 */

/** @brief Typical D-Cache line size (e.g., STM32H7). Harmless on non-cached
 * platforms. */
#define ESY_CACHE_LINE 32u

/**
 * @brief Block ring buffer state (SPSC).
 *
 * Notes:
 * - This is a POD-style structure intentionally exposed to users.
 * - The ring does not own the memory; `buffer` is owned by the caller and must
 * remain valid for the lifetime of the ring.
 */
typedef struct esy_block_ring_s {
  /** @brief Backing data region base address (capacity = block_size *
   * block_count). */
  uint8_t *buffer;

  /** @brief Size of each block in bytes (must be > 0). */
  uint32_t block_size;

  /** @brief Number of blocks (must be a power-of-two). */
  uint32_t block_count;

  /** @brief Convenience mask = block_count - 1 for fast modulo operations. */
  uint32_t mask;

  /**
   * @brief Write index (producer-owned writer).
   *
   * The producer is the only writer of head. The consumer reads head to
   * determine available data.
   */
  ESY_BLOCK_RING_ALIGNAS(ESY_CACHE_LINE) ESY_BLOCK_RING_ATOMIC_UINT32 head;

  /**
   * @brief Read index (consumer-owned writer).
   *
   * The consumer is the only writer of tail. The producer reads tail to
   * determine available free space.
   */
  ESY_BLOCK_RING_ALIGNAS(ESY_CACHE_LINE) ESY_BLOCK_RING_ATOMIC_UINT32 tail;
} esy_block_ring_t;

#undef ESY_BLOCK_RING_ALIGNAS
#undef ESY_BLOCK_RING_ATOMIC_UINT32

/**
 * @brief Initialize the block ring (does not allocate memory).
 *
 * On success:
 * - rb->mask is set to (block_count - 1)
 * - head and tail are reset to 0
 *
 * The ring object and backing bytes may contain arbitrary initial values.
 * Repeated init replaces the binding and discards queued blocks without
 * clearing their bytes. Stop both endpoints and retire every returned block
 * pointer before init/reset; old pointers must not be accessed or committed
 * afterward. The caller retains the backing region and its resource cleanup.
 * Invalid arguments leave the ring and supplied buffer unchanged. After a
 * failed first init, retry init successfully before any other ring operation;
 * a rejected reinit leaves the previous initialized ring usable.
 *
 * @param[in,out] rb          Ring object.
 * @param[in]     buffer      Backing memory base address, capacity >=
 * block_size * block_count.
 * @param[in]     block_size  Size of each block in bytes (>0). Align to 4/8
 * bytes if possible.
 * @param[in]     block_count Number of blocks (must be power-of-two).
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  Invalid arguments (NULL pointers, size=0, count
 * not power-of-two, etc.).
 */
extern esy_err_t esy_block_ring_init(esy_block_ring_t *rb, uint8_t *buffer,
                                     uint32_t block_size, uint32_t block_count);

/**
 * @brief Get a pointer to the next writable block (producer).
 *
 * Returns NULL when the ring is full. The returned pointer remains valid until
 * esy_block_ring_commit_write() is called (or the ring is reset).
 *
 * @param[in,out] rb Ring.
 * @return Writable block pointer; NULL if the ring is full.
 */
extern uint8_t *esy_block_ring_get_write_block(esy_block_ring_t *rb);

/**
 * @brief Commit one produced block (producer).
 *
 * Advances the write index by one block. Must be paired with the previous
 * esy_block_ring_get_write_block() call that returned non-NULL.
 *
 * @param[in,out] rb Ring.
 */
extern void esy_block_ring_commit_write(esy_block_ring_t *rb);

/**
 * @brief Get a pointer to the next readable block (consumer).
 *
 * Returns NULL when the ring is empty. The returned pointer remains valid until
 * esy_block_ring_commit_read() is called (or the ring is reset).
 *
 * @param[in,out] rb Ring.
 * @return Readable block pointer; NULL if the ring is empty.
 */
extern uint8_t *esy_block_ring_get_read_block(esy_block_ring_t *rb);

/**
 * @brief Commit one consumed block (consumer).
 *
 * Advances the read index by one block. Must be paired with the previous
 * esy_block_ring_get_read_block() call that returned non-NULL.
 *
 * @param[in,out] rb Ring.
 */
extern void esy_block_ring_commit_read(esy_block_ring_t *rb);

/**
 * @brief Get number of used blocks (lock-free read).
 *
 * @param[in] rb Ring.
 * @return Number of blocks currently stored (0..block_count - 1).
 */
extern uint32_t esy_block_ring_used(const esy_block_ring_t *rb);

/**
 * @brief Get number of free blocks (lock-free read).
 *
 * @param[in] rb Ring.
 * @return Number of blocks available for writing (0..block_count - 1).
 */
extern uint32_t esy_block_ring_free(const esy_block_ring_t *rb);

/**
 * @brief Check whether the ring is full.
 *
 * @param[in] rb Ring.
 * @retval true  Full (get_write_block would return NULL).
 * @retval false Not full.
 */
extern bool esy_block_ring_full(const esy_block_ring_t *rb);

/**
 * @brief Check whether the ring is empty.
 *
 * @param[in] rb Ring.
 * @retval true  Empty (get_read_block would return NULL).
 * @retval false Not empty.
 */
extern bool esy_block_ring_empty(const esy_block_ring_t *rb);

/**
 * @brief Discard queued blocks and set head and tail to zero.
 *
 * Requires successful init. Stop both endpoints and retire all returned block
 * pointers before resetting; no later access/commit may use an old grant.
 * The buffer binding and payload bytes are unchanged. NULL is a no-op.
 * This is not a substitute for initializing the ring and its buffer binding.
 *
 * @param[in,out] rb Ring.
 */
extern void esy_block_ring_reset(esy_block_ring_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_DS_ESY_BLOCK_RING_H */
