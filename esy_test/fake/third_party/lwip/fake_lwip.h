/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TEST_FAKE_THIRD_PARTY_LWIP_FAKE_LWIP_H
#define ESY_TEST_FAKE_THIRD_PARTY_LWIP_FAKE_LWIP_H
#include "lwip/tcp.h"
/** Deterministic single-threaded lwIP raw-API fake. No production SDK required.
 */
void fake_lwip_reset(void);
void fake_lwip_fail(err_t allocation, err_t connect, err_t close, err_t output);
void fake_lwip_set_write_result(err_t result);
/** @brief Fail listen allocation without freeing the original PCB. */
void fake_lwip_set_listen_result(err_t result);
struct tcp_pcb* fake_lwip_last_pcb(void);
struct tcp_pcb* fake_lwip_accept(void);
err_t fake_lwip_connect(void);
/**
 * @brief Offer copied bytes to the receive callback using a static pbuf.
 * ERR_OK/ERR_ABRT relinquish the fake's ownership to the callback. Other
 * callback errors retain the same pbuf in pcb->refused_data for redelivery.
 * A refused pbuf blocks new offers until redelivery succeeds or the PCB is
 * retired. A successful close, abort or error releases provider-owned refused
 * data; accepted pbufs remain the application's responsibility.
 * @param pcb Live connection with a receive callback.
 * @param data Bytes copied before the callback; may be NULL only for zero len.
 * @param len Byte count, at most the fake pbuf's 512-byte payload capacity.
 * @return Callback result, or ERR_MEM for invalid input/resource exhaustion.
 */
err_t fake_lwip_receive(struct tcp_pcb* pcb, const uint8_t* data, u16_t len);
/**
 * @brief Retry the exact refused pbuf without copying or allocating another.
 * @param pcb Live connection with a refused pbuf and receive callback.
 * @return Callback result, or ERR_MEM when no pending delivery is possible.
 */
err_t fake_lwip_redeliver(struct tcp_pcb* pcb);
err_t fake_lwip_fin(struct tcp_pcb* pcb);
void fake_lwip_ack(struct tcp_pcb* pcb);
void fake_lwip_error(struct tcp_pcb* pcb);
unsigned fake_lwip_live_pcbs(void);
unsigned fake_lwip_aborts(void);
unsigned fake_lwip_writes(void);
unsigned fake_lwip_live_pbufs(void);
/** @brief Foreign or already retired pbuf free attempts since reset. */
unsigned fake_lwip_invalid_pbuf_frees(void);

#endif
