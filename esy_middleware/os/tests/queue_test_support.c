/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "queue_test_support.h"

#include <stddef.h>
#include <string.h>

static queue_test_support_ctx_t s_ctx;

void queue_test_support_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.send_result = pdPASS;
  s_ctx.receive_result = pdPASS;
  s_ctx.send_from_isr_result = pdPASS;
  s_ctx.receive_from_isr_result = pdPASS;
}

queue_test_support_ctx_t *queue_test_support_get_ctx(void) { return &s_ctx; }

QueueHandle_t xQueueCreateStatic(UBaseType_t queue_length,
                                 UBaseType_t item_size, uint8_t *queue_storage,
                                 StaticQueue_t *queue_buffer) {
  queue_buffer->data = queue_storage;
  queue_buffer->depth = queue_length;
  queue_buffer->item_size = item_size;
  queue_buffer->count = 0;

  return s_ctx.create_should_fail ? NULL : queue_buffer;
}

void vQueueDelete(QueueHandle_t queue) {
  (void)queue;
  s_ctx.delete_calls++;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t ticks_to_wait) {
  (void)queue;
  (void)item;
  s_ctx.send_ticks_to_wait = ticks_to_wait;
  if (s_ctx.emulate_storage && s_ctx.send_result == pdPASS) {
    if (queue->count == queue->depth) return pdFALSE;
    memcpy(queue->data + queue->count++ * queue->item_size, item, queue->item_size);
  }
  return s_ctx.send_result;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t ticks_to_wait) {
  (void)queue;
  (void)item;
  s_ctx.receive_ticks_to_wait = ticks_to_wait;
  if (s_ctx.emulate_storage && s_ctx.receive_result == pdPASS) {
    if (!queue->count) {
      if (s_ctx.on_empty) s_ctx.on_empty();
      return pdFALSE;
    }
    memcpy(item, queue->data, queue->item_size);
    --queue->count;
    memmove(queue->data, queue->data + queue->item_size, queue->count * queue->item_size);
  }
  return s_ctx.receive_result;
}

BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *higher_priority_task_woken) {
  (void)queue;
  (void)item;
  *higher_priority_task_woken = s_ctx.higher_priority_task_woken;
  return s_ctx.emulate_storage ? xQueueSend(queue, item, 0) : s_ctx.send_from_isr_result;
}

BaseType_t xQueueReceiveFromISR(QueueHandle_t queue, void *item,
                                BaseType_t *higher_priority_task_woken) {
  (void)queue;
  (void)item;
  *higher_priority_task_woken = s_ctx.higher_priority_task_woken;
  return s_ctx.emulate_storage ? xQueueReceive(queue, item, 0) : s_ctx.receive_from_isr_result;
}
