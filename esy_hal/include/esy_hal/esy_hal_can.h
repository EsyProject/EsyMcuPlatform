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
 * @brief Caller-driven CAN with provider-specific native resources.
 * All operations, including operations on different controllers, require one
 * externally serialized non-ISR owner. The consumer schedules receive/status
 * calls often enough for its traffic and FIFO/queue capacity. Send/receive
 * perform at most one native frame operation; provider execution contributes
 * latency. STM32F4 requires no OS, interrupts, software queues or allocation,
 * and bounds control handshakes by register observations, not elapsed time.
 *
 * Classic ESP32 uses ESP-IDF 5.3.4 TWAI and FreeRTOS. Init/deinit install/remove
 * its SDK-owned driver, RX queue, IRQ and possible PM resources; the provider
 * can allocate and block internally during these lifecycle calls. No platform
 * task or application callback is created. TX has no software queue. All data
 * calls use zero native wait, with no native creation/destruction. SDK ISR/lock
 * execution still contributes latency; this is not a hard-time guarantee.
 * One native RX entry and at most ESY_CAN_ESP32_FILTER_CAPACITY software rules
 * are processed per receive. All native traffic occupies RX queue slots before
 * software filtering, including frames that will be rejected. Never use the
 * same native controller or pins through another SDK owner while initialized.
 */

/**
 * @brief Fill every configuration field; NULL is ignored.
 * Selects normal mode, Classical CAN, automatic retransmission, timing
 * {prescaler=1, segment1=13, segment2=2, sjw=1}, and the default poll limit.
 * phys_index and port pins are unselected; native RX capacity defaults to
 * ESY_CAN_DEFAULT_RX_QUEUE_CAPACITY. Override timing for the actual input clock.
 * ESP32 selects phys_index=1, requires explicit distinct port pins, and requires
 * a supported even prescaler rather than the generic default of one.
 * @param[out] config Configuration storage, or NULL.
 */
void esy_hal_can_default_cfg(esy_can_config_t *config);

/**
 * @brief Initialize a zero handle into stopped state rejecting all RX frames.
 * @param[in,out] can Caller-owned {0} handle; keep its address stable.
 * @param[in] config Borrowed only for this call; board pin ownership is caller-owned.
 * @retval ESY_OK Initialized and stopped; set filters before start.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer, timing, mode or poll budget.
 * @retval ESY_ERR_STATE Repeated init; live handle is preserved.
 * @retval ESY_ERR_BUSY Native resource domain already owned.
 * @retval ESY_ERR_NOT_SUPPORTED Controller or requested mode is unavailable.
 * @retval ESY_ERR_TIMEOUT Hardware handshake failed; if ctx is retained, call
 * deinit and retain board resources and handle storage until cleanup succeeds.
 * Only deinit is legal after failed init; status/stop require successful init.
 * ESP32 returns NO_MEM on native allocation failure. Reported native install
 * failures release their partial resources and leave the zero handle reusable.
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
 * ESP32 clears its native RX queue on start. Drain any required pre-stop RX
 * first. After an ESP32 bus-off stop, start returns STATE until deinit/init;
 * no automatic bus-off recovery or native-driver reinstallation is performed.
 * @param[in,out] can Initialized stopped handle.
 * @return ESY_OK on start, STATE for wrong lifecycle, TIMEOUT for a failed
 * bounded handshake, INVALID_ARG for NULL, or a provider error.
 */
esy_err_t esy_hal_can_start(esy_can_t *can);

/**
 * @brief Abort pending TX and quiesce; repeat on stopped handles is successful.
 * Pending RX remains drainable while stopped. ESP32 start clears its SDK RX
 * queue; STM32F4 retains its FIFO until receive/deinit. ESP32 bus-off is already
 * electrically quiet: stop succeeds without requesting recovery, keeps its
 * native driver owned, and requires deinit/init before any subsequent start.
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
 * @brief Copy and release at most one native RX FIFO/queue entry; never wait.
 * Classical wire DLC 9..15 is normalized to eight payload/request bytes.
 * ESP32 discards a software-filter mismatch and returns BUSY after this one
 * entry; an accepted frame may remain behind it. Continue bounded owner turns
 * according to traffic rather than treating BUSY as proof the raw queue is empty.
 * @param[in,out] can Initialized handle; stopped handles may drain queued RX.
 * @param[out] frame Written only on success, including zeroed unused data bytes.
 * @return OK, BUSY when no accepted frame was produced, STATE for invalid lifecycle, INVALID_ARG for
 * NULL, or IO for a malformed hardware frame (entry is discarded).
 */
esy_err_t esy_hal_can_receive(esy_can_t *can, esy_can_frame_t *frame);

/**
 * @brief Replace the complete filter set while stopped; zero rules reject all.
 * Invalid rules are rejected before mutation. A provider failure may require
 * reapplying the complete set while stopped. STM32F4 accepts at most 28 rules;
 * ESP32 copies at most ESY_CAN_ESP32_FILTER_CAPACITY rules into fixed storage
 * and matches format/ID/RTR in receive. No caller filter pointer is retained.
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
 * ESP32 rx_pending counts SDK queue entries before software filtering;
 * rx_overrun also includes SDK queue loss and errata-workaround loss. Its native
 * API does not expose the last bus error, so last_error is zero. Listen-only
 * native error counters are unavailable and are reported as zero by the SDK.
 * @return OK, INVALID_ARG, STATE or a provider error.
 */
esy_err_t esy_hal_can_get_status(esy_can_t *can, esy_can_status_t *status);

/**
 * @brief Request bus-off recovery, aborting outstanding transmit requests.
 * A successful request does not promise recovery completion. Poll get_status
 * until ERROR_ACTIVE/PASSIVE before submitting again. Repeated requests during
 * recovery return BUSY and must not restart it. The CAN idle-bit sequence is
 * hardware-driven; no software timer can reactivate a completed request.
 * ESP32 does not implement recovery and returns NOT_SUPPORTED. After bus-off,
 * stop/deinit and initialize again; query caps before selecting recovery policy.
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
