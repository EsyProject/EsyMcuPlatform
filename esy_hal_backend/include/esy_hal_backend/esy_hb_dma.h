/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_dma.h
 * @brief DMA HAL backend interface.
 *
 * Design rules:
 * - The caller provides a fully populated @ref esy_dma_config_t structure.
 * - The HAL backend MUST NOT perform any dynamic memory allocation.
 * - The HAL backend is responsible for interpreting the semantics of dreq,
 *   channel numbers, IRQ indices, etc.
 * - If a feature or capability is not supported, the HAL backend shall return
 *   @ref ESY_ERR_NOT_SUPPORTED.
 * - Mutating transfer operations take the same non-NULL stable owner token.
 *   Successful prepare claims the physical channel/stream until successful
 *   abort or explicit native retirement/release. Hardware idle and terminal IRQ
 *   notification do not release a claim. Another prepare, including the same
 *   owner, returns BUSY until retirement. Foreign start/abort/control never
 *   changes the claimed resource. The owner token is the first argument of
 *   mutating operations and must outlive the prepared transaction.
 */

#ifndef ESY_HB_DMA_H
#define ESY_HB_DMA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Claim one physical DMA resource without configuring hardware.
 *
 * A non-NULL stable owner token identifies the transaction. Claims are atomic
 * across tasks/cores; an existing claim, including the same token, returns BUSY.
 * Prepare claims internally; this entry point is for native SDK DMA providers.
 * Serialize all operations belonging to one owner. IRQs must not acquire claims.
 * Pico also reserves the SDK channel claim and coordinates built-in Wi-Fi
 * lazy allocation; assembly contention returns BUSY for retry. External raw
 * SDK allocators must be serialized with HAL/backend claim operations, since
 * fixed-ID native claims are panic-on-conflict rather than try-claim APIs.
 * @param[in] owner Borrowed identity, alive until successful abort/release.
 * @param[in] dreq Platform routing key.
 * @param[in] channel Platform request/channel selector.
 * @retval ESY_OK Resource claimed while hardware is idle.
 * @retval ESY_ERR_BUSY Resource claimed or hardware active.
 * @retval <0 Invalid arguments or unavailable generic DMA.
 */
esy_err_t esy_hb_dma_claim(const void *owner, uint32_t dreq, uint8_t channel);

/**
 * @brief Release a matching claim after the native provider has retired access.
 *
 * This operation does not stop hardware or wait for hardware idle. The caller
 * must first stop peripheral requests, retire native IRQ/callback access, and
 * stop using the DMA handle. Supporting ports allow native completion IRQs to
 * release as their final ownership action;
 * no returning dispatcher may subsequently access the released native handle.
 * Hardware activity or a foreign claim returns BUSY without modification.
 * An already unclaimed idle resource returns OK. Successful release clears
 * prepared metadata before publishing the resource as available. Failed cleanup
 * must retain the claim, buffers and callback context for task-side retry.
 * Pico release is task-only and returns NOT_SUPPORTED in ISR context before
 * mutation. Its short metadata exclusion may await a bounded peer dispatcher;
 * callbacks must only notify and must not call task lifecycle operations.
 * @param[in] owner Non-NULL transaction identity used when claiming/preparing.
 * @param[in] dreq Platform routing key.
 * @param[in] channel Platform request/channel selector.
 * @retval ESY_OK Claim released or resource already unclaimed and idle.
 * @retval ESY_ERR_NOT_SUPPORTED Pico release called from ISR context.
 * @retval <0 Invalid route/owner, foreign claim, or hardware still active.
 */
esy_err_t esy_hb_dma_release(const void *owner, uint32_t dreq, uint8_t channel);

/**
 * @brief Resolve a routing pair to its physical DMA resource index.
 *
 * This pure metadata query is available before prepare. The returned index is
 * less than 32 and identifies one exclusive channel/stream within this backend.
 * Pico uses channel; STM32F4 uses the controller/stream encoded in dreq, while
 * channel selects the peripheral request. Distinct routing pairs that alias
 * one physical resource return the same index. Failure preserves the output.
 *
 * @param[in] dreq Platform routing key.
 * @param[in] channel Platform request/channel selector.
 * @param[out] out_id Physical resource index used by the backend start mask.
 * @retval ESY_OK Valid route and resource index.
 * @retval ESY_ERR_INVALID_ARG Invalid route or NULL output.
 * @retval ESY_ERR_NOT_SUPPORTED Generic DMA is unavailable on this backend.
 */
