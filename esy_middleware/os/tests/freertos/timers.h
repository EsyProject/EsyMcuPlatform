/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_TIMERS_H
#define TEST_TIMERS_H
#include "FreeRTOS.h"
#include <stdbool.h>
typedef struct StaticTimer_s *TimerHandle_t;
typedef struct StaticTimer_s {
  void *id;
  void (*callback)(TimerHandle_t);
  bool active, deleted, reload;
} StaticTimer_t;
typedef void (*PendedFunction_t)(void *, uint32_t);
TimerHandle_t xTimerCreateStatic(const char *, TickType_t, BaseType_t, void *, void (*)(TimerHandle_t), StaticTimer_t *);
void *pvTimerGetTimerID(TimerHandle_t timer);
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks);
BaseType_t xTimerStop(TimerHandle_t timer, TickType_t ticks);
BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t ticks);
BaseType_t xTimerPendFunctionCall(PendedFunction_t function, void *arg, uint32_t value, TickType_t ticks);
#endif
