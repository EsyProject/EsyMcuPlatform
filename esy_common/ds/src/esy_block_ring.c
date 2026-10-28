/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_block_ring.h"
#include "esy_common/utils/base/esy_bits.h"

#include <stddef.h>

esy_err_t esy_block_ring_init(esy_block_ring_t *rb, uint8_t *buffer,
                              uint32_t block_size, uint32_t block_count) {
  if (!rb || !buffer || block_size == 0u || block_count < 2u ||
      !esy_is_power_of_two_u32(block_count)) {
    return ESY_ERR_INVALID_ARG;
  }

  rb->buffer = buffer;
  rb->block_size = block_size;
  rb->block_count = block_count;
  rb->mask = block_count - 1u;

  atomic_store_explicit(&rb->head, 0u, memory_order_relaxed);
  atomic_store_explicit(&rb->tail, 0u, memory_order_relaxed);
  return ESY_OK;
}

/* Producer side: get a writable block. */
uint8_t *esy_block_ring_get_write_block(esy_block_ring_t *rb) {
  if (!rb || !rb->buffer) {
    return NULL;
  }

  /* Reading the producer-owned head only needs relaxed ordering. */
  uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
  /* Reading the consumer-owned tail needs acquire ordering. */
  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

  uint32_t next = (head + 1u) & rb->mask;
  if (next == tail) {
    return NULL;
  }
  return rb->buffer + (head & rb->mask) * rb->block_size;
}

/* Producer side: commit a completed write. */
void esy_block_ring_commit_write(esy_block_ring_t *rb) {
  if (!rb || !rb->buffer || rb->block_count == 0u || esy_block_ring_full(rb)) {
    return;
  }

  uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
  uint32_t next = (head + 1u) & rb->mask;
  /* Release makes block writes visible before the head update. */
  atomic_store_explicit(&rb->head, next, memory_order_release);
}

/* Consumer side: get a readable block. */
uint8_t *esy_block_ring_get_read_block(esy_block_ring_t *rb) {
  if (!rb || !rb->buffer) {
    return NULL;
  }

  /* Reading the consumer-owned tail only needs relaxed ordering. */
  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
  /* Reading the producer-owned head needs acquire ordering. */
  uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

  if (tail == head) {
    return NULL;
  }
  return rb->buffer + (tail & rb->mask) * rb->block_size;
}

/* Consumer side: commit a completed read. */
void esy_block_ring_commit_read(esy_block_ring_t *rb) {
  if (!rb || !rb->buffer || rb->block_count == 0u || esy_block_ring_empty(rb)) {
    return;
  }

  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
  uint32_t next = (tail + 1u) & rb->mask;
  /* Release keeps data reads before the tail update. */
  atomic_store_explicit(&rb->tail, next, memory_order_release);
}

/* Read-only query. Values are approximate snapshots under concurrency. */
uint32_t esy_block_ring_used(const esy_block_ring_t *rb) {
  if (!rb || rb->block_count == 0u) {
    return 0u;
  }

  uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
  if (head >= tail) {
    return head - tail;
  }
  return (rb->block_count - tail) + head;
}

uint32_t esy_block_ring_free(const esy_block_ring_t *rb) {
  if (!rb || rb->block_count == 0u) {
    return 0u;
  }

  return rb->block_count - esy_block_ring_used(rb) - 1u;
}

bool esy_block_ring_full(const esy_block_ring_t *rb) {
  if (!rb || rb->block_count == 0u) {
    return false;
  }

  uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
  return (((head + 1u) & rb->mask) == tail);
}

bool esy_block_ring_empty(const esy_block_ring_t *rb) {
  if (!rb || rb->block_count == 0u) {
    return true;
  }

  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
  uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
  return (tail == head);
}

void esy_block_ring_reset(esy_block_ring_t *rb) {
  if (!rb) {
    return;
  }

  /* Reset is only supported when neither endpoint is active. */
  atomic_store_explicit(&rb->head, 0u, memory_order_relaxed);
  atomic_store_explicit(&rb->tail, 0u, memory_order_relaxed);
}
