/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_adc.h
 * @brief ADC HAL backend interface (handle-based API; no internal dynamic allocation).
 *
 * Design constraints:
 * - The caller provides storage for @ref esy_adc_t; the HAL backend must NOT allocate
 *   memory from heap (malloc/new) internally.
 * - Physical controller and channel selection are provided via
 *   @ref esy_adc_config_t (e.g., phys_index and channel mapping).
 * - If a capability is not supported by the backend, the HAL backend should return
 *   @ref ESY_ERR_NOT_SUPPORTED or an equivalent negative error code.
 * - One owner serializes lifecycle across every ADC handle, from availability
 *   preflight through init and including PM transitions. Serialize trigger and
 *   sampling calls in task/startup context as well.
 *   DMA/ADC callbacks are bounded notifications and must not reenter these calls.
 * - DMA descriptors use byte lengths and two-byte-aligned, DMA-accessible 16-bit
 *   sample buffers. STM32F4 converts bytes to native DMA units internally and
 *   accepts up to 65,535 samples, including whole scan sequences only.
 * - Descriptors/channel lists are copied during submit. Buffers and callback
 *   arguments remain borrowed until terminal callback return or successful abort.
 *   A failed submit that remains busy retains this ownership until abort.
 * - STM32F4 links the ADC provider to the generic DMA owner's existing stream
 *   handle. Board MSP code supplies ADC clocks/GPIO, not a second DMA handle.
 *   Reserve each physical ADC and assigned DMA stream for this one owner.
 */

#ifndef ESY_HB_ADC_H
#define ESY_HB_ADC_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_adc_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name ADC HAL backend APIs (handle-only, explicit extern) */
/** @{ */

/**
 * @brief Check physical-controller availability before board configuration.
 *
 * This read-only task/startup query returns BUSY for an existing owner and
 * INVALID_ARG for an unavailable index. One owner must serialize all ADC
 * lifecycle operations, including this check through the following init;
 * the query does not reserve hardware. No state or board routing is changed.
 *
 * @param[in] phys_index Physical ADC controller index.
 * @return ESY_OK when available, or a negative error without side effects.
 */
extern esy_err_t esy_hb_adc_check_available(uint8_t phys_index);

/**
 * @brief Initialize an ADC controller/channel.
 *
 * The HAL backend configures and binds the underlying ADC hardware according to
 * @p config. No heap allocation is performed.
 * Initialize caller-owned handle storage with {0} before first use. A live
 * handle must not be copied, moved or cleared. Repeated init returns STATE
 * without changing its resources. Failed init may retain partial native state;
 * keep the handle alive and complete deinit before retrying. Successful deinit
 * clears ctx and permits another init; cleanup failure retains it for retry.
 *
 * @param[in,out] adc     ADC handle storage (provided by the caller)
 * @param[in]     config  ADC configuration (includes phys_index/channel, etc.)
 * @retval ESY_OK Success
 * @retval <0     Negative error code on failure
 */
extern esy_err_t esy_hb_adc_init(esy_adc_t *adc,
                                     const esy_adc_config_t *config);

/**
 * @brief Deinitialize the ADC.
 *
 * Releases backend resources and resets internal state. The storage of @p adc
 * itself is owned by the caller and is not freed by the HAL backend. Success
 * clears @p adc->ctx after hardware/callback retirement. Failure preserves live
 * backend state and borrowed storage so the task owner can retry cleanup.
 *
 * @param[in,out] adc ADC handle
 * @retval ESY_OK Retired, including an already deinitialized zero handle.
 * @retval <0 Cleanup failed; preserve the handle for retry.
 */
extern esy_err_t esy_hb_adc_deinit(esy_adc_t *adc);

/**
 * @brief Stop ADC requests and retire the single generic-owned DMA stream.
 *
 * Serialized task/startup context only; never call from a DMA/ADC callback.
 * Success is idempotent and guarantees no later buffer/callback access.
 * Failure retains the handle, descriptor and borrowed storage for retry.
 * A latched error whose ISR could not retire hardware is delivered after a
 * successful task-side retry; ordinary cancellation produces no completion.
 *
 * @param[in] adc ADC handle, non-NULL.
 * @return ESY_OK when retired; a negative error otherwise.
 */
esy_err_t esy_hb_adc_abort_dma(esy_adc_t *adc);

