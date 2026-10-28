/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_pwm.h
 * @brief HAL PWM interface (handle-based)
 *
 * Design notes:
 * - The caller provides storage for the PWM handle; the HAL assembles
 *   backend function pointers and forwards operations.
 * - No controller/channel index is exposed at the API level; all operations are
 *   performed on @ref esy_pwm_t*.
 * - Functions return @ref esy_err_t (0 on success, negative on failure) or
 *   non-negative values for valid results.
 *
 * Concurrency & context:
 * - Most APIs are synchronous configuration calls.
 * - One owner serializes all calls, including poll, abort and deinit. DMA IRQs
 *   only publish events; the owner must call poll to retire DMA and notify users.
 * - Callbacks registered here may run in interrupt context; blocking operations
 *   are not allowed inside callbacks.
 *
 * Pico shared slices:
 * - Serialize every PWM handle's calls on one task/core, including lifecycle.
 *   The backend exclusively owns its slices and the wrap IRQ on that core;
 *   do not reconfigure them or the output GPIO overrides through another driver.
 *   Board logical indices must map to distinct GPIOs. Backend preflight rejects
 *   an occupied logical index before its board pin is configured. Distinct GPIOs
 *   aliasing the same hardware slice/channel cannot have separate owners.
 * - A/B channels may share a slice at the same requested frequency. A conflicting
 *   init or frequency change while a peer remains initialized returns BUSY.
 *   Setting the existing frequency is a no-op; a sole owner may retune the slice.
 *   Keep clk_sys fixed while owned; the fractional divider quantizes frequency.
 *   Zero or a frequency above clk_sys is invalid; unrepresentable low frequency
 *   or 100% duty at a full 65536-tick period returns NOT_SUPPORTED.
 * - Init leaves its pin disabled. Enable, disable, suspend and deinit affect
 *   only that channel; the counter continues while any nonsuspended channel is
 *   enabled. Disabled/suspended pins use a GPIO override at logical inactive
 *   level (low normally, high when inverted), retaining their compare setting.
 *   Polarity changes preserve the peer's inversion. Resume does not reset phase;
 *   compare/preload changes and output transitions are not guaranteed glitch-free.
 * - DMA requires sole ownership of the slice, even if its peer is disabled.
 *   Pending DMA retirement prevents a peer joining. Wrap callbacks are delivered
 *   only for enabled, nonsuspended channels and must not call PWM APIs. All
 *   registered handlers on the shared IRQ require the same board IRQ priority;
 *   conflict returns BUSY. NULL unregisters only this handle, and successful
 *   unregister/deinit retires its callback context before returning.
 *
 * STM32F4 timing:
 * - Frequency conversion uses the selected timer's APB1/APB2 divider and, where
 *   present, TIMPRE. Integer PSC/ARR divisors quantize the requested frequency;
 *   an exact frequency is not guaranteed. Zero or a frequency above the timer
 *   input clock is invalid. Keep RCC clocks fixed while PWM is active; clock
 *   changes require owner-controlled output stop and frequency reconfiguration.
 * - TIM2/TIM5 use 32-bit periods; other supported timers use 16-bit periods.
 *   PSC and period belong to the whole timer. Coordinate channels through one
 *   owner and do not share that timer with logical timers or timer capture.
 * - Duty is quantized to compare ticks. A 100% request with ARR at its maximum
 *   cannot fit ARR+1 in the compare register and returns ESY_ERR_NOT_SUPPORTED.
 *   Invalid or unrepresentable duty/frequency requests return before changing
 *   the running registers. Other hardware failures may occur after reconfiguration.
 *
 * ESP32 shared timers:
 * - Physical channel pairs 0/1, 2/3, 4/5 and 6/7 share one low-speed LEDC timer
 *   at 13-bit resolution. Serialize calls across all ESP32 PWM handles through
 *   one owner, including init/deinit; do not configure these resources directly
 *   through the SDK or another driver while this backend owns them.
 * - A channel may join an occupied timer only at the same requested frequency.
 *   A conflicting init or frequency change while a peer owns the timer returns
 *   ESY_ERR_BUSY before changing it. Setting the current frequency is a no-op.
 *   A sole owner may change frequency. SDK divider quantization still applies.
 * - Init leaves output disabled. Successful final deinit releases the timer's
 *   SDK clock claim, allowing later init at another frequency. Cleanup failure
 *   retains the claim and handle for deinit retry; do not use or discard that
 *   handle until cleanup succeeds. A timer awaiting final cleanup rejects new
 *   channel claims even at the same frequency.
 */

