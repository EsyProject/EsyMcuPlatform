/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000u
#endif
#ifndef TEST_TICK_TYPE
#define TEST_TICK_TYPE uint32_t
#endif
#ifndef configRUN_TIME_COUNTER_TYPE
#define configRUN_TIME_COUNTER_TYPE uint32_t
#endif
#ifndef configUSE_TRACE_FACILITY
#define configUSE_TRACE_FACILITY 1
#endif
#define configUSE_TICKLESS_IDLE 1
#ifndef configNUMBER_OF_CORES
#define configNUMBER_OF_CORES 1
#endif
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSTACK_DEPTH_TYPE uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE size_t
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 2
#define portMAX_DELAY ((TickType_t)~(TickType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)((uint64_t)(ms)*configTICK_RATE_HZ/1000u))
typedef int BaseType_t;
typedef uint32_t UBaseType_t;
typedef TEST_TICK_TYPE TickType_t;
typedef uint32_t StackType_t;
typedef void *TaskHandle_t;
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
int xPortInIsrContext(void);
void os_fixture_context_irq_disable(void);
void os_fixture_context_irq_enable(void);
uint32_t os_fixture_context_irq_save(void);
void os_fixture_context_irq_restore(uint32_t key);
#define portDISABLE_INTERRUPTS() os_fixture_context_irq_disable()
#define portENABLE_INTERRUPTS() os_fixture_context_irq_enable()
#define portSET_INTERRUPT_MASK_FROM_ISR() os_fixture_context_irq_save()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(key) os_fixture_context_irq_restore(key)
#endif