esy_err_t esy_hb_dma_get_resource_id(uint32_t dreq, uint8_t channel,
                                   uint8_t *out_id);

/**
 * @brief Prepare a DMA transfer (do not start immediately).
 *
 * Initializes the DMA channel according to @p transfer, but does not trigger
 * the transfer. This allows multiple channels to be prepared and then started
 * simultaneously. Task context only: serialize configuration and lifecycle.
 * Callbacks execute in ISR context and must be bounded and nonblocking. They
 * must not prepare/start_single/abort DMA (including through peripheral APIs);
 * Pico holds callback-retirement exclusion until the callback returns.
 * All public validation precedes claiming/hardware writes. A successful prepare
 * retains its claim across failed start and terminal IRQs until retirement.
 * STM32F4 memory-to-memory transfers require DMA2 and non-circular mode;
 * DMA1 rejects them with NOT_SUPPORTED before claiming or modifying hardware.
 * Concurrent Pico first-time assembly of one shared DMA IRQ may return BUSY;
 * retry from the task owner. That rejection retains no physical claim.
 * Pico DMA IRQs assembled on the same core must request exactly the same
 * irq_priority. The first assembly fixes that core's DMA priority until reset;
 * a conflicting prepare returns BUSY without retaining its new channel claim.
 * This prevents nested dispatch from starving the IRQ holding callback
 * retirement exclusion. Native users must not change those IRQ priorities.
 *
 * @param[in] owner Stable non-NULL transaction identity.
 * @param[in] transfer DMA transfer configuration
 * @retval ESY_OK Success
 * @retval <0     Negative error code on failure
 */
extern esy_err_t esy_hb_dma_prepare(const void *owner,
                                   const esy_dma_config_t *transfer);

/**
 * @brief Start prepared physical DMA resources.
 *
 * Validate the entire mask before starting anything. Failure starts no resource.
 * The mask uses the indices returned by esy_hb_dma_get_resource_id(), not the
 * public HAL's configured-route indices or raw peripheral request channels.
 * Every selected resource must be prepared by the matching owner before any
 * resource starts; a rejected batch preserves all prepared claims.
 *
 * @param[in] owner Stable identity used to prepare every selected resource.
 * @param[in] channel_mask Physical resource bitmask.
 * @retval ESY_OK Success
 * @retval <0     Negative error code on failure
 */
extern esy_err_t esy_hb_dma_start(const void *owner, uint32_t channel_mask);

/**
 * @brief Prepare and immediately start a single DMA transfer.
 *
 * This is equivalent to calling @ref esy_hb_dma_prepare() followed by
 * @ref esy_hb_dma_start() for the corresponding channel.
 * A failed start after successful preparation retains the claim for abort/retry.
 *
 * @param[in] owner Stable non-NULL transaction identity.
 * @param[in] transfer DMA transfer configuration
 * @retval ESY_OK Success
 * @retval <0     Negative error code on failure
 */
extern esy_err_t esy_hb_dma_start_single(const void *owner,
                                        const esy_dma_config_t *transfer);

/**
 * @brief Abort an ongoing DMA transfer on a specific channel from task context.
 *
 * Success retires hardware and its registered callbacks, including an idle
 * channel with a pending IRQ. Failure retains ownership; keep borrowed storage
 * and retry. Never call from a DMA callback.
 * Successful abort releases only the matching claim. An unclaimed idle route
 * returns OK; a foreign claim returns BUSY without stopping hardware or callbacks.
 *
 * @param[in] owner Stable identity used when claiming/preparing the resource.
 * @param[in] dreq    DMA request source identifier
 * @param[in] channel DMA channel number
 * @retval ESY_OK Success
 * @retval <0     Negative error code on failure
 */
extern esy_err_t esy_hb_dma_abort(const void *owner, uint32_t dreq,
                                 uint8_t channel);

/**
 * @brief Check whether a DMA channel is busy.
 *
 * @param[in] dreq    DMA request source identifier
 * @param[in] channel DMA channel number
 * @retval true  Transfer is in progress
 * @retval false Channel is idle or invalid
 */
extern bool esy_hb_dma_is_busy(uint32_t dreq, uint8_t channel);

