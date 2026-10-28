/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_BIN_TCP_CHANNEL_H
/** @brief Fixed storage bound or caller-owned storage declaration/binding. */
#define ESY_PROTO_BIN_TCP_CHANNEL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/network/esy_network_connection.h"
#include "esy_middleware/protocol/esy_proto_bin_session.h"

/**
 * @file
 * @brief ProtoBin framing over a borrowed network connection extension.
 *
 * The owner initializes/connects/listens/reconnects/closes its network provider
 * independently. This channel owns only protocol state and buffers; no native
 * network, Wi-Fi, task or clock provider is selected here. All calls and stream
 * callbacks share one serialized task/core. No API is ISR-safe. The connection
 * extension supplies whole writes, session IDs and peer-ACK status separately
 * from its basic byte stream. All callbacks share connection.stream.context.
 * One connection is exclusively borrowed: other consumers must not read or send
 * on its stream. Endpoint setup/listening states stay with the provider owner;
 * this channel observes only INACTIVE, OPEN and receive-half-closed EOF.
 * Protocol TX completion waits for transport peer ACK; a new/disconnected session
 * drops old-session protocol transactions with IO_ERROR before reuse. Peer EOF
 * is a receive half-close: polling drains buffered RX and transport peer ACKs
 * over as many bounded calls as needed before resetting remaining transactions.
 * Existing queued TX may also be submitted during this drain. The owner bounds
 * drain time and may explicitly close on timeout, then polls once to reset the
 * terminated session. Polling
 * performs one optional stream progress step and the transport's bounded work.
 * Provider/callback latency contributes to its execution time.
 *
 * Handles require {0} before first init. The provider, callbacks and buffers must
 * remain alive until successful deinit. Serialize teardown with all channel
 * and sender operations. Deinit stops this borrower; the provider can remain
 * open because sends copied their bytes. The channel never closes it. Protocol callbacks may
 * reserve/commit/cancel TX, but must not recursively poll/reset/init/deinit.
 * Session-reset callbacks must not enqueue TX. Timeout units and wrap constraints
 * remain those of esy_proto_bin_session.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Caller-owned channel capacity; rebuild consumers if changed. */
#define ESY_PROTO_BIN_TCP_CHANNEL_CTX_BYTES 1024u

/** @brief Opaque caller-owned channel, initially {0}. */
typedef struct {
  alignas(ESY_PROTO_BIN_SESSION_ALIGNMENT)
      ESY_ALIGNED_STORAGE(priv, ESY_PROTO_BIN_TCP_CHANNEL_CTX_BYTES);
} esy_proto_bin_tcp_channel_t;

/** @brief Caller-owned buffers retained until successful channel
 * deinitialization. */
typedef struct {
  uint8_t *tx_queue_storage;
  size_t tx_queue_storage_size;
  uint8_t *tx_work_buf;
  size_t tx_work_buf_size;
  uint8_t *rx_work_buf;
  size_t rx_work_buf_size;
} esy_proto_bin_tcp_channel_static_resources_t;

/** @brief Fixed storage bound or caller-owned storage declaration/binding. */
#define ESY_PROTO_BIN_TCP_CHANNEL_STATIC_STORAGE(                                \
    name, tx_queue_block_count, tx_payload_max, rx_payload_max)               \
  struct {                                                                    \
    uint8_t tx_queue_storage[(tx_queue_block_count) *                         \
                             ((tx_payload_max) +                              \
                              ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES)];        \
    uint8_t tx_work_buf[(tx_payload_max) + ESY_PROTO_BIN_OVERHEAD_BYTES];     \
    uint8_t rx_work_buf[(rx_payload_max) + ESY_PROTO_BIN_OVERHEAD_BYTES];     \
  } name

