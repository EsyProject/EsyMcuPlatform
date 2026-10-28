/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COMMON_DS_ESY_RING_BUFFER_H
#define ESY_COMMON_DS_ESY_RING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
#include <atomic>
#define ESY_RING_BUFFER_ATOMIC_UINT32 std::atomic<uint32_t>
#else
#include <stdatomic.h>
#define ESY_RING_BUFFER_ATOMIC_UINT32 _Atomic uint32_t
#endif

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_ring_buffer.h
 * @brief Byte-stream ring buffer (SPSC: single-producer / single-consumer,
 * zero-copy API).
 *
 * ## Overview
 * This module implements a byte-oriented ring buffer designed for SPSC usage:
 * - One producer thread/core advances `head`.
 * - One consumer thread/core advances `tail`.
 *
 * The API is "zero-copy" in the sense that users can obtain a pointer to a
 * contiguous region inside the ring (linear span), write/read directly, and
 * then commit the exact number of bytes via commit APIs.
 *
 * ## Concurrency model (SPSC)
 * - Producer-only APIs:
 *   - esy_ring_buffer_get_write_ptr()
 *   - esy_ring_buffer_commit_write()
 * - Consumer-only APIs:
 *   - esy_ring_buffer_get_read_ptr()
 *   - esy_ring_buffer_commit_read()
 * - Read-only helper APIs (safe for both sides):
 *   - esy_ring_buffer_used(), esy_ring_buffer_free()
 *   - esy_ring_buffer_empty(), esy_ring_buffer_full()
 * - esy_ring_buffer_init() and esy_ring_buffer_reset() must only be called when
 *   neither endpoint is active.
 *
 * If you violate the SPSC restriction (multiple producers or consumers),
 * behavior is undefined.
 *
 * ## Atomicity and visibility
 * - `head` and `tail` are C11 atomics to ensure cross-core visibility.
 * - Correctness assumes the implementation uses appropriate memory ordering in
 * `.c`.
 *
 * ## Capacity and mask
 * - `capacity` must be a nonzero power of two, including a capacity of one.
 * - `mask` is always `capacity - 1`. Masking preserves buffer positions when
 *   the unsigned 32-bit head and tail counters wrap around.
 *
 * ## Memory ownership
 * - The backing `buffer` is owned by the caller and must remain valid for the
 *   lifetime of the ring buffer.
 */

typedef struct esy_ring_buffer_s {
  /** @brief Backing buffer base address. Owned by the caller. */
  uint8_t *buffer;

  /** @brief Buffer capacity in bytes (a nonzero power of two). */
  uint32_t capacity;

  /**
   * @brief Wrap-around mask, always equal to `capacity - 1`.
   */
  uint32_t mask;

  /**
   * @brief Write index (producer-owned writer).
   *
   * The producer is the only writer of `head`. The consumer reads `head` to
   * determine how much data is available.
   */
  ESY_RING_BUFFER_ATOMIC_UINT32 head;

  /**
   * @brief Read index (consumer-owned writer).
   *
   * The consumer is the only writer of `tail`. The producer reads `tail` to
   * determine how much free space is available.
   */
  ESY_RING_BUFFER_ATOMIC_UINT32 tail;
} esy_ring_buffer_t;

#undef ESY_RING_BUFFER_ATOMIC_UINT32

/**
 * @brief Initialize a ring buffer (does not allocate memory).
 *
 * Capacity must be a nonzero power of two. On success, head and tail are reset
 * to 0, and `mask` is set to `capacity - 1`. Invalid arguments leave the ring
 * object and backing buffer unchanged. No prior initialization of the ring
 * object or buffer is required. Reinitialization resets a stopped ring; neither
 * endpoint may be active.
 * Retire all returned read/write spans before reinit; they must not be accessed
 * or committed afterward. Payload bytes are not cleared. After a failed first
 * init, retry init successfully before other ring operations; a rejected reinit
 * preserves the previous initialized ring.
 *
 * @param[in,out] rb       Ring buffer object (must not be NULL).
 * @param[in]     buffer   Backing buffer pointer allocated by the caller (must
 * not be NULL).
 * @param[in]     capacity Buffer size in bytes (a nonzero power of two).
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  NULL pointers, zero capacity, or a capacity that
 *                             is not a power of two.
 */
