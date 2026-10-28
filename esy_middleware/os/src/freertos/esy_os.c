/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/os/esy_os_fatal.h"

#include <stdatomic.h>

#ifdef ESY_PLATFORM_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portable.h"
#else
#include "FreeRTOS.h"
#include "task.h"
#include "portable.h"
#endif
#include "esy_hal/esy_hal_system.h"

#ifdef ESY_PLATFORM_STM32F4
/* The Cortex-M port exports a function, which need not be a macro. */
extern void xPortSysTickHandler(void);
#endif

/* Limit IRQ-masked tick replay independently of the requested sleep duration. */
#define ESY_OS_TICKLESS_MAX_PENDING_TICKS 1024u

typedef struct {
  const esy_os_tickless_ops_t *tickless_ops;
  void *tickless_idle_ctx;
  uint32_t tickless_fraction_scaled;
  bool initialized;
  _Atomic(esy_os_fatal_handler_t) fatal_handler;
} esy_os_ctx_t;

static esy_os_ctx_t s_ctx;

void esy_os_init(void) {
  if (s_ctx.initialized) {
    return;
  }
  s_ctx.tickless_ops = NULL;
  s_ctx.tickless_idle_ctx = NULL;
  s_ctx.tickless_fraction_scaled = 0u;
  s_ctx.initialized = true;
}

void esy_os_start_scheduler(void) { vTaskStartScheduler(); }

esy_err_t esy_os_set_fatal_handler(esy_os_fatal_handler_t handler) {
  if (handler == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.initialized ||
      atomic_load_explicit(&s_ctx.fatal_handler, memory_order_relaxed) != NULL) {
    return ESY_ERR_STATE;
  }
#ifndef ESY_PLATFORM_ESP32
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return ESY_ERR_STATE;
#endif
  /* One serialized startup owner publishes once. IDF already runs its native
   * scheduler at app entry, so peer fatal readers require atomic publication;
   * no compare/exchange, kernel lock or runtime rebind is needed. */
  atomic_store_explicit(&s_ctx.fatal_handler, handler, memory_order_release);
  return ESY_OK;
}

#if defined(__GNUC__)
__attribute__((noreturn)) ESY_FAULT_RAM_FUNC
#endif
static void fatal_dispatch(esy_fault_reason_t reason, uint32_t detail0,
                            uint32_t detail1) {
  esy_os_fatal_handler_t handler =
      atomic_load_explicit(&s_ctx.fatal_handler, memory_order_acquire);
  if (handler != NULL) handler(reason, detail0, detail1);
#if defined(__arm__)
  __asm volatile("cpsid i" ::: "memory");
  for (;;) __asm volatile("wfe" ::: "memory");
#else
  taskDISABLE_INTERRUPTS();
  for (;;) {}
#endif
}

void esy_os_assert_failed(const char *file, uint32_t line) {
  fatal_dispatch(ESY_FAULT_REASON_ASSERT, (uint32_t)(uintptr_t)file, line);
}

void esy_os_yield_from_isr(bool should_yield) {
  if (should_yield) {
#ifdef ESY_PLATFORM_ESP32
    portYIELD_FROM_ISR();
#else
    portYIELD_FROM_ISR(pdTRUE);
#endif
  }
}

esy_err_t esy_os_set_tickless_ops(const esy_os_tickless_ops_t *ops, void *ctx) {
  if ((ops == NULL) || (ops->prepare == NULL) || (ops->enter_atomic == NULL) ||
      (ops->recover_atomic == NULL) || (ops->complete == NULL) ||
      (ops->abort == NULL) || (ops->check_prepare_recovery == NULL) ||
      (ops->read_clock_atomic == NULL) || !s_ctx.initialized) {
    return ESY_ERR_INVALID_ARG;
  }
  if (s_ctx.tickless_ops != NULL) {
    return ESY_ERR_STATE;
  }
#if configNUMBER_OF_CORES > 1
#if defined(ESY_PLATFORM_ESP32) || !defined(configTICK_CORE) || \
    !defined(portGET_CORE_ID) || !defined(portGET_ISR_LOCK) || \
    !defined(portRELEASE_ISR_LOCK)
  /* The IDF per-core scheduler and unverified SMP ports need their own bridge. */
  return ESY_ERR_NOT_SUPPORTED;
#endif
#endif
  s_ctx.tickless_idle_ctx = ctx;
  s_ctx.tickless_ops = ops;
  s_ctx.tickless_fraction_scaled = 0u;
  return ESY_OK;
}

