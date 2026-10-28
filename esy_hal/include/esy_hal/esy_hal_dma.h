/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_dma.h
 * @brief HAL DMA interface.
 *
 * This header defines the HAL DMA abstraction.
 *
 * It provides two typical usage patterns:
 * 1) "Prepare first, start later":
 *    - Configure one or more channels with @ref esy_hal_dma_prepare
 *    - Start a set of channels simultaneously with @ref esy_hal_dma_start
 * 2) "One-shot start":
 *    - Configure and start a transfer immediately with @ref esy_hal_dma_start_single
 *
 * It also exposes common control operations such as setting read/write address,
 * transfer count, and explicit rejection of unsupported channel chaining.
 *
 * Contract:
 * - The caller-owned @ref esy_dma_t object must be zero-initialized before init.
 * - Configured routes must resolve to distinct physical DMA resources. Request
 *   channel selectors may repeat on different streams. The route count must not
 *   exceed @ref ESY_HAL_DMA_MAX_TRACKED_CHANNELS. Routing metadata stays immutable
 *   and borrowed until deinit.
 * - Prepare/start/poll/abort/control require one serialized task owner. IRQs only
 *   publish events. Drive poll until terminal retirement; callbacks run in that
 *   task, must be bounded, and must not reenter this controller's APIs.
 * - Prepare borrows buffers/callback arguments until successful abort or terminal
 *   callback return. Failed start retains the prepared request for retry/abort.
 *   Failed retirement retains ownership and any graph lease; continue poll or
 *   retry abort before freeing storage. Half/circular callbacks are nonterminal.
 * - Init validates routes without reserving hardware. Prepare exclusively claims
 *   each physical resource across controllers and peripheral DMA users. A second
 *   prepare returns BUSY, even before the first transfer starts or after hardware
 *   completion while retirement remains pending. Abort/control/start cannot
 *   modify another owner's transfer. Keep the controller at a stable address
 *   until every accepted request has retired; never copy a live handle.
 * - On Pico, claims also reserve channels in the native SDK claim table. The
 *   built-in Wi-Fi provider shares a nonblocking assembly guard with HAL DMA;
 *   contention returns BUSY or is retried by that provider. Direct external SDK
 *   allocators must be serialized with HAL/peripheral DMA claim/release because
 *   they do not participate in that guard. The SDK fixed-channel claim has no
 *   recoverable atomic try-claim operation.
 * - Functions returning @ref esy_err_t return ESY_OK on success, negative values on failure.
 * - @c dreq is the DMA request source identifier (platform-defined).
 * - @c channel is the DMA channel index.
 * - A start mask's bit N selects entry N in the configured channels array, not
 *   a hardware channel, STM32 request selector, stream number or DREQ value.
 *   The HAL translates selected routes to backend physical resource indices.
 *
 * Notes:
 * - A channel must be prepared before start or any address/count control.
 *   Prepared storage is borrowed even though is_busy does not report it busy.
 * - Some platforms separate DMA channels per peripheral or per DREQ group; the mapping is
 *   platform-specific and defined in the implementation.
 */

#ifndef ESY_HAL_DMA_H
#define ESY_HAL_DMA_H

#include <stdint.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_common/device/esy_device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESY_HAL_DMA_MAX_TRACKED_CHANNELS 16u

struct esy_dma_s;

/** @brief Optional DMA-controller clock/domain transition hook. */
typedef esy_err_t (*esy_dma_device_transition_fn_t)(
    struct esy_dma_s *dma, esy_device_pm_action_t action,
    esy_power_state_t system_state, void *arg);

/** @brief Internal per-channel state embedded in a caller-owned DMA handle. */
typedef struct {
  struct esy_dma_s *owner;
  esy_dma_callback_t complete_cb;
  void *complete_arg;
  esy_dma_callback_t half_cb;
  void *half_arg;
  esy_dma_callback_t error_cb;
  void *error_arg;
  uint32_t dreq;
  uint8_t channel;
  uint8_t resource_id; /**< Backend physical channel/stream identity. */
  esy_device_atomic_u32_t events; /**< Internal IRQ-to-owner event publication. */
  bool ref_held;
  bool circular;
  bool prepared;
  bool started;
} esy_dma_channel_runtime_t;

/** @brief Caller-owned DMA-controller handle. */
typedef struct esy_dma_s {
  esy_device_t device;
  const esy_dma_inner_config_t *channels; /**< Borrowed immutable routing list. */
  uint32_t channel_count;
  esy_dma_device_transition_fn_t transition;
  void *transition_arg;
  esy_dma_channel_runtime_t runtime[ESY_HAL_DMA_MAX_TRACKED_CHANNELS];
  bool initialized;
} esy_dma_t;

/** @brief DMA-controller handle initialization configuration. */
typedef struct {
  const esy_dma_inner_config_t *channels; /**< Routes in start-mask index order. */
  uint32_t channel_count; /**< Number of distinct physical DMA resources. */
  esy_dma_device_transition_fn_t transition;
  void *transition_arg;
} esy_dma_controller_config_t;

