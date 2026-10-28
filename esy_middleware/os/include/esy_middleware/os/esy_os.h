/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_OS_H
#define ESY_OS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/os/esy_os_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_os.h
 * @brief OS abstraction layer - core services.
 *
 * This header provides a minimal OS abstraction layer entry points:
 * - scheduler lifecycle (init / start)
 * - ISR yield request helper
 * - scheduler state query and SysTick forwarding hook
 * - an optional product-owned transactional tickless-idle adapter
 *
 * Notes:
 * - The implementation is platform/RTOS specific (e.g., FreeRTOS).
 * - Unless explicitly stated, APIs are expected to be called from task context.
 */

/**
 * @brief Initialize the OS abstraction layer.
 *
 * Performs early initialization for the OS adaptation layer if needed.
 * On most platforms this can be a no-op.
 *
 * This function is idempotent and may be called multiple times safely.
 */
extern void esy_os_init(void);

/**
 * @brief Start the scheduler (does not return on success).
 *
 * Enters the task scheduling loop and is typically called as the last step in
 * main().
 *
 * @note If this function returns, it should be treated as platform-specific
 * behavior (e.g., failure to start the scheduler or a non-standard RTOS port).
 */
extern void esy_os_start_scheduler(void);

/**
 * @brief Request a task context switch from ISR context.
 *
 * This function should be called at the end of an ISR to request a context
 * switch if a higher priority task has been woken.
 *
 * @param[in] should_yield
 *  - true: request a context switch on ISR exit.
 *  - false: do not request a context switch.
 */
extern void esy_os_yield_from_isr(bool should_yield);

/**
 * @brief Atomic tickless-entry result supplied to the OS adapter.
 */
typedef struct esy_os_tickless_result_s {
  bool committed;
  bool entered;
  bool elapsed_valid;
  bool fatal;
  uint64_t elapsed_us;
  esy_err_t fatal_error;
} esy_os_tickless_result_t;

/**
 * @brief Product-owned transaction callbacks for tickless idle.
 *
 * Prepare, complete, and abort execute in idle-task context with interrupts
 * enabled. prepare receives the remaining relative window after the OS has
 * subtracted elapsed time in the current kernel tick; it must use that value as
 * a latest-resume bound. Atomic entry and recover_atomic execute with global
 * interrupts disabled. recover_atomic restores only the IRQ-safe subset of the
 * backend and device graph while keeping source IRQs locally masked. It must be
 * bounded, non-blocking, allocation-free, logging-free, and independent of
 * RTOS time. After the OS tick and global interrupts are restored, complete or
 * abort performs late recovery and releases source IRQs last. The FreeRTOS
 * scheduler remains suspended by the idle path until this port hook returns,
 * so late callbacks must not wait on RTOS objects, delays, or task scheduling.
 *
 * Settlement includes the stopped-tick interval through early recovery even
 * when entry is canceled or exceeds the requested window. It steps strictly
 * before the next task timeout, then publishes the remaining elapsed ticks to
 * the native pending path for expiry processing when the idle owner resumes
 * scheduling. One hardware-pending tick remains owned by its ISR; fractional
 * tick time is carried between transactions. Native tick hooks may therefore
 * run during replay in IRQ-masked idle context and must remain bounded and
 * obey their non-blocking ISR restrictions.
 *
 * Replay is limited to 1024 synthetic ticks per transaction. Unrepresentable
 * elapsed time, excess replay, or clock failure causes masked fail-stop/reset
 * before partial settlement or tick/IRQ restoration; time is never truncated.
 * Provider recovery and native tick-hook costs must fit the product's IRQ and
 * watchdog budgets, including one native increment per replayed tick.
 */
typedef struct esy_os_tickless_ops_s {
  esy_err_t (*prepare)(void *ctx, uint64_t maximum_sleep_us);
  esy_err_t (*enter_atomic)(void *ctx, esy_os_tickless_result_t *out_result);
  esy_err_t (*complete)(void *ctx);
  esy_err_t (*abort)(void *ctx);
  esy_err_t (*recover_atomic)(void *ctx);
  /** @brief After a failed prepare, ESY_OK proves cleanup completed. Any error
   * requires masked fail-stop/reset. Called with interrupts enabled. */
  esy_err_t (*check_prepare_recovery)(void *ctx);
  /** @brief Read the shared monotonic microsecond clock with IRQs masked.
   * Must be bounded, non-blocking and valid across entry and early recovery.
   * The OS samples at tick suspension and after recover_atomic; entry's
   * elapsed_us is diagnostic only and is not used for kernel settlement. */
  esy_err_t (*read_clock_atomic)(void *ctx, uint64_t *out_time_us);
} esy_os_tickless_ops_t;

/**
 * @brief Install the single product tickless transaction adapter.
 *
 * Call after @ref esy_os_init and before the scheduler starts. A second
 * registration cannot replace the installed operations. Without registered
 * operations, tickless idle fails closed.
 * Native sleep admission must reject already-pended kernel ticks. SMP support
 * additionally requires global scheduler suspension retaining the native TASK
 * lock, a declared tick-owner core and native IRQ-preserving ISR lock macros.
 * Only that core enters the adapter. ESP-IDF SMP and unverified SMP providers
 * are rejected rather than borrowing single-core settlement assumptions.
 *
 * @return ESY_OK on registration, ESY_ERR_INVALID_ARG for invalid operations or
 * uninitialized OS, ESY_ERR_STATE if occupied, or ESY_ERR_NOT_SUPPORTED when the
 * native SMP settlement contract is unavailable.
 */
extern esy_err_t esy_os_set_tickless_ops(const esy_os_tickless_ops_t *ops,
                                         void *ctx);

/**
 * @brief Clear the currently installed tickless transaction adapter.
 *
 * The operations and context must exactly match the registration. Call
 * before the scheduler starts or after it has stopped.
 */
extern esy_err_t esy_os_clear_tickless_ops(const esy_os_tickless_ops_t *ops,
                                           void *ctx);

/**
 * @brief Query whether the scheduler has started.
 *
 * @retval true  Scheduler is running.
 * @retval false Scheduler has not started.
 *
 * @note The platform SysTick owner may call this bounded query from ISR context
 * to decide whether to forward to the kernel tick handler. A suspended
 * scheduler still counts as started. Typical FreeRTOS implementation:
 * xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED.
 */
extern bool esy_os_started(void);

/**
 * @brief Get the number of CPU cores managed by the OS scheduler.
 *
 * This may be smaller than the number of physical platform cores when the OS is
 * built in single-core mode on a multi-core MCU.
 *
 * @return Number of scheduler-managed cores. Returns at least 1.
 */
extern uint8_t esy_os_core_count(void);

/**
 * @brief OS SysTick hook entry (ISR context).
 *
 * The unique platform SysTick owner first forwards the hardware timebase using
 * esy_hal_system_tick_from_isr(). It calls this function exactly once only when
 * @ref esy_os_started returns true, to advance the RTOS timebase. Calling before
 * scheduler startup is invalid. The STM32F4 FreeRTOS port owns this sequence.
 *
 * @note STM32F4 calls the native xPortSysTickHandler() function; no same-named
 * macro is required (a self-mapping macro is allowed). FreeRTOSConfig must not
 * alias xPortSysTickHandler to SysTick_Handler: the platform owns that wrapper
 * and needs a distinct native handler symbol. This hook is a no-op on ports
 * with their own native tick owner (Pico and ESP32), where the consumer must not
 * add another tick wrapper.
 */
extern void esy_os_systick(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_OS_H */
