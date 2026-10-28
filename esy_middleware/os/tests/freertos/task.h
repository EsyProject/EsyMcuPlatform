/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_TASK_H
#define TEST_TASK_H

#include "FreeRTOS.h"

typedef enum {
  eAbortSleep = 0,
  eStandardSleep,
  eNoTasksWaitingTimeout
} eSleepModeStatus;

typedef enum {
  eRunning, eReady, eBlocked, eSuspended, eDeleted, eInvalid
} eTaskState;

/* Minimal native snapshot contract used only by OS wrapper tests. */
typedef struct {
  TaskHandle_t xHandle;
  const char *pcTaskName;
  UBaseType_t xTaskNumber;
  eTaskState eCurrentState;
  UBaseType_t uxCurrentPriority;
  UBaseType_t uxBasePriority;
  configRUN_TIME_COUNTER_TYPE ulRunTimeCounter;
  uint32_t usStackHighWaterMark;
} TaskStatus_t;

UBaseType_t uxTaskGetSystemState(TaskStatus_t *items, UBaseType_t capacity,
                                configRUN_TIME_COUNTER_TYPE *total);
void vTaskSuspendAll(void);
int xTaskResumeAll(void);

void vTaskStartScheduler(void);
eSleepModeStatus eTaskConfirmSleepModeStatus(void);
void vTaskStepTick(TickType_t ticks);
BaseType_t xTaskIncrementTick(void);
uint32_t os_fixture_tick_core_id(void);
void os_fixture_tick_isr_lock(void);
void os_fixture_tick_isr_unlock(void);
#if configNUMBER_OF_CORES > 1 && !defined(ESY_PLATFORM_ESP32)
#define portGET_CORE_ID() os_fixture_tick_core_id()
#define portGET_ISR_LOCK() os_fixture_tick_isr_lock()
#define portRELEASE_ISR_LOCK() os_fixture_tick_isr_unlock()
#endif
int xTaskGetSchedulerState(void);
void os_fixture_disable_interrupts(void);
void os_fixture_yield_from_isr(int should_yield);

#define taskDISABLE_INTERRUPTS()  os_fixture_disable_interrupts()
#ifdef ESY_PLATFORM_ESP32
#define portYIELD_FROM_ISR() os_fixture_yield_from_isr(1)
#else
#define portYIELD_FROM_ISR(value) os_fixture_yield_from_isr(value)
#endif

typedef struct {
  void (*entry)(void *);
  void *arg;
  eTaskState state;
} StaticTask_t;
typedef enum {eNoAction, eSetBits, eIncrement, eSetValueWithOverwrite,
              eSetValueWithoutOverwrite} eNotifyAction;
TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name,
    uint32_t depth, void *arg, UBaseType_t priority, StackType_t *stack, StaticTask_t *storage);
void vTaskDelete(TaskHandle_t task);
void vTaskSuspend(TaskHandle_t task);
eTaskState eTaskGetState(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
void vTaskDelayUntil(TickType_t *previous, TickType_t period);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyWait(uint32_t entry, uint32_t exit, uint32_t *value, TickType_t ticks);
BaseType_t xTaskNotifyStateClear(TaskHandle_t task);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken);
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction action, BaseType_t *woken);
uint32_t native_enter(int isr, void *mux);
void native_exit(int isr, void *mux, uint32_t key);
void native_yield(void);
#ifndef ESY_PLATFORM_ESP32
#define taskENTER_CRITICAL() ((void)native_enter(0, NULL))
#define taskEXIT_CRITICAL() native_exit(0, NULL, 0)
#define taskENTER_CRITICAL_FROM_ISR() native_enter(1, NULL)
#define taskEXIT_CRITICAL_FROM_ISR(key) native_exit(1, NULL, key)
#endif
#define taskYIELD() native_yield()
#endif /* TEST_TASK_H */