#ifndef ESY_HAL_PWM_H
#define ESY_HAL_PWM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_common/defs/peripheral/esy_pwm_defs.h"

/**
 * @note Handle lifecycle: initialize caller-owned handle storage with {0} before
 * its first init. Serialize lifecycle and data operations on one owner context.
 * Repeated init, including init while registered, returns ESY_ERR_STATE without
 * changing the live handle. Stop asynchronous work before unregister/deinit.
 * A different handle using an occupied logical index returns BUSY before board
 * pin configuration. The board must not assign the same GPIO to different
 * logical indices; pin configuration/reset remains exclusive to that index.
 * Failed init attempts cleanup. A cleanup error preserves the live handle and
 * board pin ownership; retry deinit before reinitializing or releasing storage.
 * Registered handles cannot be deinitialized directly. Successful deinit returns
 * an unregistered, quiescent handle to the zero state, ready for another init.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @note This initializer replaces every field. Apply caller overrides afterward.
 * @brief Fill a PWM configuration structure with platform default values.
 *
 * The defaults are platform-specific and intended to be safe starting values.
 * The caller may override individual fields after this call.
 *
 * @param[out] cfg Pointer to the configuration structure; must not be NULL.
 */
extern void esy_hal_pwm_default_cfg(esy_pwm_config_t *cfg);

/**
 * @brief Initialize a PWM instance.
 *
 * This function configures the underlying PWM hardware according to @p cfg
 * and binds it to the provided handle.
 *
 * @param[in,out] pwm PWM handle storage provided by the caller.
 * @param[in]     cfg PWM configuration (physical index, GPIO, timing parameters, etc.).
 *
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  Invalid arguments.
 * @retval ESY_ERR_BUSY         Hardware resource is busy.
 * @retval ESY_ERR_NO_MEM       Backend storage is insufficient.
 * @retval ESY_ERR_NOT_SUPPORTED Requested timing cannot be represented.
 * @retval ESY_ERR_FAIL         Other platform-specific failure.
 */
extern esy_err_t esy_hal_pwm_init(esy_pwm_t *pwm,
                                       const esy_pwm_config_t *cfg);

/**
 * @brief Deinitialize a PWM instance.
 *
 * Releases all hardware resources associated with the PWM handle.
 * The zero handle is idempotent. Registered handles return ESY_ERR_STATE.
 * Call on the owner outside callbacks. DMA is aborted first; failure preserves
 * the handle, pin ownership, buffers and callbacks for a later retry.
 *
 * @param[in,out] pwm PWM handle.
 * @return ESY_OK after retirement, or a cleanup error with ownership retained.
 */
extern esy_err_t esy_hal_pwm_deinit(esy_pwm_t *pwm);

/**
 * @brief Set PWM duty cycle.
 *
 * @param[in] pwm        PWM handle.
 * @param[in] duty_cycle Duty cycle in the range [0.0, 1.0].
 *
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  Invalid duty cycle or handle.
 * @retval ESY_ERR_STATE        PWM not initialized or not enabled.
 * @retval ESY_ERR_NOT_SUPPORTED Duty endpoint cannot be represented by this period.
 * @retval ESY_ERR_FAIL         Other failure.
 */
extern esy_err_t esy_hal_pwm_set_duty(esy_pwm_t *pwm, float duty_cycle);

/**
 * @brief Set PWM output frequency (period/wrap).
 *
 * @param[in] pwm     PWM handle.
 * @param[in] freq_hz Target frequency in Hz; must be greater than 0.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid frequency.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_BUSY          A Pico/ESP32 peer owns the shared timer at a
 *                              different frequency, or this handle is busy.
 * @retval ESY_ERR_NOT_SUPPORTED Runtime change or requested timing not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 * @note STM32F4 does not arbitrate frequency changes between channels. Its
 * single owner must stop affected outputs and coordinate the timer-wide PSC/ARR
 * update before calling this function; see the file-level timing contract.
 */