esy_err_t esy_os_clear_tickless_ops(const esy_os_tickless_ops_t *ops,
                                    void *ctx) {
  if ((ops == NULL) || (s_ctx.tickless_ops != ops) ||
      (s_ctx.tickless_idle_ctx != ctx)) {
    return ESY_ERR_INVALID_ARG;
  }
  s_ctx.tickless_ops = NULL;
  s_ctx.tickless_idle_ctx = NULL;
  s_ctx.tickless_fraction_scaled = 0u;
  return ESY_OK;
}

#if defined(configUSE_TICKLESS_IDLE) && (configUSE_TICKLESS_IDLE == 1)
static uint64_t ticks_to_us(TickType_t ticks) {
  const uint64_t seconds = (uint64_t)ticks / configTICK_RATE_HZ;
  const uint64_t remainder =
      (((uint64_t)ticks % configTICK_RATE_HZ) * 1000000u) /
      configTICK_RATE_HZ;
  if (seconds > (UINT64_MAX - remainder) / 1000000u) return UINT64_MAX;
  return (seconds * 1000000u) + remainder;
}

static bool settle_elapsed_ticks(uint64_t elapsed_us,
                                  TickType_t expected_idle_ticks,
                                  bool tick_pending) {
  uint64_t seconds = elapsed_us / 1000000u;
  uint64_t remainder_us = elapsed_us % 1000000u;
  uint64_t scaled_remainder =
      (remainder_us * configTICK_RATE_HZ) + s_ctx.tickless_fraction_scaled;
  const uint64_t fractional_ticks = scaled_remainder / 1000000u;
  if (seconds > (UINT64_MAX - fractional_ticks) / configTICK_RATE_HZ)
    return false;
  uint64_t elapsed_ticks = (seconds * configTICK_RATE_HZ) + fractional_ticks;
  if (tick_pending && (elapsed_ticks > 0u)) {
    elapsed_ticks--;
  }

  /* Step strictly before the earliest unblock, then replay every remaining
   * tick through the native pending path. The idle task still owns scheduler
   * suspension: xTaskResumeAll() will process those ticks and unblock tasks.
   * A hardware-pending tick is left to its ISR exactly once. */
  const uint64_t safe_step = (uint64_t)expected_idle_ticks - 1u;
  const uint64_t step_ticks = elapsed_ticks < safe_step ? elapsed_ticks : safe_step;
  const uint64_t pending_ticks = elapsed_ticks - step_ticks;
  if (pending_ticks > ESY_OS_TICKLESS_MAX_PENDING_TICKS ||
      pending_ticks >= (uint64_t)portMAX_DELAY) return false;

  s_ctx.tickless_fraction_scaled = (uint32_t)(scaled_remainder % 1000000u);
  if (step_ticks != 0u) vTaskStepTick((TickType_t)step_ticks);
  for (uint64_t tick = 0u; tick < pending_ticks; ++tick) {
#if configNUMBER_OF_CORES > 1 && !defined(ESY_PLATFORM_ESP32) && \
    defined(portGET_ISR_LOCK) && defined(portRELEASE_ISR_LOCK)
    /* The native SMP idle suspension owns TASK lock. Only the short tick
     * publication also needs ISR lock, in the native TASK -> ISR order.
     * HAL already masks this core's IRQs; these locks do not restore them. */
    portGET_ISR_LOCK();
    (void)xTaskIncrementTick();
    portRELEASE_ISR_LOCK();
#else
    (void)xTaskIncrementTick();
#endif
  }
  return true;
}

static bool recover_tickless_transaction_masked(void) {
  if (s_ctx.tickless_ops->recover_atomic(s_ctx.tickless_idle_ctx) != ESY_OK) {
    esy_hal_system_reset();
    return false;
  }
  return true;
}

static bool abort_tickless_transaction(void) {
  if (s_ctx.tickless_ops->abort(s_ctx.tickless_idle_ctx) != ESY_OK) {
    esy_hal_system_reset();
    return false;
  }
  return true;
}