extern esy_err_t esy_ring_buffer_init(esy_ring_buffer_t *rb, uint8_t *buffer,
                                      uint32_t capacity);

/**
 * @brief Discard queued bytes and set head and tail to zero.
 *
 * Requires successful init. Stop both endpoints and retire every returned span
 * first; an old span must not be accessed or committed afterward. The buffer
 * binding and payload bytes remain unchanged. NULL is a no-op. Reset alone
 * does not initialize the buffer binding after a failed first init.
 *
 * @param[in,out] rb Ring buffer object.
 */
extern void esy_ring_buffer_reset(esy_ring_buffer_t *rb);

/**
 * @brief Get the number of bytes currently stored in the ring.
 *
 * @param[in] rb Ring buffer object.
 * @return Used byte count.
 */
extern uint32_t esy_ring_buffer_used(const esy_ring_buffer_t *rb);

/**
 * @brief Get the number of free bytes available for writing.
 *
 * @param[in] rb Ring buffer object.
 * @return Free byte count.
 */
extern uint32_t esy_ring_buffer_free(const esy_ring_buffer_t *rb);

/**
 * @brief Check whether the ring buffer is empty.
 *
 * @param[in] rb Ring buffer object.
 * @retval true  Empty (no readable data).
 * @retval false Not empty.
 */
extern bool esy_ring_buffer_empty(const esy_ring_buffer_t *rb);

/**
 * @brief Check whether the ring buffer is full.
 *
 * @param[in] rb Ring buffer object.
 * @retval true  Full (no writable space).
 * @retval false Not full.
 */
extern bool esy_ring_buffer_full(const esy_ring_buffer_t *rb);

/* ================== zero-copy producer side ================== */

/**
 * @brief Get the current contiguous writable region.
 *
 * The returned region is linear (it will not wrap around the end of the
 * buffer). The producer may write up to the returned length, then must call
 * esy_ring_buffer_commit_write() with the actual number of bytes written.
 *
 * @param[in,out] rb   Ring buffer object.
 * @param[out]    out  Output pointer to the writable span inside `buffer`.
 * @return Number of bytes writable in this contiguous span (0 if full).
 */
extern uint32_t esy_ring_buffer_get_write_ptr(esy_ring_buffer_t *rb,
                                              uint8_t **out);

/**
 * @brief Commit bytes written by the producer (advance head).
 *
 * @param[in,out] rb  Ring buffer object.
 * @param[in]     len Number of bytes actually written (must be <= last
 * get_write_ptr result).
 */
extern void esy_ring_buffer_commit_write(esy_ring_buffer_t *rb, uint32_t len);

/* ================== zero-copy consumer side ================== */

/**
 * @brief Get the current contiguous readable region.
 *
 * The returned region is linear (it will not wrap around the end of the
 * buffer). The consumer may read up to the returned length, then must call
 * esy_ring_buffer_commit_read() with the actual number of bytes consumed.
 *
 * @param[in,out] rb   Ring buffer object.
 * @param[out]    out  Output pointer to the readable span inside `buffer`.
 * @return Number of bytes readable in this contiguous span (0 if empty).
 */
extern uint32_t esy_ring_buffer_get_read_ptr(esy_ring_buffer_t *rb,
                                             uint8_t **out);

/**
 * @brief Commit bytes consumed by the consumer (advance tail).
 *
 * @param[in,out] rb  Ring buffer object.
 * @param[in]     len Number of bytes actually consumed (must be <= last
 * get_read_ptr result).
 */
extern void esy_ring_buffer_commit_read(esy_ring_buffer_t *rb, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_DS_ESY_RING_BUFFER_H */
