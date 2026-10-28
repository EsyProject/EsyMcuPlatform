/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_QUEUE_H
#define TEST_QUEUE_H

#include <stdint.h>

#include "FreeRTOS.h"

typedef struct StaticQueue_s {
  uint8_t *data;
  size_t depth, item_size, count;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;

QueueHandle_t xQueueCreateStatic(UBaseType_t queue_length,
                                 UBaseType_t item_size, uint8_t *queue_storage,
                                 StaticQueue_t *queue_buffer);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t ticks_to_wait);
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *higher_priority_task_woken);
BaseType_t xQueueReceiveFromISR(QueueHandle_t queue, void *item,
                                BaseType_t *higher_priority_task_woken);

#endif /* TEST_QUEUE_H */
