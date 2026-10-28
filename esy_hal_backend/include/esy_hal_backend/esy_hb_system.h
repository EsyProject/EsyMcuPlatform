/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_system.h
 * @brief Low-level system initialization and core infrastructure interface.
 *
 * This header provides fundamental system services, including:
 * - System clock and stdio initialization
 * - Time base access (milliseconds / microseconds) and blocking delays
 * - Global interrupt enable/disable
 * - System tick suspend/resume
 * - Application jump / handover
 * - Core and execution-context queries
 *
 * Conventions:
 * - Return type is @ref esy_err_t: 0 indicates success, negative values
 * indicate failure.
 * - @ref esy_hb_system_get_tick usually returns a millisecond system tick;
 *   its behavior may be affected by tick suspend/resume depending on
 * implementation.
 * - @ref esy_hb_system_time_32_us returns a 32-bit microsecond counter that
 * wraps naturally; always compute time intervals using differences.
 */

#ifndef ESY_HB_SYSTEM_H
#define ESY_HB_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_system_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize system clock and basic peripherals.
 *
 * Configures the system clock according to @p config and initializes
 * standard I/O backends (e.g. UART or USB logging).
 * STM32F4 also enables and verifies shared DWT CYCCNT for blocking delays,
 * without resetting its value. An unavailable counter returns
 * ESY_ERR_NOT_SUPPORTED; a failed vendor HAL initialization returns ESY_ERR_IO.
 *
 * @param[in] config Pointer to system configuration structure
 * @retval ESY_OK Success
 * @retval <0     Error code (invalid arguments, clock configuration failure,
 * etc.)
 */
extern esy_err_t esy_hb_system_init(const esy_system_config_t *config);

/**
 * @brief Copy SDK random bytes in serialized normal startup/task context.
 *
 * Requires initialized SDK/system and stable provider/low-power configuration.
 * Not ISR/fault safe. Synchronous O(length) work may wait for SDK entropy or
 * first-use seeding; no heap, task or caller resource ownership is added.
 * Pico uses pico_rand and explicitly copies little-endian generated words;
 * ESP32 uses esp_fill_random with its native entropy-source limitations.
 * Neither cryptographic strength nor collision-free identity is promised.
 * STM32F4 fails closed until a supported random source is provided.
 * Output is authoritative only on OK; zero length performs no SDK operation.
 *
 * @param[out] buffer Writable output, NULL permitted only at zero length.
 * @param[in] length Requested bytes; caller owns the latency budget.
 * @return OK; INVALID_ARG for NULL/nonzero; NOT_SUPPORTED without a source;
 * another negative SDK/provider failure.
 */
esy_err_t esy_hb_system_random_bytes(uint8_t *buffer, size_t length);

/**
 * @brief Get the current system clock frequency.
 *
 * @return System clock frequency in Hertz (Hz)
 */
extern uint32_t esy_hb_system_clk_sys_get_hz(void);

/**
 * @brief Blocking delay for a specified number of milliseconds.
 *
 * @param[in] ms Delay duration in milliseconds
 */
extern void esy_hb_system_delay_ms(uint32_t ms);

/**
 * @brief Blocking delay for a specified number of microseconds.
 *
 * Requires successful system initialization. Zero returns immediately. STM32F4
 * uses the shared DWT counter without resetting it or masking interrupts; it
 * reserves no TIM peripheral and works with SysTick suspended or IRQs masked.
 * Keep SystemCoreClock current, HCLK fixed and DWT enabled throughout the call.
 * Startup/task callers and short bounded STM32F4 ISR callers are supported;
 * preemption extends the delay. Use only for short waits, not precise edges or
 * low-power elapsed-time accounting. Delay support does not depend on
 * config.enable_us_tick. STM32F4 initialization fails if CYCCNT cannot advance.
 *
 * @param[in] us Delay duration in microseconds
 */
extern void esy_hb_system_delay_us(uint32_t us);

/**
 * @brief Get the system tick count since power-up.
 *
 * The counter increments monotonically and wraps around on overflow
 * (32-bit arithmetic).
 *
 * @return Millisecond system tick count
 */
extern uint32_t esy_hb_system_get_tick(void);

/**
 * @brief Advance the backend timebase from its unique interrupt owner.
 *
 * Call exactly once per configured system timebase interrupt, including before
 * scheduler startup. STM32F4 advances its vendor HAL tick; Pico and ESP32 use
 * independently advancing clocks and perform no work. ISR-only, bounded and
 * nonblocking; this function never advances the OS tick or requests a yield.
 * The interrupt owner must preserve the backend's configured tick period and
 * prevent concurrent or duplicate forwarding. No caller resources are retained.
 */
extern void esy_hb_system_tick_from_isr(void);

/**
 * @brief Get a 32-bit microsecond time counter.
 *
 * The counter automatically wraps around at 32-bit overflow and is
 * intended for high-resolution interval measurements.
 *
 * @return 32-bit microsecond counter value
 */
extern uint32_t esy_hb_system_time_32_us(void);

/**
 * @brief Read a 64-bit microsecond counter that continues in supported sleep.
 *
 * The counter is monotonic across every power state advertised by the active
 * low-power port. A port without an always-on hardware counter must return
 * @ref ESY_ERR_NOT_SUPPORTED instead of synthesizing time from a counter that
 * stops or wraps without software observation.
 *
 * @param[out] out_time_us Current always-on timestamp in microseconds.
 * @retval ESY_OK Timestamp written.
 * @retval ESY_ERR_INVALID_ARG Output pointer is NULL.
 * @retval ESY_ERR_NOT_SUPPORTED No compliant counter is available.
 */
