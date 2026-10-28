/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_os_queue.h"

#include <string.h>

#ifndef FAKE_ESY_OS_QUEUE_MAX_OBJECTS
#define FAKE_ESY_OS_QUEUE_MAX_OBJECTS 16
#endif

typedef struct esy_os_queue_s {
  bool used;
  uint8_t *buffer;
  size_t item_size;
  size_t max_items;
  size_t head;
  size_t tail;
  size_t count;
} esy_os_queue_impl_t;

typedef struct {
  esy_os_queue_impl_t queues[FAKE_ESY_OS_QUEUE_MAX_OBJECTS];
  esy_err_t create_result;
  esy_err_t send_result;
  esy_err_t receive_result;
  fake_esy_os_queue_observations_t observations;
  void (*send_hook)(void);
  void (*receive_hook)(void);
  uint32_t isr_yield_call;
} fake_esy_os_queue_ctx_t;
static fake_esy_os_queue_ctx_t s_ctx;

static esy_os_queue_impl_t *fake_os_queue_alloc_slot(void) {
  uint32_t i;

  for (i = 0u; i < FAKE_ESY_OS_QUEUE_MAX_OBJECTS; ++i) {
    if (!s_ctx.queues[i].used) {
      memset(&s_ctx.queues[i], 0, sizeof(s_ctx.queues[i]));
      s_ctx.queues[i].used = true;
      return &s_ctx.queues[i];
    }
  }

  return NULL;
}

void fake_esy_os_queue_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.create_result = ESY_OK;
  s_ctx.send_result = ESY_OK;
  s_ctx.receive_result = ESY_OK;
}

void fake_esy_os_queue_set_create_result(esy_err_t ret) {
  s_ctx.create_result = ret;
}

void fake_esy_os_queue_set_send_result(esy_err_t ret) {
  s_ctx.send_result = ret;
}

void fake_esy_os_queue_set_receive_result(esy_err_t ret) {
  s_ctx.receive_result = ret;
}

const fake_esy_os_queue_observations_t *fake_esy_os_queue_get_observations(void) {
  return &s_ctx.observations;
}

void fake_esy_os_queue_set_send_hook(void (*hook)(void)) {
  s_ctx.send_hook = hook;
}

void fake_esy_os_queue_set_receive_hook(void (*hook)(void)) {
  s_ctx.receive_hook = hook;
}

void fake_esy_os_queue_set_isr_yield_on_call(uint32_t call) {
  s_ctx.isr_yield_call = call;
}

esy_err_t esy_os_queue_create_static(size_t item_size,
                                     size_t max_items,
                                     void *queue_buffer,
                                     esy_os_queue_static_storage_t *storage,
                                     esy_os_queue_t **out_queue) {
  esy_os_queue_impl_t *slot;

  s_ctx.observations.create_count++;
  if (out_queue) *out_queue = NULL;

  if (s_ctx.create_result != ESY_OK) {
    return s_ctx.create_result;
  }

  if ((item_size == 0u) || (max_items == 0u) || (queue_buffer == NULL) ||
      (out_queue == NULL) || (storage == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  slot = fake_os_queue_alloc_slot();
  if (slot == NULL) {
    return ESY_ERR_NO_MEM;
  }

  slot->buffer = (uint8_t *)queue_buffer;
  slot->item_size = item_size;
  slot->max_items = max_items;
  *out_queue = (esy_os_queue_t *)slot;
  return ESY_OK;
}

void esy_os_queue_delete(esy_os_queue_t *queue) {
  esy_os_queue_impl_t *q = (esy_os_queue_impl_t *)queue;

  if ((q == NULL) || !q->used) {
    return;
  }

  s_ctx.observations.delete_count++;
  memset(q, 0, sizeof(*q));
}

esy_err_t esy_os_queue_send(esy_os_queue_t *queue,
                            const void *item,
                            uint32_t timeout_ms) {
  esy_os_queue_impl_t *q = (esy_os_queue_impl_t *)queue;
  size_t offset;

  s_ctx.observations.send_count++;
  s_ctx.observations.last_send_timeout_ms = timeout_ms;
  if (s_ctx.send_hook != NULL) {
    void (*hook)(void) = s_ctx.send_hook;
    s_ctx.send_hook = NULL;
    hook();
  }

  if (s_ctx.send_result != ESY_OK) {
    return s_ctx.send_result;
  }

  if ((q == NULL) || (item == NULL) || !q->used) {
    return ESY_ERR_INVALID_ARG;
  }

  if (q->count >= q->max_items) {
    return ESY_ERR_TIMEOUT;
  }

  offset = q->tail * q->item_size;
  memcpy(&q->buffer[offset], item, q->item_size);
  q->tail = (q->tail + 1u) % q->max_items;
  q->count++;
  return ESY_OK;
}

esy_err_t esy_os_queue_receive(esy_os_queue_t *queue,
                               void *item,
                               uint32_t timeout_ms) {
  esy_os_queue_impl_t *q = (esy_os_queue_impl_t *)queue;
  size_t offset;

  s_ctx.observations.receive_count++;
  s_ctx.observations.last_receive_timeout_ms = timeout_ms;
  if (s_ctx.receive_hook != NULL) {
    void (*hook)(void) = s_ctx.receive_hook;
    s_ctx.receive_hook = NULL;
    hook();
  }

  if (s_ctx.receive_result != ESY_OK) {
    return s_ctx.receive_result;
  }

  if ((q == NULL) || (item == NULL) || !q->used) {
    return ESY_ERR_INVALID_ARG;
  }

  if (q->count == 0u) {
    return ESY_ERR_TIMEOUT;
  }

  offset = q->head * q->item_size;
  memcpy(item, &q->buffer[offset], q->item_size);
  q->head = (q->head + 1u) % q->max_items;
  q->count--;
  return ESY_OK;
}

esy_err_t esy_os_queue_send_from_isr(esy_os_queue_t *queue,
                                     const void *item,
                                     bool *need_yield) {
  uint32_t call = ++s_ctx.observations.send_from_isr_count;
  esy_err_t result = esy_os_queue_send(queue, item, 0u);
  if (need_yield != NULL) {
    *need_yield = result == ESY_OK && call == s_ctx.isr_yield_call;
  }
  return result;
}

esy_err_t esy_os_queue_receive_from_isr(esy_os_queue_t *queue,
                                        void *item,
                                        bool *need_yield) {
  if (need_yield != NULL) {
    *need_yield = false;
  }
  return esy_os_queue_receive(queue, item, 0u);
}

__attribute__((constructor)) static void fake_esy_os_queue_ctor(void) {
  fake_esy_os_queue_reset();
}
