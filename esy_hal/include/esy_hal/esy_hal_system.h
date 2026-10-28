/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_system.h
 * @brief HAL system infrastructure API.
 *
 * Provides basic system services including:
 * - system initialization,
 * - system clock query,
 * - blocking delays (millisecond / microsecond),
 * - time base counters,
 * - core and execution-context queries.
 *
 * Conventions:
 * - Functions returning @ref esy_err_t:
 *   - ESY_OK (0) indicates success;
 *   - negative values indicate failure (see esy_error_types.h).
 * - @ref esy_hal_system_get_tick() typically returns a
 * millisecond-resolution system tick.
 * - @ref esy_hal_system_time_32_us() returns a 32-bit microsecond counter
 *   which wraps naturally on overflow.
 */

#ifndef ESY_HAL_SYSTEM_H
#define ESY_HAL_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifndef ESY_HAL_RAM_FUNC
#if defined(__GNUC__)
#define ESY_HAL_RAM_FUNC \
  __attribute__((noinline, section(".time_critical.esy_hal")))
#else
#define ESY_HAL_RAM_FUNC
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize basic system infrastructure.
 *
 * Initializes system clocks and essential low-level facilities
 * (e.g. stdio or platform-specific runtime support).
 *
 * This function is typically called early during system startup.
 * STM32F4 enables and verifies the shared DWT counter for microsecond delays;
 * an unavailable counter returns ESY_ERR_NOT_SUPPORTED. It never resets CYCCNT.
 *
 * @return
 *  - ESY_OK on success
 *  - negative error code on failure
 */
extern esy_err_t esy_hal_system_init(void);

/**
 * @brief Fill caller-owned bytes from the port's SDK random source.
 *
 * Normal startup/task context after successful system/SDK initialization only;
 * never call from an ISR, terminal fault or Flash-unavailable capture path.
 * One owner serializes this synchronous operation with source configuration
 * and low-power changes. Work and possible SDK waiting scale with length;
 * the first Pico call also seeds the SDK generator. No caller storage is
 * retained and no HAL-owned heap, task or Flash write is introduced.
 *
 * Pico uses pico_rand with the consumer's SDK entropy configuration. ESP32
 * uses esp_fill_random; entropy quality depends on native RF/entropy-source
 * state. This API promises neither cryptographic suitability nor collision-free
 * identifiers. The owner must assess entropy for its use. STM32F4 currently
 * returns NOT_SUPPORTED rather than synthesizing bytes from a tick or UID.
 * Zero length always succeeds without invoking the SDK, including NULL buffer.
 * Nonempty output is authoritative only on OK; no partial-byte count is given.
 *
 * @param[out] buffer Writable bytes, non-NULL when length is nonzero.
 * @param[in] length Number of requested bytes; caller budgets synchronous cost.
 * @return OK; INVALID_ARG for NULL with nonzero length; NOT_SUPPORTED when no
 * supported random source exists; another negative provider error.
 */
esy_err_t esy_hal_system_random_bytes(uint8_t *buffer, size_t length);

/**
 * @brief Get the current system clock frequency.
 *
 * @return System clock frequency in Hertz (Hz).
 */
extern uint32_t esy_hal_system_clk_sys_get_hz(void);

/**
 * @brief Blocking delay for the specified number of milliseconds.
 *
 * @param[in] ms Delay duration in milliseconds.
 */
extern void esy_hal_system_delay_ms(uint32_t ms);

/**
 * @brief Blocking delay for the specified number of microseconds.
 *
 * Call after successful system initialization. Zero returns immediately.
 * STM32F4 busy-waits on DWT CYCCNT without changing the interrupt mask or
 * allocating a peripheral timer. Interrupts and scheduling may extend the wait;
 * use it only for short synchronous hardware delays, not exact pulse timing.
 * The STM32F4 clock owner must keep SystemCoreClock current and HCLK fixed for
 * the call. Do not reset or disable the shared DWT counter while initialized,
 * or enter sleep during the call. Task/startup context is the portable contract.
 * STM32F4 also supports short bounded ISR/masked calls after initialization.
 * Its delay support is independent of optional microsecond timestamp settings.
 *
 * @param[in] us Delay duration in microseconds.
 */
extern void esy_hal_system_delay_us(uint32_t us);

/**
 * @brief Get the millisecond tick count since power-up.
 *
 * The counter monotonically increases and wraps around on overflow.
 *
 * @return Millisecond tick count.
 */
extern uint32_t esy_hal_system_get_tick(void);

/**
 * @brief Forward one system timebase interrupt to the hardware backend.
 *
 * The unique timebase ISR owner calls this exactly once per configured tick,
 * including before the scheduler starts. On STM32F4 this advances the vendor
 * HAL tick; ports with an independently advancing timebase perform no work.
 * This bounded, nonblocking ISR-only operation does not advance the OS tick,
 * yield, allocate memory or retain caller resources. The owner must preserve
 * the backend's configured tick period and prevent duplicate forwarding.
 */
extern void esy_hal_system_tick_from_isr(void);

