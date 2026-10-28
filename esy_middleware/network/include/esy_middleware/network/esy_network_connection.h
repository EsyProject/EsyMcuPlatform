/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_NETWORK_CONNECTION_H
#define ESY_NETWORK_CONNECTION_H

#include "esy_middleware/network/esy_network_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Usability of an observed connection, independent of endpoint setup. */
typedef enum {
  ESY_NETWORK_CONNECTION_INACTIVE = 0, /**< No usable current peer connection. */
  ESY_NETWORK_CONNECTION_OPEN, /**< Receive and send are available. */
  ESY_NETWORK_CONNECTION_EOF /**< Receive half-closed; buffered RX and send remain valid. */
} esy_network_connection_state_t;

/** @brief One owner-context observation; capacities are not reservations. */
typedef struct {
  esy_network_connection_state_t state; /**< EOF retains the peer until owner close. */
  uint64_t session_id; /**< Nonzero and distinct for each connection until provider deinit. */
  uint32_t send_capacity; /**< Current single-write capacity, not a reservation. */
  uint32_t unacked_bytes; /**< Accepted TX bytes not acknowledged by the peer. */
  uint32_t rx_available; /**< Bytes readable without progressing provider input. */
} esy_network_connection_status_t;

/**
 * @brief Borrowed byte stream extended for reliable framed consumers.
 *
 * This descriptor is copied by value and owns no endpoint or storage. Its
 * provider and stream.context outlive every borrower. All callbacks use that
 * same stream.context on one serialized non-ISR owner, following the base stream
 * execution and buffer-copy contract. No second context or implicit progress is
 * provided. Both extension callbacks are required by consumers such as ProtoBin.
 *
 * send_all accepts the entire nonzero request, returns zero for backpressure,
 * or a negative status without accepting any bytes. A successful return equals
 * the requested size. Accepted bytes are copied/retired before returning.
 * get_status observes connection/session and peer-ACK evidence without input or
 * timer progress; output is written only on success. unacked_bytes must never
 * report mere local acceptance as peer acknowledgement. INACTIVE or a changed
 * session invalidates all prior-session TX evidence. EOF permits buffered RX
 * drain and sending, including protocol acknowledgements, until explicit close.
 * Connection establishment, listening, cancellation and reconnect policy stay
 * with the endpoint owner and are not represented by these three states.
 */
typedef struct {
  esy_network_stream_t stream; /**< Basic prefix send/receive and explicit progress. */
  int32_t (*send_all)(void *context, const void *data, size_t size); /**< Whole write. */
  esy_err_t (*get_status)(void *context, esy_network_connection_status_t *status); /**< Observe. */
} esy_network_connection_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_NETWORK_CONNECTION_H */