/**
 * @brief Initialize a zero-initialized caller-owned DMA handle.
 *
 * This does not register PM policy. Every routing pair is validated by the
 * backend and aliases of one physical resource are rejected. Request channel
 * selectors may repeat when their streams differ. Failure preserves the handle.
 * Keep the routing array immutable and alive until deinit succeeds. Independent
 * handles may describe overlapping routes; prepare arbitrates their ownership.
 */
esy_err_t esy_hal_dma_init(esy_dma_t *dma,
                                const esy_dma_controller_config_t *config);

/**
 * @brief Deinitialize an idle, unregistered DMA handle.
 * Other controllers' activity on overlapping routes does not prevent deinit;
 * this operation neither stops hardware nor releases another owner's claim.
 */
esy_err_t esy_hal_dma_deinit(esy_dma_t *dma);

/**
 * @brief Fill a DMA config structure with platform-recommended defaults.
 *
 * Writes safe default values into @p config. The caller may then override the
 * fields required by the current transfer (addresses, count, dreq, channel, etc.).
 *
 * @param[out] config DMA config structure to initialize. Must not be NULL.
 */
extern void esy_hal_dma_default_cfg(esy_dma_config_t *config);

/**
 * @brief Prepare (configure) one DMA transfer without starting it.
 *
 * This is typically used to configure multiple channels first, then start them
 * together (e.g., synchronized acquisition pipelines). A physical resource
 * already prepared or retained by another controller/peripheral returns BUSY
 * without changing that request's buffers, callbacks or configuration.
 *
 * @param[in,out] dma Caller-owned initialized DMA-controller handle.
 * @param[in,out] config DMA transfer configuration. Must not be NULL.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, resource busy, not supported, etc.).
 */
extern esy_err_t esy_hal_dma_prepare(esy_dma_t *dma,
                                          esy_dma_config_t *config);

/**
 * @brief Start prepared DMA routes by configured-route bitmask.
 *
 * Bit N selects channels[N] from the controller configuration. All selected
 * routes must have completed @ref esy_hal_dma_prepare. The backend validates
 * their physical mask before starting anything; failure starts no resource.
 * For routes {dreq=15, channel=4} and {dreq=10, channel=4}, masks 1 and 2 select
 * the first and second route, while mask 3 starts both distinct streams.
 *
 * @param[in,out] dma Caller-owned initialized DMA-controller handle.
 * @param[in] route_mask Bit N selects configured route N.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
extern esy_err_t esy_hal_dma_start(esy_dma_t *dma,
                                        uint32_t route_mask);

/**
 * @brief Convenience: prepare and immediately start a single DMA transfer.
 *
 * Equivalent to:
 * - @c esy_hal_dma_prepare(dma, transfer)
 * - @c esy_hal_dma_start(dma, 1u << configured_route_index)
 *
 * The channel used is determined by fields inside @p transfer.
 *
 * @param[in,out] dma Caller-owned initialized DMA-controller handle.
 * @param[in,out] transfer DMA transfer configuration. Must not be NULL.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
extern esy_err_t esy_hal_dma_start_single(esy_dma_t *dma,
                                               esy_dma_config_t *transfer);

/**
 * @brief Abort an ongoing DMA transfer on the specified channel.
 *
 * An inactive route in this controller is an idempotent no-op, including when
 * another controller/peripheral currently owns the same physical resource.
 * Success retires this controller's hardware/callback access and releases its
 * physical claim. Failure retains the request and claim for retry.
 *
 * @param[in,out] dma Initialized caller-owned controller.
 * @param[in] dreq    DMA request source identifier (platform-defined).
 * @param[in] channel DMA channel index.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
extern esy_err_t esy_hal_dma_abort(esy_dma_t *dma, uint32_t dreq,
                                        uint8_t channel);

/**
 * @brief Retire pending DMA events and deliver callbacks from the owner task.
 *
 * Visits at most channel_count routes. Terminal error takes priority over full
 * completion; hardware and backend callback access retire before notification.
 * Retirement may wait for SDK hardware abort and a peer-core callback. Only the
 * route count is bounded: provider and user callback time contributes to poll,
 * and this interface promises no wall-clock execution limit. A failed route
 * retains its events/storage/lease for retry while independent routes progress.
 * Half and circular-cycle events coalesce between polls and retain ownership.
 * Call even when hardware is idle; busy remains true until terminal retirement.
 * No callback may reenter this controller's lifecycle, poll or transfer APIs.
 *
 * @param[in,out] dma Initialized caller-owned controller.
 * @retval ESY_OK All currently observed events handled or no event pending.
 * @retval ESY_ERR_INVALID_ARG Invalid controller.
 * @retval <0 A retirement failed; keep storage alive and retry poll/abort.
 */
esy_err_t esy_hal_dma_poll(esy_dma_t *dma);