extern esy_err_t esy_hal_pwm_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz);

/**
 * @brief Enable or disable PWM output.
 *
 * @param[in] pwm    PWM handle.
 * @param[in] enable true to enable output, false to disable.
 *
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  Invalid arguments.
 * @retval ESY_ERR_STATE        PWM not initialized.
 * @retval ESY_ERR_FAIL         Other failure.
 */
extern esy_err_t esy_hal_pwm_enable(esy_pwm_t *pwm, bool enable);

/**
 * @brief Configure PWM output polarity.
 *
 * @param[in] pwm       PWM handle.
 * @param[in] start_low true if the output starts low; false if it starts high.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_NOT_SUPPORTED Polarity control not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_set_polarity(esy_pwm_t *pwm, bool start_low);

/**
 * @brief Enable or disable trigger output (optional feature).
 *
 * Trigger outputs are typically used to drive ADC/DMA or other peripherals.
 *
 * @param[in] pwm     PWM handle.
 * @param[in] enabled true to enable trigger output, false to disable.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_NOT_SUPPORTED Trigger output not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_enable_trigger_output(esy_pwm_t *pwm,
                                                        bool enabled);

/**
 * @brief Configure and start DMA-driven PWM waveform output (optional).
 * @note Start from disabled output. The descriptor is copied; length is bytes.
 * STM32F4 accepts aligned 16/32-bit compare elements (32-bit only on TIM2/5),
 * selects CCx DMA requests at update events and temporarily owns timer CCDS.
 * The board must supply a valid timer/channel DMA route and exclusive ownership
 * of that physical DMA resource. Do not use it through generic DMA or another
 * peripheral until PWM retirement. Pico supports finite 16-bit elements; its
 * generic DMA provider rejects circular mode. Pico also requires sole slice
 * ownership, including disabled peers. ESP32 DMA remains unsupported.
 * @note Either loop or dma_cfg.circular requests circular operation. Finite
 * completion/error and circular cycle/half notifications are delivered only by
 * esy_hal_pwm_poll on the owner. Circular notifications may coalesce between
 * polls. Keep buffers, handle and callback arguments alive until a terminal
 * callback returns or abort succeeds; hardware idle or an IRQ is insufficient.
 * Submission/cleanup failure can retain a borrow: retry abort before reuse.
 * While DMA/callback work is pending, control calls return BUSY; deinit first
 * attempts retirement and retains ownership if cleanup fails.
 * Callbacks must not reenter PWM calls or release handle/binding storage.
 *
 * @param[in] pwm     PWM handle.
 * @param[in] dma_cfg DMA configuration describing the transfer.
 * @param[in] loop    true for continuous looping, false for one-shot.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_BUSY          DMA or PWM resource busy.
 * @retval ESY_ERR_NOT_SUPPORTED DMA-based PWM output not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_dma_output(esy_pwm_t *pwm,
                                             esy_dma_config_t *dma_cfg,
                                             bool loop);

/**
 * @brief Abort PWM DMA and retire all pending callback references.
 * @param[in,out] pwm Initialized PWM handle.
 * @return ESY_OK when idle/retired; cleanup failure retains buffers and state.
 * @note Owner context only, outside callbacks. Successful abort suppresses
 * pending DMA notifications and disables DMA output. It leaves ordinary idle
 * PWM output unchanged. Provider abort may wait for hardware acknowledgement;
 * this is not an ISR API or a hard real-time bound.
 */
extern esy_err_t esy_hal_pwm_abort_dma(esy_pwm_t *pwm);

/**
 * @brief Advance retirement and deliver at most one notification of each kind.
 * @param[in,out] pwm Initialized PWM handle.
 * @return ESY_OK, or a provider cleanup error with DMA ownership retained.
 * @note Call regularly from the serialized owner until finite completion/error
 * is delivered, and while circular notifications are desired. No fixed period
 * is imposed: polling cadence bounds notification latency, not waveform timing.
 * Terminal work retires hardware and callbacks before notifying the caller.
 * Finite completion reports consumed DMA elements and leaves PWM enabled,
 * holding the final compare after its next hardware preload update. It does
 * not promise the end of that output period. Explicitly disable ordinary output
 * before runtime suspension. Error/abort stops output instead.
 * Cleanup may wait in the provider; user callback duration is part of this call.
 */
