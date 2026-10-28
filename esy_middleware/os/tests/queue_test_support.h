/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef QUEUE_TEST_SUPPORT_H
#define QUEUE_TEST_SUPPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "queue.h"

typedef struct queue_test_support_ctx_s {
  BaseType_t send_result;
  BaseType_t receive_result;
  BaseType_t send_from_isr_result;
  BaseType_t receive_from_isr_result;
  BaseType_t higher_priority_task_woken;
  TickType_t send_ticks_to_wait;
  TickType_t receive_ticks_to_wait;
  uint32_t delete_calls;
  bool create_should_fail;
  bool emulate_storage;
  void (*on_empty)(void);
} queue_test_support_ctx_t;

void queue_test_support_reset(void);
queue_test_support_ctx_t *queue_test_support_get_ctx(void);

#endif /* QUEUE_TEST_SUPPORT_H */