void vPortSuppressTicksAndSleep(TickType_t expected_idle_time) {
  esy_os_tickless_result_t result = {0};
  uint64_t maximum_sleep_us;
  uint64_t elapsed_us;
  uint64_t stopped_at_us;
  uint64_t recovered_at_us;
  uint32_t prepare_partial_tick_us = 0u;
  uint32_t partial_tick_us = 0u;
  uint32_t irq_status;
  bool tick_pending;
  esy_err_t ret;

  if (s_ctx.tickless_ops == NULL) return;
#if configNUMBER_OF_CORES > 1 && defined(configTICK_CORE) && \
    defined(portGET_CORE_ID)
  if (portGET_CORE_ID() != configTICK_CORE) return;
#endif
  maximum_sleep_us = ticks_to_us(expected_idle_time);
  if (maximum_sleep_us == 0u) return;
  ret = esy_hal_system_get_tick_partial_us(configTICK_RATE_HZ,
                                                &prepare_partial_tick_us);
  if ((ret != ESY_OK) ||
      ((uint64_t)prepare_partial_tick_us >= maximum_sleep_us)) {
    return;
  }
  maximum_sleep_us -= prepare_partial_tick_us;
  ret = s_ctx.tickless_ops->prepare(s_ctx.tickless_idle_ctx, maximum_sleep_us);
  if (ret != ESY_OK) {
    if (s_ctx.tickless_ops->check_prepare_recovery(s_ctx.tickless_idle_ctx) !=
        ESY_OK) {
      (void)esy_hal_system_irq_save();
      esy_hal_system_reset();
    }
    return;
  }

  irq_status = esy_hal_system_irq_save();
  if (eTaskConfirmSleepModeStatus() == eAbortSleep) {
    if (!recover_tickless_transaction_masked()) return;
    esy_hal_system_irq_restore(irq_status);
    (void)abort_tickless_transaction();
    return;
  }
  ret = esy_hal_system_suspend_tick(configTICK_RATE_HZ, &partial_tick_us);
  if (ret != ESY_OK) {
    if (!recover_tickless_transaction_masked()) return;
    esy_hal_system_irq_restore(irq_status);
    (void)abort_tickless_transaction();
    return;
  }
  if (s_ctx.tickless_ops->read_clock_atomic(s_ctx.tickless_idle_ctx,
                                           &stopped_at_us) != ESY_OK) {
    esy_hal_system_reset();
    return;
  }
  if ((uint64_t)partial_tick_us < maximum_sleep_us)
    (void)s_ctx.tickless_ops->enter_atomic(s_ctx.tickless_idle_ctx, &result);
  if (result.fatal) {
    esy_hal_system_reset();
    return;
  }
  if (!recover_tickless_transaction_masked()) return;
  if ((s_ctx.tickless_ops->read_clock_atomic(s_ctx.tickless_idle_ctx,
                                            &recovered_at_us) != ESY_OK) ||
      (recovered_at_us < stopped_at_us) ||
      ((recovered_at_us - stopped_at_us) > (UINT64_MAX - partial_tick_us))) {
    esy_hal_system_reset();
    return;
  }
  tick_pending = esy_hal_system_tick_is_pending();
  elapsed_us = partial_tick_us + (recovered_at_us - stopped_at_us);
  if (elapsed_us > 0u) {
    if (!settle_elapsed_ticks(elapsed_us, expected_idle_time, tick_pending)) {
      esy_hal_system_reset();
      return;
    }
  }
  esy_hal_system_resume_tick();
  esy_hal_system_irq_restore(irq_status);
  ret = result.committed ? s_ctx.tickless_ops->complete(s_ctx.tickless_idle_ctx)
                         : s_ctx.tickless_ops->abort(s_ctx.tickless_idle_ctx);
  if (ret != ESY_OK) esy_hal_system_reset();
}
#endif

#if defined(__GNUC__)
__attribute__((noreturn)) ESY_FAULT_RAM_FUNC
#endif
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
  fatal_dispatch(ESY_FAULT_REASON_STACK_OVERFLOW,
                 (uint32_t)(uintptr_t)xTask, (uint32_t)(uintptr_t)pcTaskName);
}

bool esy_os_started(void) {
  return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

uint8_t esy_os_core_count(void) { return (uint8_t)configNUMBER_OF_CORES; }

void esy_os_systick(void) {
#ifdef ESY_PLATFORM_STM32F4
  xPortSysTickHandler();
#endif
}
