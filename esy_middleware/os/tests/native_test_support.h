/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NATIVE_TEST_SUPPORT_H
#define NATIVE_TEST_SUPPORT_H
#include "task.h"
#include "timers.h"
#include "semphr.h"
#include "message_buffer.h"
#include <stdbool.h>
typedef struct {
  bool create_fail, command_fail, fence_fail, reset_fail, isr;
  unsigned deletes, command_count, fence_count, critical_depth, isr_entries, task_entries, yields;
  TickType_t ticks, period;
  uint32_t local_irq_mask;
  eNotifyAction action;
  StaticTask_t *last_task;
  TimerHandle_t last_timer;
  TaskHandle_t current;
  void *task_mux, *isr_mux;
  void (*on_delay)(void);
  void (*on_unlock)(void);
} native_test_support_ctx_t;
void native_test_support_reset(void);
native_test_support_ctx_t *native_test_support_get(void);
void native_drain_commands(void);
void native_fire_timer(void);
void native_run_worker(void);
#endif