/**
 * @brief Bind a DMA interrupt to a specific CPU core.
 *
 * Useful on multi-core systems where DMA IRQs can be routed to different cores.
 * On Pico this selects an owner before the IRQ's first prepare (default core 0).
 * The first prepare must execute on that selected core or returns STATE without
 * native IRQ/channel changes. Once assembled, prepares may run on either core;
 * rebinding returns BUSY. The consumer must not independently enable this IRQ
 * on the other core. No implicit cross-core execution or peer NVIC access occurs.
 *
 * @param[in] irq_index DMA interrupt index (platform-defined, e.g. 0 or 1)
 * @param[in] core_id   Target CPU core ID (e.g. 0 or 1)
 * @retval ESY_OK Success
 * @retval <0     Negative error code on failure
 */
extern esy_err_t esy_hb_dma_bind_irq_to_core(uint32_t irq_index,
                                                 uint32_t core_id);

/**
 * @brief Set the DMA read (source) address.
 *
 * Task context, prepared idle resource only. Invalid/alignment/range, busy,
 * unprepared and unsupported operations return an error without modification.
 * Success with trigger_now starts the configured transfer.
 *
 * @param[in] owner Stable identity used when preparing the resource.
 * @param[in] dreq        DMA request source identifier
 * @param[in] channel     DMA channel number
 * @param[in] addr        Source address
 * @param[in] trigger_now If true, trigger the transfer immediately
 */
extern esy_err_t esy_hb_dma_set_read_addr(const void *owner, uint32_t dreq, uint8_t channel,
                                         const void *addr, bool trigger_now);

/**
 * @brief Set the DMA write (destination) address.
 *
 * Uses the same validation and task-context contract as set_read_addr.
 *
 * @param[in] owner Stable identity used when preparing the resource.
 * @param[in] dreq        DMA request source identifier
 * @param[in] channel     DMA channel number
 * @param[out] addr       Destination address
 * @param[in] trigger_now If true, trigger the transfer immediately
 */
extern esy_err_t esy_hb_dma_set_write_addr(const void *owner, uint32_t dreq, uint8_t channel,
                                          void *addr, bool trigger_now);

/**
 * @brief Set the number of data items to be transferred by DMA.
 *
 * Count is in configured-width units, must be nonzero and fit the native
 * counter. Uses the same validation and task-context contract as set_read_addr.
 *
 * @param[in] owner Stable identity used when preparing the resource.
 * @param[in] dreq        DMA request source identifier
 * @param[in] channel     DMA channel number
 * @param[in] count       Number of transfer items
 * @param[in] trigger_now If true, trigger the transfer immediately
 */
extern esy_err_t esy_hb_dma_set_trans_count(const void *owner, uint32_t dreq, uint8_t channel,
                                           size_t count, bool trigger_now);

/**
 * @brief Get remaining DMA transfer count.
 *
 * The returned count is expressed in DMA transfer units (not bytes), where
 * the unit size is defined by the channel transfer width configuration.
 *
 * @param[in]  dreq            DMA request source identifier
 * @param[in]  channel         DMA channel number
 * @param[out] out_remaining   Remaining transfer count (units)
 * @retval ESY_OK Success
 * @retval <0     Negative error code on failure
 */
extern esy_err_t esy_hb_dma_get_trans_count_remaining(uint32_t dreq,
                                                          uint8_t channel,
                                                          size_t *out_remaining);

/**
 * @brief Configure DMA channel chaining.
 *
 * When the current channel completes, the next channel is automatically
 * triggered. This backend-only operation does not supply a public HAL chain
 * lifecycle. STM32F4 and ESP32 return ESY_ERR_NOT_SUPPORTED.
 *
 * @param[in] owner Stable identity that must own both physical resources.
 * @param[in] dreq          DMA request source identifier
 * @param[in] channel       Current DMA channel number
 * @param[in] chain_channel Next physical DMA resource index
 */
extern esy_err_t esy_hb_dma_chain_to(const void *owner, uint32_t dreq, uint8_t channel,
                                    uint8_t chain_channel);

/**
 * @brief Global DMA interrupt handler entry point.
 *
 * The platform-specific ISR should call this function to dispatch DMA
 * completion or error events to the HAL backend. STM32F4 supports single
 * buffer only; simultaneous errors suppress success/half notifications. Error
 * dispatch disables requests and IRQs without waiting for stream retirement.
 * The task owner must abort successfully before reclaiming storage.
 *
 * @param[in] dreq    DMA request source identifier
 * @param[in] channel DMA channel number
 */
extern void esy_hb_dma_handle_irq(uint32_t dreq, uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_DMA_H */