extern esy_err_t esy_hal_pwm_poll(esy_pwm_t *pwm);

/**
 * @brief Register a wrap (period) interrupt callback.
 *
 * The callback is invoked when the PWM counter wraps. The port acknowledges
 * its owned interrupt before notification; callers do not clear native flags.
 * NULL disables notifications and retires the previous callback argument before
 * returning success. Keep that argument alive until unregister/deinit succeeds.
 * One task/core serializes registration and lifecycle; callbacks only notify
 * the owner and must not reenter PWM configuration or lifecycle. STM32F4 owns
 * its complete timer and update vector; do not share them with another driver.
 * Its dispatch entry acknowledges UPDATE once and notifies every registered
 * channel on that timer. Those channels require one owner and IRQ priority;
 * unregistering one channel preserves the other channels' notifications.
 * ESP32 LEDC does not support period callbacks. A non-NULL handler returns
 * NOT_SUPPORTED without borrowing its argument or allocating an ISR; NULL
 * performs callback cleanup and succeeds only after any native resource retires.
 *
 * @param[in] pwm     PWM handle.
 * @param[in] handler Callback function (NULL to unregister).
 * @param[in] arg     User argument passed to the callback.
 *
 * @warning The callback may run in interrupt context.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_NOT_SUPPORTED Wrap interrupt not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_set_wrap_handler(esy_pwm_t *pwm,
                                                   void (*handler)(void *),
                                                   void *arg);

/**
 * @brief Suspend a PWM instance for low-power operation (optional).
 *
 * @param[in] pwm PWM handle.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_NOT_SUPPORTED Suspend not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_suspend(esy_pwm_t *pwm);

/**
 * @brief Resume a previously suspended PWM instance (optional).
 *
 * @param[in] pwm PWM handle.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_NOT_SUPPORTED Resume not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_resume(esy_pwm_t *pwm);

/**
 * @brief Enable or disable low-power run mode (optional).
 *
 * @param[in] pwm          PWM handle.
 * @param[in] enable_lprun true to enable low-power run mode.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_NOT_SUPPORTED Low-power run mode not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_set_low_power_mode(esy_pwm_t *pwm,
                                                     bool enable_lprun);

/**
 * @brief Select the PWM clock source (optional).
 *
 * @param[in] pwm          PWM handle.
 * @param[in] clock_source Platform-defined clock source identifier.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid clock source.
 * @retval ESY_ERR_STATE         PWM not initialized.
 * @retval ESY_ERR_NOT_SUPPORTED Clock source selection not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_set_clock_source(esy_pwm_t *pwm,
                                                   uint8_t clock_source);

/**
 * @brief Query public PWM capabilities for an initialized handle.
 *
 * Uses @ref esy_pwm_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  pwm      PWM handle.
 * @param[out] out_caps Capability bitmask.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_NOT_SUPPORTED Capability query not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_get_caps(const esy_pwm_t *pwm,
                                           uint32_t *out_caps);

/**
 * @brief Get the physical controller index (optional).
 *
 * @param[in]  pwm       PWM handle.
 * @param[out] out_index Physical controller index.
 *
 * @retval ESY_OK                Success.
 * @retval ESY_ERR_INVALID_ARG   Invalid arguments.
 * @retval ESY_ERR_NOT_SUPPORTED Query not supported.
 * @retval ESY_ERR_FAIL          Other failure.
 */
extern esy_err_t esy_hal_pwm_get_phys_index(const esy_pwm_t *pwm,
                                                 uint8_t *out_index);

/**
 * @brief PWM interrupt dispatch entry.
 *
 * Called from the platform interrupt vector to handle PWM-related interrupts.
 *
 * @param[in] index Physical controller index.
 */
extern void esy_hal_pwm_handle_irq(uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_PWM_H */
