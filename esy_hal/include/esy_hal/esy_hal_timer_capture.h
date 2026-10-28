/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_timer_capture.h
 * @brief HAL timer capture interface (handle-based).
 *
 * Design principles:
 * - The caller provides storage for the handle; the HAL assembles
 *   backend state and function pointers and dispatches operations.
 * - No controller index or channel number is exposed in the API; all operations
 *   are performed via @ref esy_timer_capture_t*.
 * - Functions return @ref esy_err_t; negative values indicate error conditions.
 *
 * Typical use cases:
 * - pulse width / period measurement,
 * - external signal timestamping,
 * - frequency or interval measurement.
 */

#ifndef ESY_HAL_TIMER_CAPTURE_H
#define ESY_HAL_TIMER_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_timer_capture_defs.h"

/**
 * @note Handle lifecycle: initialize caller-owned handle storage with {0} before
 * its first init. Serialize lifecycle and data operations on one owner context.
 * Repeated init, including init while registered, returns ESY_ERR_STATE without
 * changing the live handle. Stop asynchronous work before unregister/deinit.
 * Failed init cleans partial state when possible. Cleanup failure retains the
 * handle and its resource claims: retry deinit successfully before another init.
 * Lifecycle calls run outside ISR callbacks; on Pico/ESP32, use the same core
 * as GPIO IRQ service registration. The slot and input GPIO (also the whole
 * timer on STM32F4) remain reserved across power suspend until final deinit.
 * Registered handles cannot be deinitialized directly. Successful deinit returns
 * an unregistered, quiescent handle to the zero state, ready for another init.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @note This initializer replaces every field. Apply caller overrides afterward.
 * @brief Fill a timer capture configuration structure with default values.
 *
 * Writes platform-recommended safe defaults into @p cfg.
 * The caller may override selected fields before initialization.
 *
 * @param[out] cfg Configuration structure to initialize; must not be NULL.
 */
extern void esy_hal_timer_capture_default_cfg(
    esy_timer_capture_config_t *cfg);

/**
 * @brief Initialize a timer capture instance.
 *
 * Binds the capture peripheral according to the provided configuration
 * and prepares it for timestamp and interval measurement.
 *
 * @param[in,out] cap  Timer capture handle (storage provided by caller).
 * @param[in]     cfg  Initialization configuration.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_STATE The handle already owns resources.
 * @retval ESY_ERR_BUSY Another handle owns the slot, GPIO, or capture timer.
 * @retval ESY_ERR_NOT_SUPPORTED The board cannot configure the selected input.
 * @retval <0     Error code; failed cleanup retains resources for deinit retry.
 */
extern esy_err_t esy_hal_timer_capture_init(
    esy_timer_capture_t *cap,
    const esy_timer_capture_config_t *cfg);

/**
 * @brief Deinitialize a timer capture instance.
 *
 * Retires capture IRQ/callback access, resets board routing, then releases
 * ownership. Failure preserves the handle and remaining resources for retry;
 * keep callback context and handle storage alive until ESY_OK. A zero handle
 * is already deinitialized. Registered handles must first be unregistered.
 *
 * @param[in,out] cap Timer capture handle.
 * @retval ESY_OK Cleanup completed; the handle is zero and reusable.
 * @retval ESY_ERR_INVALID_ARG cap is NULL.
 * @retval ESY_ERR_STATE The handle is registered or lifecycle context is invalid.
 * @retval <0 Cleanup failed; retain storage and retry after resolving the cause.
 */
extern esy_err_t esy_hal_timer_capture_deinit(esy_timer_capture_t *cap);

/**
 * @brief Get the most recently captured timestamp.
 *
 * The timestamp is expressed in microseconds in the backend's capture-counter
 * domain, not necessarily time since boot. Initialization or power resume may
 * reset that domain. Before the first capture it reports zero. Each query
 * reads one complete ISR-published value; separate value/interval queries do
 * not form a coherent pair. On STM32F4 it wraps every 2^32 microseconds for TIM2/TIM5
 * and every 2^16 microseconds for the other supported timers.
 *
 * @param[in]  cap        Timer capture handle.
 * @param[out] out_value  Captured timestamp value (microseconds).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_timer_capture_get_value(
    const esy_timer_capture_t *cap,
    uint32_t *out_value);

/**
 * @brief Get the interval between the two most recent capture events.
 *
 * The interval is expressed in microseconds. Consecutive events must be less
 * than one capture-counter wrap apart; additional wraps cannot be detected.
 * Every port reports zero with ESY_OK until two events have been captured.
 * The ISR computes and publishes a complete interval, including a legitimate
 * zero timestamp. Pico/ESP32 use a 32-bit microsecond domain; STM32F4 uses the
 * selected 16-bit or 32-bit counter width. Separate value/interval queries do
 * not form a coherent pair.
 *
 * @param[in]  cap           Timer capture handle.
 * @param[out] out_interval  Time interval between captures (microseconds).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_timer_capture_get_interval(
    const esy_timer_capture_t *cap,
    uint32_t *out_interval);

/**
 * @brief Query the physical controller or channel index (optional).
 *
 * This function is provided for diagnostics, debugging, or platform-specific
 * introspection. Availability and meaning of the returned index are
 * platform-dependent.
 *
 * @param[in]  cap        Timer capture handle.
 * @param[out] out_index  Physical controller or channel index.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_timer_capture_get_phys_index(
    const esy_timer_capture_t *cap,
    uint8_t *out_index);

/**
 * @brief Query public TIMER_CAPTURE capabilities for an initialized handle.
 *
 * Uses @ref esy_timer_capture_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in] cap Initialized capture handle; externally synchronized.
 * @param[out] out_caps Public capability mask, written only on success.
 * @retval ESY_OK Capabilities returned.
 * @retval ESY_ERR_INVALID_ARG A pointer is NULL.
 * @retval ESY_ERR_NOT_IMPLEMENTED The backend has no capability query.
 */
esy_err_t esy_hal_timer_capture_get_caps(
    const esy_timer_capture_t *cap, uint32_t *out_caps);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_TIMER_CAPTURE_H */