extern esy_err_t esy_hb_system_always_on_time_us(uint64_t *out_time_us);

/** @brief Return power states through which the always-on clock is valid. */
extern uint32_t esy_hb_system_always_on_state_mask(void);

/**
 * @brief Read elapsed microseconds in the current kernel tick period.
 *
 * This bounded query does not stop or reprogram the tick source. It is called
 * with interrupts enabled immediately before tickless preparation so the OS
 * can exclude the already elapsed fraction from its sleep deadline.
 *
 * @param[in] tick_rate_hz Configured kernel tick rate.
 * @param[out] out_partial_tick_us Elapsed current-period microseconds.
 * @retval ESY_OK Current fraction written.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments or the tick is suspended.
 * @retval ESY_ERR_NOT_SUPPORTED The port uses another tickless mechanism.
 * @retval ESY_ERR_STATE The running tick configuration is inconsistent.
 */
extern esy_err_t esy_hb_system_get_tick_partial_us(
    uint32_t tick_rate_hz, uint32_t *out_partial_tick_us);

/**
 * @brief Stop the system tick and capture the elapsed current-period fraction.
 *
 * Resume starts a full tick period. The OS adapter accounts the returned
 * partial interval together with sleep elapsed time.
 *
 * @param[in] tick_rate_hz Configured kernel tick rate.
 * @param[out] out_partial_tick_us Elapsed microseconds in the stopped period.
 * @retval ESY_OK Tick stopped and partial interval written.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments or unbalanced nesting.
 * @retval ESY_ERR_NOT_SUPPORTED The port uses another tickless mechanism.
 */
extern esy_err_t esy_hb_system_suspend_tick(uint32_t tick_rate_hz,
                                                uint32_t *out_partial_tick_us);

/**
 * @brief Resume the system tick.
 */
extern void esy_hb_system_resume_tick(void);

/**
 * @brief Report whether a kernel tick interrupt is pending.
 *
 * This bounded query is used by the OS tickless adapter while interrupts are
 * masked. It must not clear the pending condition.
 */
extern bool esy_hb_system_tick_is_pending(void);

/**
 * @brief Disable global interrupts.
 *
 * Typically used to enter a critical section. The duration of disabled
 * interrupts should be kept as short as possible.
 */
extern void esy_hb_system_disable_irq(void);

/**
 * @brief Enable global interrupts.
 *
 * Restores interrupt handling after leaving a critical section.
 */
extern void esy_hb_system_enable_irq(void);

/**
 * @brief Disable interrupts and return a platform-defined restore token.
 *
 * The returned token must be passed back to
 * @ref esy_hb_system_irq_restore. This API is intended for short,
 * non-blocking critical regions that need to preserve the previous interrupt
 * enable state.
 *
 * Tokens belong to the saving core and must be restored in reverse save order.
 * ESP32 preserves the SDK interrupt level in SDK-supported task/ISR contexts
 * rather than unconditionally enabling interrupts; this does not lock resources
 * accessed from another core.
 *
 * @return Platform-defined interrupt restore token.
 */
extern uint32_t esy_hb_system_irq_save(void);

/**
 * @brief Restore the interrupt state saved by @ref esy_hb_system_irq_save.
 *
 * @param[in] saved_status Restore token returned by
 *                         @ref esy_hb_system_irq_save.
 */
extern void esy_hb_system_irq_restore(uint32_t saved_status);

/**
 * @brief Issue a platform memory barrier.
 *
 * Ensures memory ordering for shared state observed across cores, interrupt
 * contexts, or low-level wait loops.
 */
extern void esy_hb_system_memory_barrier(void);

/**
 * @brief Signal a low-level platform event.
 *
 * This wakes peers blocked in @ref esy_hb_system_wait_for_event on
 * platforms that support event wait/signal primitives. Unsupported platforms
 * may implement this as a no-op.
 */
extern void esy_hb_system_signal_event(void);

/**
 * @brief Wait for a low-level platform event.
 *
 * This is intended for short hardware-level wait loops. Unsupported platforms
 * may implement this as a no-op.
 */
extern void esy_hb_system_wait_for_event(void);

/** @brief Reset the MCU. This function does not return. */
extern void esy_hb_system_reset(void);

/**
 * @brief Get the current program counter (PC) value.
 *
 * Intended for debugging or diagnostic purposes only.
 * Availability depends on platform and compiler support.
 *
 * @return Current PC register value
 */
extern uintptr_t esy_hb_system_get_pc(void);

/**
 * @brief Get the ID of the current CPU core.
 *
 * @return Current core ID
 */
extern uint8_t esy_hb_system_get_core_id(void);

/**
 * @brief Get the total number of available CPU cores.
 *
 * @return Number of CPU cores
 */
extern uint8_t esy_hb_system_get_core_count(void);

/**
 * @brief Check whether the current context is an interrupt service routine
 * (ISR).
 *
 * @retval true  Currently executing in ISR context
 * @retval false Not executing in ISR context
 */
extern bool esy_hb_system_is_in_isr(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_SYSTEM_H */
