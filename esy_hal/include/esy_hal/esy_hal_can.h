/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_CAN_H
#define ESY_HAL_CAN_H

#include "esy_common/defs/peripheral/esy_can_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_hal_can.h
 * @brief Caller-driven CAN without OS, callbacks, software queues or allocation.
 * All operations, including operations on different controllers, require one
 * externally serialized non-ISR owner. The consumer schedules receive/status
 * calls often enough for its traffic and FIFO capacity. Send/receive perform at
 * most one frame operation; provider register access still contributes latency.
 * Control calls have finite observation budgets, not wall-clock deadlines.
 */

/**
 * @brief Fill every configuration field; NULL is ignored.
 * Selects normal mode, Classical CAN, automatic retransmission, timing
 * {prescaler=1, segment1=13, segment2=2, sjw=1}, and the default poll limit.
 * phys_index is zero (unselected). Override timing for the actual input clock.
 * @param[out] config Configuration storage, or NULL.
 */
void esy_hal_can_default_cfg(esy_can_config_t *config);

/**
 * @brief Initialize a zero handle into stopped state with all filters disabled.
 * @param[in,out] can Caller-owned {0} handle; keep its address stable.
 * @param[in] config Borrowed only for this call; board routing is caller-owned.
 * @retval ESY_OK Initialized and stopped; set filters before start.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer, timing, mode or poll budget.
 * @retval ESY_ERR_STATE Repeated init; live handle is preserved.
 * @retval ESY_ERR_BUSY Native resource domain already owned.
 * @retval ESY_ERR_NOT_SUPPORTED Controller or requested mode is unavailable.
 * @retval ESY_ERR_TIMEOUT Hardware handshake failed; if ctx is retained, call
 * deinit and retain board resources and handle storage until cleanup succeeds.
 * Only deinit is legal after failed init; status/stop require successful init.
 */
esy_err_t esy_hal_can_init(esy_can_t *can, const esy_can_config_t *config);

/**
 * @brief Stop, abort queued TX and release provider ownership; zero is idempotent.
 * @param[in,out] can Initialized or zero handle.
 * @return ESY_OK leaves a zero reusable handle; any failure retains ownership
 * and storage for retry. NULL returns INVALID_ARG. Pending traffic may be lost.
 */
esy_err_t esy_hal_can_deinit(esy_can_t *can);

/**
 * @brief Start a stopped controller; repeated start returns STATE.
 * @param[in,out] can Initialized stopped handle.
 * @return ESY_OK on start, STATE for wrong lifecycle, TIMEOUT for a failed
 * bounded handshake, INVALID_ARG for NULL, or a provider error.
 */
esy_err_t esy_hal_can_start(esy_can_t *can);

/**
 * @brief Abort pending TX and quiesce; repeat on stopped handles is successful.
 * RX frames already in the hardware FIFO are retained until receive/deinit;
 * consumers must drain stale frames before restarting if freshness is required.
 * @param[in,out] can Initialized handle, including faulted/recovering state.
 * @return ESY_OK when stopped, TIMEOUT with ownership retained, INVALID_ARG for
 * NULL, STATE for an uninitialized handle, or a provider error.
 */
esy_err_t esy_hal_can_stop(esy_can_t *can);

/**
 * @brief Copy one frame to a free hardware mailbox without waiting for the bus.
 * ESY_OK means accepted, not acknowledged or delivered. Query status for pending
 * requests/errors. Automatic retransmission may retain a mailbox indefinitely;
 * the owner must apply its own cancellation/stop policy. No buffer is borrowed.
 * @param[in,out] can Running handle; listen-only cannot transmit.
 * @param[in] frame Valid frame; transmit ESI must be false.
 * @return OK, BUSY when full, STATE when stopped/bus-off/recovering, INVALID_ARG
 * for invalid frame/pointers, NOT_SUPPORTED for unimplemented FD/BRS, or IO.
 */
esy_err_t esy_hal_can_send(esy_can_t *can, const esy_can_frame_t *frame);

/**
 * @brief Copy and release at most one hardware RX FIFO entry; never wait.
 * Classical wire DLC 9..15 is normalized to eight payload/request bytes.
 * @param[in,out] can Initialized handle; stopped handles may drain queued RX.
 * @param[out] frame Written only on success, including zeroed unused data bytes.
 * @return OK, BUSY when empty, STATE for invalid lifecycle, INVALID_ARG for
 * NULL, or IO for a malformed hardware frame (entry is discarded).
 */
esy_err_t esy_hal_can_receive(esy_can_t *can, esy_can_frame_t *frame);

/**
 * @brief Replace the complete filter set while stopped; zero rules reject all.
 * Invalid rules are rejected before mutation. A provider failure may require
 * reapplying the complete set while stopped. STM32F4 accepts at most 28 rules.
 * @param[in,out] can Initialized stopped handle.
 * @param[in] filters Borrowed array; NULL is valid only when count is zero.
 * @param[in] count Number of rules, bounded by provider capacity.
 * @return OK, INVALID_ARG, STATE, NOT_SUPPORTED or a provider error.
 */
esy_err_t esy_hal_can_set_filters(esy_can_t *can,
                                 const esy_can_filter_t *filters, size_t count);

/**
 * @brief Sample state/counters without clearing sticky diagnostics.
 * @param[in,out] can Initialized handle; query advances observed recovery state.
 * @param[out] status Written only on success; counters are native snapshots.
 * @return OK, INVALID_ARG, STATE or a provider error.
 */
esy_err_t esy_hal_can_get_status(esy_can_t *can, esy_can_status_t *status);

/**
 * @brief Request bus-off recovery, aborting outstanding transmit requests.
 * A successful request does not promise recovery completion. Poll get_status
 * until ERROR_ACTIVE/PASSIVE before submitting again. Repeated requests during
 * recovery return BUSY and must not restart it. The CAN idle-bit sequence is
 * hardware-driven; no software timer can reactivate a completed request.
 * @param[in,out] can Bus-off handle.
 * @return OK when requested, BUSY if already recovering, STATE if not bus-off,
 * TIMEOUT on handshake failure, INVALID_ARG for NULL, or NOT_SUPPORTED.
 */
esy_err_t esy_hal_can_recover(esy_can_t *can);

/**
 * @brief Query capabilities for an initialized handle; output changes on success.
 * @param[in] can Initialized handle.
 * @param[out] caps Public esy_can_cap_t mask; unknown bits must be ignored.
 * @return OK, INVALID_ARG, STATE or NOT_IMPLEMENTED when no query exists.
 */
esy_err_t esy_hal_can_get_caps(const esy_can_t *can, uint32_t *caps);

#ifdef __cplusplus
}
#endif
#endif /* ESY_HAL_CAN_H */