/**
 * @brief Perform a single blocking conversion.
 *
 * The call blocks until one conversion completes (or an error occurs).
 *
 * @param[in] adc ADC handle
 * @return >=0 Sample value (e.g., 0~4095 for 12-bit ADC)
 * @return <0  Negative error code on failure
 */
extern int32_t esy_hb_adc_sample_once(esy_adc_t *adc);

/**
 * @brief Configure and start a single-channel DMA transfer (one-shot).
 *
 * The caller provides @p dma_config (addresses, length, callbacks, and inner
 * routing information if required). The HAL backend does not allocate memory.
 * The byte length is nonzero and divisible by sizeof(uint16_t); circular mode
 * is rejected. STM32F4 starts the linked generic DMA handle exactly once through
 * the ADC provider, then retires it before terminal completion.
 *
 * @param[in] adc        ADC handle
 * @param[in] dma_config DMA configuration
 * @retval ESY_OK Success
 * @retval <0    Negative error code on failure
 */
extern esy_err_t esy_hb_adc_transfer_dma(esy_adc_t *adc,
                                             esy_dma_config_t *dma_config);

/**
 * @brief Query whether ADC DMA is busy.
 *
 * @param[in] adc ADC handle
 * @retval true  DMA transfer is in progress
 * @retval false DMA is idle
 */
extern bool esy_hb_adc_is_dma_busy(const esy_adc_t *adc);

/**
 * @brief Configure and start a multi-channel DMA scan transfer (one-shot).
 *
 * Channels are scanned in the order specified by @p index_list. The byte length
 * must be divisible by sizeof(uint16_t) * num_indexes; STM32F4 supports 1..16
 * channels. This finite API rejects circular mode.
 *
 * @param[in] adc         ADC handle
 * @param[in] dma_config  DMA configuration
 * @param[in] index_list  Channel index list
 * @param[in] num_indexes Number of channels in @p index_list
 * @retval ESY_OK Success
 * @retval <0    Negative error code on failure
 */
extern esy_err_t esy_hb_adc_transfer_dma_multi_channel(
    esy_adc_t *adc, esy_dma_config_t *dma_config,
    const uint8_t *index_list, uint8_t num_indexes);

/**
 * @brief Start continuous DMA sampling (circular mode).
 *
 * This mode is platform-dependent. If unsupported, return
 * @ref ESY_ERR_NOT_SUPPORTED.
 * STM32F4 selects circular DMA regardless of the descriptor's circular flag.
 * Its completion and half callbacks report cycles and retain borrowed storage
 * until successful abort or terminal error. ADC and DMA IRQ priorities are equal
 * for each active ADC; concurrently active ADCs share that callback priority.
 *
 * @param[in] adc        ADC handle
 * @param[in] dma_config DMA configuration
 * @retval ESY_OK Success
 * @retval <0    Negative error code on failure
 */
extern esy_err_t esy_hb_adc_start_continuous_dma(esy_adc_t *adc,
                                                     esy_dma_config_t *dma_config);

/**
 * @brief Configure an external trigger source (e.g., timer TRGO).
 *
 * Trigger selection uses the public ADC enumerations. The backend maps each
 * supported source to its native hardware trigger.
 *
 * @param[in] adc    ADC handle
 * @param[in] edge   Trigger edge selection
 * @param[in] source Trigger source selection
 * @retval ESY_OK Success
 * @retval <0    Negative error code on failure
 */
extern esy_err_t esy_hb_adc_set_trigger_source(esy_adc_t *adc,
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
 * @param[in]  adc       ADC handle
 * @param[out] out_caps  Capability bitmask
 * @retval ESY_OK Success
 * @retval <0    Negative error code (ESY_ERR_NOT_SUPPORTED if not available)
 */
extern esy_err_t esy_hb_adc_get_caps(const esy_adc_t *adc,
                                         uint32_t *out_caps);

/**
 * @brief Get the physical ADC controller index (optional).
 *
 * @param[in]  adc        ADC handle
 * @param[out] out_index  Physical controller index
 * @retval ESY_OK Success
 * @retval <0    Negative error code (ESY_ERR_NOT_SUPPORTED if not available)
 */
extern esy_err_t esy_hb_adc_get_phys_index(const esy_adc_t *adc,
                                               uint8_t *out_index);

/**
 * @brief Dispatch the shared native ADC IRQ for active backend-owned instances.
 *
 * The unique ADC_IRQHandler owner forwards exactly once. This bounded ISR
 * entry handles native overrun/error events without allocating or blocking.
 */
void esy_hb_adc_handle_irq(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_ADC_H */