/**
 * @brief Get a 32-bit microsecond counter since power-up.
 *
 * The counter wraps naturally on overflow. Its unit does not guarantee its
 * resolution. STM32F4 always returns its millisecond HAL tick multiplied by 1000.
 * ESP32 uses its native microsecond counter only when enable_us_tick is enabled;
 * otherwise it also returns the millisecond tick multiplied by 1000. Pico uses
 * its native microsecond counter. Select a platform-appropriate timer/capture
 * API when finer resolution is required.
 *
 * When computing time differences, unsigned subtraction should be used
 * to handle wrap-around correctly:
 * @code
 * delta_us = (now - prev);
 * @endcode
 *
 * @return 32-bit microsecond counter value.
 */
extern uint32_t esy_hal_system_time_32_us(void);

/**
 * @brief Read the platform 64-bit always-on microsecond timebase.
 *
 * The returned source must continue through every low-power state for which
 * it is used to account elapsed time. Unsupported ports fail closed.
 *
 * @param[out] out_time_us Current timestamp.
 * @retval ESY_OK Timestamp written.
 * @retval ESY_ERR_INVALID_ARG Output pointer is NULL.
 * @retval ESY_ERR_NOT_SUPPORTED No compliant source is available.
 */
extern esy_err_t esy_hal_system_always_on_time_us(uint64_t *out_time_us);

/** @brief Return power states through which the always-on clock is valid. */
extern uint32_t esy_hal_system_always_on_state_mask(void);

/**
 * @brief Read elapsed microseconds in the current kernel tick period.
 *
 * This bounded task-context query leaves the tick source running. The OS uses
 * it immediately before tickless preparation to exclude elapsed current-period
 * time from the latest wake deadline.
 *
 * @param[in] tick_rate_hz Configured kernel tick rate.
 * @param[out] out_partial_tick_us Elapsed current-period microseconds.
 * @retval ESY_OK Current fraction written.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments or the tick is suspended.
 * @retval ESY_ERR_NOT_SUPPORTED The port uses another tickless mechanism.
 * @retval ESY_ERR_STATE The running tick configuration is inconsistent.
 */
extern esy_err_t esy_hal_system_get_tick_partial_us(
    uint32_t tick_rate_hz, uint32_t *out_partial_tick_us);

/**
 * @brief Stop the kernel tick and report elapsed time in its current period.
 *
 * Resume restarts a full tick period, so the OS adapter must include the
 * returned partial interval in elapsed-time settlement.
 */
extern esy_err_t esy_hal_system_suspend_tick(
    uint32_t tick_rate_hz, uint32_t *out_partial_tick_us);

/** @brief Restart the platform kernel tick source after tickless entry. */
extern void esy_hal_system_resume_tick(void);

/** @brief Query a pending kernel tick without clearing it. */
extern bool esy_hal_system_tick_is_pending(void);

/**
 * @brief Get the ID of the CPU core currently executing.
 *
 * @return Zero-based core ID of the current execution context.
 */
extern uint8_t esy_hal_system_get_core_id(void);

/**
 * @brief Get the total number of available CPU cores.
 *
 * @return Number of CPU cores (>= 1).
 */
extern uint8_t esy_hal_system_get_core_count(void);

/**
 * @brief Check whether the current context is an interrupt service routine
 * (ISR).
 *
 * @retval true  The current code is executing in ISR context.
 * @retval false The current code is executing in task/thread context.
 */
extern bool esy_hal_system_is_in_isr(void);

/**
 * @brief Disable interrupts and return a platform-defined restore token.
 *
 * The returned token must be passed to
 * @ref esy_hal_system_irq_restore on the same core, in reverse save order.
 * Nested regions preserve the entry interrupt mask, including an already
 * masked entry. On ESP32 use SDK-supported task/ISR contexts; this uses the
 * SDK's local interrupt-level mask, not a cross-core lock. Regions must be short
 * and nonblocking.
 *
 * @return Platform-defined interrupt restore token.
 */
extern uint32_t esy_hal_system_irq_save(void);

/**
 * @brief Restore the interrupt state saved by @ref
 * esy_hal_system_irq_save.
 *
 * @param[in] saved_status Restore token returned by
 *                         @ref esy_hal_system_irq_save.
 */
extern void esy_hal_system_irq_restore(uint32_t saved_status);

/**
 * @brief Issue a platform memory barrier for shared low-level state.
 */
extern void esy_hal_system_memory_barrier(void);

/**
 * @brief Signal a low-level platform event.
 */
extern void esy_hal_system_signal_event(void);

/**
 * @brief Wait for a low-level platform event.
 */
extern void esy_hal_system_wait_for_event(void);

/**
 * @brief Read the current execution address for image-region identification.
 * @return Platform-specific program counter value.
 * @note Synchronous; no state is retained. Intended for boot/diagnostic code.
 */
uintptr_t esy_hal_system_get_pc(void);

/** @brief Reset the MCU. This function does not return. */
extern void esy_hal_system_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_SYSTEM_H */
