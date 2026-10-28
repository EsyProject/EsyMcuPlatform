/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_timer_capture.h
 * @brief Timer capture HAL backend interface (handle-based, no internal allocation).
 *
 * Design notes:
 * - The caller provides the handle storage; the HAL backend must not allocate from heap.
 * - Physical controller index and pin/timer mapping are provided via
 *   @ref esy_timer_capture_config_t.
 * - If a capability is not supported, the implementation should return
 *   @ref ESY_ERR_NOT_SUPPORTED (or an equivalent error code).
 */

#ifndef ESY_HB_TIMER_CAPTURE_H
#define ESY_HB_TIMER_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_timer_capture_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name HAL backend top-level API (handle-based, explicit extern) */
/** @{ */

/**
 * @brief Reserve a capture slot and its hardware identity before board setup.
 *
 * The input GPIO and, on STM32F4, the whole timer are exclusive among capture
 * handles. Populate all resource identities in config before calling. A board
 * configure hook must not change them. A suspended owner may claim its unchanged
 * identity again; an enabled or partially retired owner cannot be reinitialized.
 * Calls contend without waiting and return ESY_ERR_BUSY if reservation is busy.
 * On Pico/ESP32 claim, init, suspend and deinit must execute on the same core as
 * GPIO IRQ service registration. Never call lifecycle functions from an ISR.
 *
 * @param[in,out] cap Zero-initialized or suspended caller-owned handle.
 * @param[in] config Complete physical resource configuration.
 * @retval ESY_OK Identity reserved without changing board or hardware state.
 * @retval ESY_ERR_BUSY Slot, GPIO, or timer already owned, or claim contended.
 * @retval ESY_ERR_STATE Handle is enabled, retiring, or on another owner core.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration or pointer.
 */
esy_err_t esy_hb_timer_capture_claim(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *config);

/**
 * @brief Retire native capture/IRQ access while retaining the resource claim.
 *
 * Call before resetting board routing. Failure retains native state and the
 * claim for retry; callback context and handle storage must remain alive.
 * Successful suspend permits board reset and reinitialization by the same owner.
 * Queries for values/intervals return ESY_ERR_STATE while suspended.
 *
 * @param[in,out] cap Claimed caller-owned capture handle.
 * @retval ESY_OK IRQ/callback access retired; physical ownership is retained.
 * @retval ESY_ERR_INVALID_ARG cap is NULL.
 * @retval <0 Cleanup or context error; retain storage and retry.
 */
esy_err_t esy_hb_timer_capture_suspend(esy_timer_capture_t *cap);

/**
 * @brief Initialize a timer capture channel.
 *
 * Initialize the caller-owned handle with {0} before first use. Serialize
 * lifecycle operations on one owner task and keep callback context alive until
 * deinit has quiesced the capture IRQ. Callbacks execute in ISR context and
 * must be bounded, nonblocking, and must not reenter lifecycle operations.
 *
 * STM32F4 reserves the entire selected timer for one capture handle. The board
 * must supply its peripheral clock/GPIO setup through the HAL MSP hooks and
 * route the vector to the configured physical capture index. Do not share the
 * timer or its IRQ lifecycle with PWM, logical timers, or another driver.
 * The backend configures a 1 MHz counter using the current APB/TIMPRE clock
 * tree; a clock that cannot divide exactly to 1 MHz is rejected with
 * ESY_ERR_NOT_SUPPORTED. Keep that clock tree unchanged while capturing.
 * Init also reserves resources when called directly. Failed initialization
 * retains the claim/native state for explicit suspend/deinit cleanup. HAL users
 * receive the HAL wrapper cleanup behavior instead.
 *
 * @param[in,out] cap    Capture handle storage (provided by the caller)
 * @param[in]     config Configuration (includes phys_index / gpio / edge selection, etc.)
 * @retval ESY_OK Success
 * @retval ESY_ERR_STATE The handle is initialized or cleanup is incomplete.
 * @retval ESY_ERR_BUSY The selected slot, input GPIO, or timer is already owned.
 * @retval ESY_ERR_NOT_SUPPORTED The timer clock cannot provide a 1 MHz tick.
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_timer_capture_init(
    esy_timer_capture_t *cap,
    const esy_timer_capture_config_t *config);

/**
 * @brief Deinitialize a timer capture channel.
 *
 * Retires native IRQ/callback access before releasing the slot/GPIO/timer claim.
 * HAL calls suspend and resets board routing before this final release. Failure
 * retains ownership and context for retry. A zero handle returns ESY_OK.
 *
 * @param[in,out] cap Capture handle.
 * @retval ESY_OK Native resources and claim released; ctx is NULL.
 * @retval ESY_ERR_INVALID_ARG cap is NULL.
 * @retval <0 Cleanup/context error; retain handle and callback storage for retry.
 */
extern esy_err_t esy_hb_timer_capture_deinit(esy_timer_capture_t *cap);

/**
 * @brief Get the most recent captured timestamp (microseconds).
 *
 * Values belong to a wrapping backend capture-counter domain that may reset
 * on initialization/resume. STM32F4 uses 32 bits for TIM2/TIM5 and 16 bits for
 * the other supported timers. This query reads one published capture value;
 * separate value/interval queries do not form a coherent pair snapshot.
 *
 * @param[in]  cap   Capture handle
 * @param[out] value Output captured timestamp in microseconds
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_timer_capture_get_value(
    const esy_timer_capture_t *cap,
    uint32_t *value);

/**
 * @brief Get the interval between the most recent two capture events (microseconds).
 *
 * Every port returns zero with ESY_OK before the second capture. Each query
 * reads one complete ISR-published interval. STM32F4 computes the difference
 * modulo the timer's 16-bit or 32-bit counter width. Events must be separated
 * by less than one counter wrap; multiple wraps are not detected.
 *
 * @param[in]  cap      Capture handle
 * @param[out] interval Output interval in microseconds
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_timer_capture_get_interval(
    const esy_timer_capture_t *cap,
    uint32_t *interval);

/**
 * @brief Timer capture IRQ handler entry (called from the interrupt vector).
 *
 * Some platforms dispatch capture interrupts by timer instance ID, others by a
 * logical "physical index". If platform-specific mapping is required, perform it
 * in the IRQ integration code and then call this function with the mapped
 * @p index.
 *
 * @param[in] index Physical controller index
 */
extern void esy_hb_timer_capture_handle_irq(uint32_t index);

/**
 * @brief Query public TIMER_CAPTURE capabilities for an initialized handle.
 *
 * Uses @ref esy_timer_capture_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  cap      Capture handle
 * @param[out] out_caps Output capability bitmask
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_timer_capture_get_caps(
    const esy_timer_capture_t *cap,
    uint32_t *out_caps);

/**
 * @brief Query physical controller index (optional).
 *
 * If not supported, return @ref ESY_ERR_NOT_SUPPORTED.
 *
 * @param[in]  cap       Capture handle
 * @param[out] out_index Output physical controller index
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_timer_capture_get_phys_index(
    const esy_timer_capture_t *cap,
    uint8_t *out_index);

/** @} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_TIMER_CAPTURE_H */
