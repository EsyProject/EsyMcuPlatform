/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_ring_buffer.h"

#include <stddef.h>

#include "esy_common/utils/base/esy_bits.h"

esy_err_t esy_ring_buffer_init(esy_ring_buffer_t *rb, uint8_t *buffer,
                               uint32_t capacity) {
  if (!rb || !buffer || !esy_is_power_of_two_u32(capacity)) {
    return ESY_ERR_INVALID_ARG;
  }

  rb->buffer = buffer;
  rb->capacity = capacity;
  rb->mask = capacity - 1u;

  atomic_store_explicit(&rb->head, 0u, memory_order_relaxed);
  atomic_store_explicit(&rb->tail, 0u, memory_order_relaxed);
  return ESY_OK;
}

void esy_ring_buffer_reset(esy_ring_buffer_t *rb) {
  if (!rb) {
    return;
  }

  /* Reset is only supported when neither endpoint is active. */
  atomic_store_explicit(&rb->head, 0u, memory_order_relaxed);
  atomic_store_explicit(&rb->tail, 0u, memory_order_relaxed);
}

uint32_t esy_ring_buffer_used(const esy_ring_buffer_t *rb) {
  if (!rb) {
    return 0u;
  }

  /* Acquire observes commits published by the opposite endpoint. */
  uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
  return head - tail;
}

uint32_t esy_ring_buffer_free(const esy_ring_buffer_t *rb) {
  if (!rb) {
    return 0u;
  }

  uint32_t used = esy_ring_buffer_used(rb);
  return (used <= rb->capacity) ? (rb->capacity - used) : 0u;
}

bool esy_ring_buffer_empty(const esy_ring_buffer_t *rb) {
  if (!rb) {
    return true;
  }
  return esy_ring_buffer_used(rb) == 0u;
}

bool esy_ring_buffer_full(const esy_ring_buffer_t *rb) {
  if (!rb) {
    return false;
  }
  return esy_ring_buffer_used(rb) >= rb->capacity;
}

uint32_t esy_ring_buffer_get_write_ptr(esy_ring_buffer_t *rb, uint8_t **out) {
  if (out) {
    *out = NULL;
  }
  if (!rb || !out || !rb->buffer || rb->capacity == 0u) {
    return 0u;
  }

  /* The producer reads the consumer-owned tail with acquire ordering. */
  uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

  uint32_t used = head - tail;
  if (used >= rb->capacity) {
    *out = NULL;
    return 0u;
  }

  uint32_t free_bytes = rb->capacity - used;
  uint32_t pos = head & rb->mask;
  uint32_t linear = rb->capacity - pos;
  uint32_t n = (free_bytes < linear) ? free_bytes : linear;

  *out = rb->buffer + pos;
  return n;
}

void esy_ring_buffer_commit_write(esy_ring_buffer_t *rb, uint32_t len) {
  if (!rb || len == 0u) {
    return;
  }
  if (len > esy_ring_buffer_free(rb)) {
    return;
  }

  /* Release makes buffer writes visible before the head update. */
  uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);

  atomic_store_explicit(&rb->head, head + len, memory_order_release);
}

uint32_t esy_ring_buffer_get_read_ptr(esy_ring_buffer_t *rb, uint8_t **out) {
  if (out) {
    *out = NULL;
  }
  if (!rb || !out || !rb->buffer || rb->capacity == 0u) {
    return 0u;
  }

  /* The consumer reads the producer-owned head with acquire ordering. */
  uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

  uint32_t used = head - tail;
  if (used == 0u) {
    *out = NULL;
    return 0u;
  }

  uint32_t pos = tail & rb->mask;
  uint32_t linear = rb->capacity - pos;
  uint32_t n = (used < linear) ? used : linear;

  *out = rb->buffer + pos;
  return n;
}

void esy_ring_buffer_commit_read(esy_ring_buffer_t *rb, uint32_t len) {
  if (!rb || len == 0u) {
    return;
  }
  if (len > esy_ring_buffer_used(rb)) {
    return;
  }

  /* Release keeps data reads before the tail update. */
  uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
  atomic_store_explicit(&rb->tail, tail + len, memory_order_release);
}
