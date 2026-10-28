/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_adc.h
 * @brief HAL ADC interface (handle-oriented).
 *
 * Design principles:
 * - The caller provides storage for the ADC handle (@ref esy_adc_t).
 * - The HAL assembles backend-specific implementations and
 *   dispatches all operations through the handle.
 * - No direct use of physical controller indices by upper layers.
 * - All operations target @ref esy_adc_t* exclusively.
 *
 * Return conventions:
 * - Functions returning @ref esy_err_t:
 *   - ESY_OK indicates success.
 *   - Negative values indicate errors.
 * - Functions returning integers:
 *   - >= 0 indicates valid data (e.g. sample value).
 *   - < 0 indicates an error code.
 */

#ifndef ESY_HAL_ADC_H
#define ESY_HAL_ADC_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_adc_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

/**
 * @note Handle lifecycle: initialize caller-owned handle storage with {0} before
 * its first init. Serialize lifecycle and data operations on one owner context.
 * Repeated init, including init while registered, returns ESY_ERR_STATE without
 * changing the live handle. Stop asynchronous work before unregister/deinit.
 * Failed init dismantles partial HAL backend state when cleanup succeeds. If
 * cleanup fails, keep the handle alive and retry deinit before another init.
 * Registered handles cannot be deinitialized directly. Successful deinit returns
 * an unregistered, quiescent handle to the zero state, ready for another init.
 * Lifecycle, trigger and sampling calls require serialized task/startup context.
 * One owner serializes lifecycle across all ADC handles, including competing
 * physical-controller initialization and PM transitions. An occupied controller
 * returns BUSY before board configuration; this preflight is not a reservation
 * against callers that violate the shared lifecycle-owner contract.
 * Callbacks may only record data or notify that owner; do not call these APIs
 * from a callback. Each physical ADC and its assigned DMA stream have one owner.
 *
 * DMA uses 16-bit samples in a DMA-accessible, two-byte-aligned destination.
 * dma_cfg->length is the destination size in bytes, not the sample count. The
 * size must contain whole samples and, for scans, whole channel sequences.
 * STM32F4 accepts at most 65,535 samples per submission. It copies descriptors
 * and channel lists during submit; the caller's configuration remains unchanged.
 * Buffers and callback arguments remain borrowed until the terminal callback
 * returns or abort succeeds. Circular completion/half callbacks are nonterminal.
 * Ordinary callbacks run in IRQ context; if an error cannot retire hardware in
 * the ISR, the error callback is deferred until successful task-side abort.
 * If a failed submit still reports busy, ownership remains retained: keep all
 * borrowed storage alive and abort before reuse. Use an application deadline to
 * abort a transfer whose expected terminal callback has not arrived.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @note This initializer replaces every field. Apply caller overrides afterward.
 * @brief Fill ADC configuration structure with default values.
 *
 * Default policy:
 * - phys_index   = 0 (unselected; STM32F4 requires native ADC index 1, 2 or 3)
 * - inner.channel = 0
 * - sample_time  = ESY_ADC_SAMPLETIME_15CYCLES
 *
 * @param[out] cfg ADC configuration pointer; must not be NULL.
 */
extern void esy_hal_adc_default_cfg(esy_adc_config_t *cfg);

/**
 * @brief Initialize an ADC instance.
 *
 * This function binds a physical ADC controller/channel to the given
 * ADC handle and prepares it for sampling or DMA operations.
 *
 * @param[in,out] adc ADC handle (caller-provided storage).
 * @param[in]     cfg ADC configuration (physical index, channel, timing, etc.).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_adc_init(esy_adc_t *adc,
                                       const esy_adc_config_t *cfg);

/**
 * @brief Deinitialize an ADC instance.
 *
 * Retires DMA before releasing hardware and board pins. Successful repeated
 * calls are safe. Failure retains the handle and any unretired borrowed storage;
 * retry cleanup instead of clearing or reinitializing the handle.
 *
 * @param[in,out] adc ADC handle.
 * @retval ESY_OK Hardware released and handle returned to zero.
 * @retval ESY_ERR_STATE Handle is registered; unregister it first.
 * @retval <0 Cleanup failed; the live handle must be preserved.
 */
extern esy_err_t esy_hal_adc_deinit(esy_adc_t *adc);

