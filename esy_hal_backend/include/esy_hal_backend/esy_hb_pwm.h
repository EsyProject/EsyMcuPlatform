/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_pwm.h
 * @brief PWM HAL backend interface (handle-based, no internal allocation).
 *
 * Design rules:
 * - The caller provides handle storage; the HAL backend must not allocate from heap.
 * - Physical controller index is provided via @ref esy_pwm_config_t.
 * - Unsupported capabilities must return @ref ESY_ERR_NOT_SUPPORTED (or equivalent).
 */

#ifndef ESY_HB_PWM_H
#define ESY_HB_PWM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_common/defs/peripheral/esy_pwm_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name HAL backend top-level APIs (handle-based, explicit extern) */
/** @{ */

/**
 * @brief Check a logical PWM slot before the HAL configures its board pin.
 *
 * This read-only preflight checks the index and existing backend ownership.
 * It does not reserve resources or validate board-provided routing. Serialize
 * it with init/deinit on the same owner as every other PWM operation; init
 * repeats the ownership check after board metadata has been supplied.
 *
 * @param[in] phys_index Logical backend slot selected by the board port.
 * @return ESY_OK if available, INVALID_ARG for an invalid slot, or BUSY while
 * another handle owns the slot, including partially completed cleanup.
 */
extern esy_err_t esy_hb_pwm_check_available(uint8_t phys_index);

/**
 * @brief Initialize a PWM channel.
 *
 * The HAL backend initializes @p pwm and configures the
 * underlying PWM hardware according to @p config.
 *
 * @param[in,out] pwm    PWM handle storage (provided by the caller)
 * @param[in]     config PWM configuration (includes phys_index and parameters)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_pwm_init(esy_pwm_t *pwm,
                                     const esy_pwm_config_t *config);

/**
 * @brief Deinitialize a PWM channel.
 *
 * The implementation should release hardware resources and clear the internal
 * context of @p pwm only after retirement succeeds. Failure retains ownership.
 * This serialized-owner call may wait in DMA abort and must not run in ISR.
 *
 * @param[in,out] pwm PWM handle
 */
extern esy_err_t esy_hb_pwm_deinit(esy_pwm_t *pwm);

/**
 * @brief Set PWM duty cycle.
 *
 * @param[in] pwm        PWM handle
 * @param[in] duty_cycle Duty cycle in range [0.0, 1.0]
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_pwm_set_duty(esy_pwm_t *pwm, float duty_cycle);

/**
 * @brief Set PWM output frequency.
 *
 * @param[in] pwm     PWM handle
 * @param[in] freq_hz Target frequency in Hz
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 *
 * @note On some platforms, the effective "wrap/period" to frequency mapping is
 *       not continuous; achievable frequencies are constrained by the base
 *       clock and divider granularity.
 */
extern esy_err_t esy_hb_pwm_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz);

/**
 * @brief Enable or disable PWM output.
 *
 * @param[in] pwm    PWM handle
 * @param[in] enable true to enable output, false to disable
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_pwm_enable(esy_pwm_t *pwm, bool enable);

/**
 * @brief Set output polarity (initial level).
 *
 * @param[in] pwm       PWM handle
 * @param[in] start_low true to start low, false to start high
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_pwm_set_polarity(esy_pwm_t *pwm, bool start_low);

/**
 * @brief Enable or disable trigger output.
 *
 * This is typically used to generate a trigger for peripherals such as ADC.
 *
 * @param[in] pwm     PWM handle
 * @param[in] enabled true to enable, false to disable
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_pwm_enable_trigger_output(esy_pwm_t *pwm, bool enabled);

/**
 * @brief Configure PWM DMA output (optional).
 *
 * @param[in] pwm        PWM handle
 * @param[in] dma_config DMA configuration (provided by caller)
 * @param[in] loop       true for cyclic/loop mode, false for one-shot
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_dma_output(esy_pwm_t *pwm,
                                           esy_dma_config_t *dma_config,
                                           bool loop);

/**
 * @brief Retire an owned DMA route and suppress pending callbacks.
 * @param[in,out] pwm Initialized handle; idle abort is idempotent.
 * @return ESY_OK after callback storage retirement, otherwise ownership remains.
 * @note Serialized owner only, outside IRQ/user callbacks. May wait in provider.
 */
extern esy_err_t esy_hb_pwm_abort_dma(esy_pwm_t *pwm);