/** @brief Fixed storage bound or caller-owned storage declaration/binding. */
#define ESY_PROTO_BIN_TCP_CHANNEL_BIND_STATIC_RESOURCES(resources, storage)       \
  do {                                                                        \
    (resources)->tx_queue_storage = (storage).tx_queue_storage;               \
    (resources)->tx_queue_storage_size = sizeof((storage).tx_queue_storage);  \
    (resources)->tx_work_buf = (storage).tx_work_buf;                         \
    (resources)->tx_work_buf_size = sizeof((storage).tx_work_buf);            \
    (resources)->rx_work_buf = (storage).rx_work_buf;                         \
    (resources)->rx_work_buf_size = sizeof((storage).rx_work_buf);            \
  } while (0)

/** @brief Connection settings and borrowed callback/storage references. */
typedef struct {
  esy_network_connection_t connection; /**< Exclusive borrowed connection and callbacks. */
  uint32_t tx_payload_max;
  uint32_t rx_payload_max;
  uint32_t tx_completion_timeout_ms; /**< Stream acceptance to peer transport ACK. */
  uint32_t acknowledgement_timeout_ms; /**< Peer transport ACK to ProtoBin ACK. */
  uint32_t retries;
  uint32_t tx_queue_block_count;
  esy_proto_bin_tp_data_cb_t data_cb;
  esy_proto_bin_tp_event_cb_t event_cb;
  void *callback_user_data;
  const esy_proto_bin_tcp_channel_static_resources_t *static_resources;
} esy_proto_bin_tcp_channel_config_t;

/** @brief Set configuration defaults; NULL is ignored. */
void esy_proto_bin_tcp_channel_default_cfg(esy_proto_bin_tcp_channel_config_t *config);

/** @brief Initialize fresh {0} storage and borrow the connection and buffers.
 * Buffers, config and resource descriptors must be pairwise disjoint from each
 * other and the whole channel handle; wrapping ranges are rejected before
 * provider I/O or handle modification.
 * Returns INVALID_ARG or STATE without borrowing on failure. Repeated init
 * preserves the existing channel. Requires connection.stream.recv, send_all and
 * get_status; base prefix-send and progress are optional for this borrower.
 * No network lifecycle operation occurs. */
esy_err_t esy_proto_bin_tcp_channel_init(
    esy_proto_bin_tcp_channel_t *channel,
    const esy_proto_bin_tcp_channel_config_t *config);

/** @brief Progress the borrowed provider once, observe its session and poll
 * protocol state. Returns STATE when inactive or the provider progress/status
 * error. Network failures reset old-session protocol state on observation. */
esy_err_t esy_proto_bin_tcp_channel_poll(esy_proto_bin_tcp_channel_t *channel);

/** @brief True after observing peer EOF, draining buffered RX/peer ACKs and
 * resetting that session. Query on the serialized owner; NULL/inactive is false.
 * The endpoint owner may then close_peer/close. A permanently blocked TCP send
 * or missing peer ACK needs an owner-imposed drain deadline and explicit close. */
bool esy_proto_bin_tcp_channel_is_peer_drained(const esy_proto_bin_tcp_channel_t *channel);

/** @brief Stop this borrower and release protocol state without completion events.
 * Returns OK, INVALID_ARG for NULL, STATE when inactive, or BUSY during poll.
 * Failure retains storage. Pending peer TCP ACKs do not retain session buffers:
 * stream sends copied their bytes, and network resources remain provider-owned. */
esy_err_t esy_proto_bin_tcp_channel_deinit(esy_proto_bin_tcp_channel_t *channel);

/** @brief Borrow the active session for payload exchange and queries, or NULL.
 * The channel alone polls and deinitializes it. For explicit peer retirement,
 * the same serialized channel owner may reset the session after stopping channel
 * polling and payload submissions. Stream sends already copied their buffers;
 * reset reports discarded transactions without claiming peer acknowledgment.
 * Do not resume the retired peer; finish its channel/provider cleanup first. */
esy_proto_bin_session_t *esy_proto_bin_tcp_channel_get_session(
    esy_proto_bin_tcp_channel_t *channel);

#ifdef __cplusplus
}
#endif

#endif /* ESY_PROTO_BIN_TCP_CHANNEL_H */
