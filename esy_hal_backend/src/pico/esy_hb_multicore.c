/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_multicore.h"
#include "esy_hb_internal.h"

#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/util/queue.h"

#define ESY_HB_MULTICORE_CORE1 1

typedef struct {
  queue_t queue;
} esy_multicore_queue_ctx_t;

typedef struct {
  bool core1_started;
} esy_hb_multicore_ctx_t;

/* One externally serialized core-0 startup/task owner launches core 1.
 * Other cores and ISR callers cannot read or mutate this lifecycle state. */
static esy_hb_multicore_ctx_t s_ctx;

_Static_assert(ESY_MULTICORE_QUEUE_PRIV_BYTES >= sizeof(esy_multicore_queue_ctx_t),
               "queue->backend.bytes is too small for esy_multicore_queue_ctx_t");

esy_err_t esy_hb_multicore_start_core(uint8_t core_id, void (*entry)(void)) {
  if (!entry || core_id != ESY_HB_MULTICORE_CORE1) return ESY_ERR_INVALID_ARG;
  if (get_core_num() != 0u || esy_is_in_isr()) return ESY_ERR_STATE;
  if (s_ctx.core1_started) return ESY_ERR_BUSY;

  s_ctx.core1_started = true;
  multicore_launch_core1(entry);
  return ESY_OK;
}

esy_err_t esy_hb_multicore_queue_init(esy_multicore_queue_t *queue,
                                          uint32_t item_size, uint32_t depth,
                                          uint8_t *buf, size_t buf_size) {
  if (!queue || item_size == 0 || depth == 0 || !buf) return ESY_ERR_INVALID_ARG;
  if (item_size > UINT16_MAX || depth > UINT16_MAX) return ESY_ERR_INVALID_ARG;
  /* The SDK ring uses one additional slot to distinguish full from empty. */
  if (buf_size / ((size_t)depth + 1u) < item_size) return ESY_ERR_NO_MEM;

  esy_multicore_queue_ctx_t *c =
      (esy_multicore_queue_ctx_t *)queue->backend.bytes;
  lock_init(&c->queue.core, next_striped_spin_lock_num());
  c->queue.data = buf;
  c->queue.element_count = (uint16_t)depth;
  c->queue.element_size = (uint16_t)item_size;
  c->queue.wptr = 0;
  c->queue.rptr = 0;

  queue->ctx = c;

  return ESY_OK;
}

void esy_hb_multicore_queue_deinit(esy_multicore_queue_t *queue) {
  if (!queue) return;

  queue->ctx = NULL;
}

void esy_hb_multicore_queue_send_blocking(esy_multicore_queue_t *queue,
                                              const void *data) {
  if (!queue || !queue->ctx || !data) return;

  esy_multicore_queue_ctx_t *c = (esy_multicore_queue_ctx_t *)queue->ctx;
  queue_add_blocking(&c->queue, data);
}

bool esy_hb_multicore_queue_try_send(esy_multicore_queue_t *queue,
                                         const void *data) {
  if (!queue || !queue->ctx || !data) return false;

  esy_multicore_queue_ctx_t *c = (esy_multicore_queue_ctx_t *)queue->ctx;
  return queue_try_add(&c->queue, data);
}

void esy_hb_multicore_queue_receive_blocking(esy_multicore_queue_t *queue,
                                                 void *data) {
  if (!queue || !queue->ctx || !data) return;

  esy_multicore_queue_ctx_t *c = (esy_multicore_queue_ctx_t *)queue->ctx;
  queue_remove_blocking(&c->queue, data);
}

bool esy_hb_multicore_queue_try_receive(esy_multicore_queue_t *queue,
                                            void *data) {
  if (!queue || !queue->ctx || !data) return false;

  esy_multicore_queue_ctx_t *c = (esy_multicore_queue_ctx_t *)queue->ctx;
  return queue_try_remove(&c->queue, data);
}