/**
 * @brief Query whether a DMA channel is busy.
 *
 * @param[in] dreq    DMA request source identifier (platform-defined).
 * @param[in] channel DMA channel index.
 *
 * @retval true  Hardware is active or terminal retirement is still pending.
 * @retval false The channel is idle or invalid.
 */
extern bool esy_hal_dma_is_busy(const esy_dma_t *dma, uint32_t dreq,
                                     uint8_t channel);

/**
 * @brief Bind a DMA IRQ line to a specific CPU core (multi-core platforms).
 *
 * On platforms that support routing DMA interrupts to different cores, this API
 * selects the core affinity for a given DMA IRQ index.
 * On Pico, select before first DMA prepare (default core 0) and perform that
 * first prepare on the selected core. A different calling core returns STATE
 * before native IRQ/channel changes. Later prepares may run on either core,
 * but rebinding an assembled IRQ returns BUSY. Do not independently enable the
 * same native IRQ on the other core; this API does not dispatch work remotely.
 * All Pico DMA IRQs assigned to one core must use the same board-provided
 * priority. First assembly reserves it until reset; conflicting prepares
 * return BUSY and retain no new physical claim. Do not change native priorities.
 *
 * @param[in] irq_index DMA IRQ index (typically 0 or 1; platform-defined).
 * @param[in] core_id   CPU core id (typically 0 or 1; platform-defined).
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (not supported, invalid args, etc.).
 */
extern esy_err_t esy_hal_dma_bind_irq_to_core(esy_dma_t *dma,
                                                   uint8_t irq_index,
                                                   uint8_t core_id);

/**
 * @brief Set DMA read (source) address.
 * @note Requires a prepared idle route. Failure preserves route state.
 *
 * @param[in] dreq        DMA request source identifier (platform-defined).
 * @param[in] channel     DMA channel index.
 * @param[in] addr        Source/read start address.
 * @param[in] trigger_now If true, trigger a previously prepared idle channel.
 */
extern esy_err_t esy_hal_dma_set_read_addr(
    esy_dma_t *dma, uint32_t dreq, uint8_t channel, const void *addr,
    bool trigger_now);

/**
 * @brief Set DMA write (destination) address.
 * @note Requires a prepared idle route. Failure preserves route state.
 *
 * @param[in] dreq        DMA request source identifier (platform-defined).
 * @param[in] channel     DMA channel index.
 * @param[in] addr        Destination/write start address.
 * @param[in] trigger_now If true, trigger a previously prepared idle channel.
 */
extern esy_err_t esy_hal_dma_set_write_addr(
    esy_dma_t *dma, uint32_t dreq, uint8_t channel, void *addr,
    bool trigger_now);

/**
 * @brief Set DMA transfer count (number of transfer units).
 *
 * The unit width is determined by the configured transfer size (e.g., 8/16/32-bit).
 * Requires a prepared idle route. Failure preserves route state/count.
 *
 * @param[in] dreq        DMA request source identifier (platform-defined).
 * @param[in] channel     DMA channel index.
 * @param[in] count       Number of transfer units to move.
 * @param[in] trigger_now If true, trigger a previously prepared idle channel.
 */
extern esy_err_t esy_hal_dma_set_trans_count(
    esy_dma_t *dma, uint32_t dreq, uint8_t channel, size_t count,
    bool trigger_now);

/**
 * @brief Get remaining DMA transfer count.
 *
 * The returned count is expressed in DMA transfer units (not bytes), where
 * the unit size depends on the configured DMA transfer width.
 *
 * @param[in]  dreq           DMA request source identifier (platform-defined).
 * @param[in]  channel        DMA channel index.
 * @param[out] out_remaining  Remaining transfer count (units).
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
extern esy_err_t esy_hal_dma_get_trans_count_remaining(
    const esy_dma_t *dma, uint32_t dreq, uint8_t channel,
    size_t *out_remaining);

/**
 * @brief Configure channel chaining.
 *
 * The generic HAL currently returns ESY_ERR_NOT_SUPPORTED on every port: a
 * transaction-wide lifecycle for automatically triggered destinations is not
 * provided. Unknown owner/routes return INVALID_ARG first; unprepared/busy
 * routes return STATE/BUSY. Valid prepared routes retain ownership unchanged.
 * The backend-only Pico primitive does not supply this generic lifecycle.
 *
 * @param[in] dreq          DMA request source identifier (platform-defined).
 * @param[in] channel       Current DMA channel index.
 * @param[in] next_route_index Index in the controller's configured routing list.
 */
extern esy_err_t esy_hal_dma_chain_to(esy_dma_t *dma, uint32_t dreq,
                                           uint8_t channel,
                                           uint8_t next_route_index);

/**
 * @brief DMA interrupt handler entry.
 *
 * The platform IRQ vector should call this function when the DMA interrupt
 * associated with @p dreq/@p channel fires, so the HAL can dispatch
 * and process DMA completion/error events.
 *
 * @param[in] dreq    DMA request source identifier (platform-defined).
 * @param[in] channel DMA channel index.
 */
extern void esy_hal_dma_handle_irq(uint32_t dreq, uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_DMA_H */