/**
 * @brief Stop finite or circular ADC DMA and release its graph lease.
 *
 * Serialized task/startup context only, never from a callback. Success is
 * idempotent, emits no ordinary completion and guarantees no later buffer or
 * callback access. A deferred hardware-error callback may run in this task
 * during successful retirement. Failure retains storage and any graph lease;
 * keep the handle, buffer and callback arguments alive and retry.
 *
 * @param[in] adc Initialized handle, or an already deinitialized zero handle.
 * @retval ESY_OK Hardware and callbacks retired.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval <0 Retirement failure; ownership remains with the ADC.
 */
esy_err_t esy_hal_adc_abort_dma(esy_adc_t *adc);

/**
 * @brief Perform a single blocking ADC conversion.
 *
 * Starts a conversion and waits until the result is available.
 *
 * @param[in] adc ADC handle.
 *
 * @return
 * - >= 0 : Conversion result (e.g. 0-4095 for 12-bit ADC).
 * - < 0  : Error code.
 */
extern int32_t esy_hal_adc_sample_once(esy_adc_t *adc);

/**
 * @brief Start a one-shot single-channel DMA ADC conversion.
 *
 * The ADC fills length / sizeof(uint16_t) samples and stops before one terminal
 * completion. Circular descriptors are rejected. Software triggering repeats
 * conversions as needed; an external trigger supplies each sequence.
 *
 * @param[in] adc     ADC handle.
 * @param[in] dma_cfg DMA configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_adc_start_dma(esy_adc_t *adc,
                                            esy_dma_config_t *dma_cfg);

/**
 * @brief Start continuous DMA ADC sampling (circular mode).
 * @note This entry selects circular mode independently of dma_cfg->circular.
 * Registered handles retain a graph lease across every circular completion;
 * abort or terminal error releases it. One-shot APIs reject circular descriptors.
 * Descriptors are copied on submit; buffers and arguments remain borrowed until
 * successful abort or terminal error. Do not stop/restart/deinit from callbacks.
 *
 * The ADC continuously samples the configured channel and
 * streams data into a circular DMA buffer until stopped.
 *
 * @param[in] adc     ADC handle.
 * @param[in] dma_cfg DMA configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_adc_start_continuous_dma(esy_adc_t *adc,
                                                       esy_dma_config_t *dma_cfg);

/**
 * @brief Start one-shot multi-channel DMA scan conversion.
 *
 * The ADC stores interleaved samples in index_list order. The byte length must
 * be divisible by sizeof(uint16_t) * num_indexes; STM32F4 accepts 1..16 channels.
 * The channel list is copied during submission. Circular descriptors are
 * rejected; completion retires the whole finite buffer.
 *
 * @param[in] adc         ADC handle.
 * @param[in] index_list Array of platform-specific channel indices.
 * @param[in] num_indexes Number of channels to scan.
 * @param[in] dma_cfg     DMA configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_adc_start_multi_channel_dma(
    esy_adc_t *adc,
    const uint8_t *index_list,
    uint8_t num_indexes,
    esy_dma_config_t *dma_cfg);

/**
 * @brief Query whether the ADC (or its DMA engine) is busy.
 *
 * @param[in] adc ADC handle.
 *
 * @retval true  DMA is active or failed cleanup still retains its ownership.
 * @retval false ADC is idle.
 */
extern bool esy_hal_adc_is_busy(const esy_adc_t *adc);

/**
 * @brief Configure an external trigger source for ADC conversion.
 * @note Software source requires edge NONE; external sources require an edge.
 * Successful configuration survives registered runtime suspend/resume. Stop DMA
 * before changing the trigger. The trigger generator remains caller-owned.
 *
 * @param[in] adc    ADC handle.
 * @param[in] edge   Trigger edge selection (@ref esy_adc_trigger_edge_t).
 * @param[in] source Trigger source selection (@ref esy_adc_trigger_source_t).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_adc_set_trigger(esy_adc_t *adc,
                                              esy_adc_trigger_edge_t edge,
                                              esy_adc_trigger_source_t source);

/**
 * @brief Query public ADC capabilities for an initialized handle.
 *
 * Uses @ref esy_adc_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  adc      ADC handle.
 * @param[out] out_caps Capability bitmask.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_adc_get_caps(const esy_adc_t *adc,
                                           uint32_t *out_caps);

/**
 * @brief Query the physical ADC controller index (optional).
 *
 * @param[in]  adc       ADC handle.
 * @param[out] out_index Physical controller index.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_adc_get_phys_index(const esy_adc_t *adc,
                                                 uint8_t *out_index);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_ADC_H */