/**
 * @brief Retire terminal DMA and deliver coalesced notifications on the owner.
 * @param[in,out] pwm Initialized handle.
 * @return ESY_OK, or cleanup error with buffer/callback ownership retained.
 * @note At most one half/full callback per call; error takes precedence.
 * Finite/error callbacks run after DMA retirement. Finite completion keeps PWM
 * output enabled to latch/hold its final compare; error/abort stops output.
 * Circular callbacks retain
 * the route. Caller serializes this call with every PWM operation.
 */
extern esy_err_t esy_hb_pwm_poll(esy_pwm_t *pwm);

/**
 * @brief Register a wrap interrupt callback.
 *
 * NULL retires the previous callback argument before returning success; the
 * source stays enabled while another channel on that timer is registered.
 * Lifecycle is serialized on the dispatch core;
 * callbacks must not reenter control or lifecycle. STM32F4's public IRQ entry
 * tests and acknowledges UPDATE once before notifying every registered channel
 * on that timer; no external ACK is needed. Shared-vector priorities must match.
 * Reserve its complete timer/update vector from independent native users.
 * ESP32 LEDC has no supported period event: non-NULL registration returns
 * ESY_ERR_NOT_SUPPORTED without borrowing the callback or allocating an ISR.
 * NULL still retires any retained native registration and succeeds only after
 * cleanup; a cleanup error retains that resource for retry.
 *
 * @param[in] pwm          PWM handle
 * @param[in] handler      Callback function (may run in ISR context)
 * @param[in] arg          User argument passed to @p handler
 * @param[in] irq_priority Interrupt priority (platform-defined; smaller is higher)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 *
 * @note The callback may be invoked in interrupt context; keep it short and
 *       avoid blocking operations.
 */
extern esy_err_t esy_hb_pwm_set_wrap_handler(esy_pwm_t *pwm,
                                                 void (*handler)(void *),
                                                 void *arg,
                                                 uint8_t irq_priority);

/**
 * @brief Suspend a PWM channel (optional, for low-power support).
 *
 * The implementation should save necessary state and stop hardware as needed.
 *
 * @param[in] pwm PWM handle
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_suspend(esy_pwm_t *pwm);

/**
 * @brief Resume a PWM channel (optional).
 *
 * Restores state previously saved by @ref esy_hb_pwm_suspend.
 *
 * @param[in] pwm PWM handle
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_resume(esy_pwm_t *pwm);

/**
 * @brief Configure low-power run mode (optional).
 *
 * @param[in] pwm          PWM handle
 * @param[in] enable_lprun true to enable low-power run, false to disable
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_set_low_power_mode(esy_pwm_t *pwm, bool enable_lprun);

/**
 * @brief Select PWM clock source (optional).
 *
 * @param[in] pwm          PWM handle
 * @param[in] clock_source Clock source selector/value (platform-defined)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_set_clock_source(esy_pwm_t *pwm, uint8_t clock_source);

/**
 * @brief Set dead-time (optional, relevant for complementary outputs).
 *
 * @param[in] pwm          PWM handle
 * @param[in] dead_time_ns Dead-time in nanoseconds
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_set_dead_time(esy_pwm_t *pwm, uint32_t dead_time_ns);

/**
 * @brief Enable or disable complementary output (optional).
 *
 * @param[in] pwm    PWM handle
 * @param[in] enable true to enable, false to disable
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_enable_complementary(esy_pwm_t *pwm, bool enable);

/**
 * @brief PWM IRQ dispatch entry (called by the platform IRQ handler).
 *
 * @param[in] index Physical controller index
 *
 * @note If a platform still routes IRQs by "timer ID", map it to the proper
 *       physical index in platform code before calling this function.
 */
extern void esy_hb_pwm_handle_irq(uint32_t index);

/**
 * @brief Query public PWM capabilities for an initialized handle.
 *
 * Uses @ref esy_pwm_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  pwm      PWM handle
 * @param[out] out_caps Public capability bitmask
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_get_caps(const esy_pwm_t *pwm, uint32_t *out_caps);

/**
 * @brief Query physical controller index (optional).
 *
 * @param[in]  pwm       PWM handle
 * @param[out] out_index Physical controller index
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_pwm_get_phys_index(const esy_pwm_t *pwm, uint8_t *out_index);

/** @} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_PWM_H */
