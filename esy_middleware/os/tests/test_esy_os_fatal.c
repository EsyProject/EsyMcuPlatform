/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/os/esy_os_fatal.h"
#include "esy_hal/esy_hal_system.h"
#include "FreeRTOS.h"
#include "task.h"
#include "unity.h"

#include <setjmp.h>
#include <string.h>

void vApplicationStackOverflowHook(TaskHandle_t task, char *name);

typedef struct {
  jmp_buf escape;
  int scheduler_state;
  uint32_t scheduler_queries;
  uint32_t dispatch_queries;
  uint32_t capture_calls;
  uint32_t halt_calls;
  uint32_t unrelated_calls;
  bool return_from_handler;
  esy_fault_reason_t reason;
  uint32_t detail0, detail1;
} fatal_test_ctx_t;

static fatal_test_ctx_t s_ctx;

int xTaskGetSchedulerState(void) {
  ++s_ctx.scheduler_queries;
  return s_ctx.scheduler_state;
}

void os_fixture_disable_interrupts(void) {
  ++s_ctx.halt_calls;
  longjmp(s_ctx.escape, 2);
}

static void capture(esy_fault_reason_t reason, uint32_t detail0,
                     uint32_t detail1) {
  ++s_ctx.capture_calls;
  s_ctx.reason = reason;
  s_ctx.detail0 = detail0;
  s_ctx.detail1 = detail1;
  s_ctx.dispatch_queries = s_ctx.scheduler_queries;
  if (!s_ctx.return_from_handler) longjmp(s_ctx.escape, 1);
}

/* Unused providers link the complete OS wrapper. Fatal dispatch must not call
 * any of these scheduling, tickless or HAL operations. */
void vTaskStartScheduler(void) { ++s_ctx.unrelated_calls; }
eSleepModeStatus eTaskConfirmSleepModeStatus(void) { ++s_ctx.unrelated_calls; return eAbortSleep; }
void vTaskStepTick(TickType_t ticks) { (void)ticks; ++s_ctx.unrelated_calls; }
BaseType_t xTaskIncrementTick(void) { ++s_ctx.unrelated_calls; return pdFALSE; }
uint32_t os_fixture_tick_core_id(void) { ++s_ctx.unrelated_calls; return 0u; }
void os_fixture_tick_isr_lock(void) { ++s_ctx.unrelated_calls; }
void os_fixture_tick_isr_unlock(void) { ++s_ctx.unrelated_calls; }
void os_fixture_yield_from_isr(int should_yield) { (void)should_yield; ++s_ctx.unrelated_calls; }
uint32_t esy_hal_system_irq_save(void) { ++s_ctx.unrelated_calls; return 0u; }
void esy_hal_system_irq_restore(uint32_t status) { (void)status; ++s_ctx.unrelated_calls; }
esy_err_t esy_hal_system_get_tick_partial_us(uint32_t rate, uint32_t *partial) {
  (void)rate; (void)partial; ++s_ctx.unrelated_calls; return ESY_ERR_NOT_SUPPORTED;
}
esy_err_t esy_hal_system_suspend_tick(uint32_t rate, uint32_t *partial) {
  (void)rate; (void)partial; ++s_ctx.unrelated_calls; return ESY_ERR_NOT_SUPPORTED;
}
void esy_hal_system_resume_tick(void) { ++s_ctx.unrelated_calls; }
bool esy_hal_system_tick_is_pending(void) { ++s_ctx.unrelated_calls; return false; }
void esy_hal_system_reset(void) { ++s_ctx.unrelated_calls; }

void setUp(void) { memset(&s_ctx, 0, sizeof(s_ctx)); }
void tearDown(void) {}

/* One process-lifetime scenario respects the immutable binding without a
 * production-only reset seam or dependence on test execution order. */
void test_os_fatal_binding_and_dispatch_follow_one_boot_lifetime(void) {
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_os_set_fatal_handler(NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_os_set_fatal_handler(capture));
  if (setjmp(s_ctx.escape) == 0) esy_os_assert_failed(NULL, 0u);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.halt_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.capture_calls);

  esy_os_init();
  /* ESP-IDF starts the native scheduler before entering app_main(). */
  s_ctx.scheduler_state = taskSCHEDULER_RUNNING;
#ifndef ESY_PLATFORM_ESP32
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_os_set_fatal_handler(capture));
  s_ctx.scheduler_state = taskSCHEDULER_NOT_STARTED;
#endif
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_os_set_fatal_handler(capture));
#ifdef ESY_PLATFORM_ESP32
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.scheduler_queries);
#endif
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_os_set_fatal_handler(capture));
  esy_os_init();
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_os_set_fatal_handler(capture));
  uint32_t queries = s_ctx.scheduler_queries;

  /* Deliberately unreadable opaque pointers prove dispatch never dereferences. */
  const char *file = (const char *)(uintptr_t)0x12345678u;
  if (setjmp(s_ctx.escape) == 0) esy_os_assert_failed(file, UINT32_MAX);
  TEST_ASSERT_EQUAL(ESY_FAULT_REASON_ASSERT, s_ctx.reason);
  TEST_ASSERT_EQUAL_HEX32(0x12345678u, s_ctx.detail0);
  TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, s_ctx.detail1);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.capture_calls);
  TEST_ASSERT_EQUAL_UINT32(queries, s_ctx.dispatch_queries);

  TaskHandle_t task = (TaskHandle_t)(uintptr_t)0x87654321u;
  char *name = (char *)(uintptr_t)0xabcdef01u;
  if (setjmp(s_ctx.escape) == 0) vApplicationStackOverflowHook(task, name);
  TEST_ASSERT_EQUAL(ESY_FAULT_REASON_STACK_OVERFLOW, s_ctx.reason);
  TEST_ASSERT_EQUAL_HEX32(0x87654321u, s_ctx.detail0);
  TEST_ASSERT_EQUAL_HEX32(0xabcdef01u, s_ctx.detail1);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.capture_calls);
  TEST_ASSERT_EQUAL_UINT32(queries, s_ctx.scheduler_queries);

  s_ctx.return_from_handler = true;
  if (setjmp(s_ctx.escape) == 0) esy_os_assert_failed(NULL, 77u);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.capture_calls);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.halt_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.unrelated_calls);
}
