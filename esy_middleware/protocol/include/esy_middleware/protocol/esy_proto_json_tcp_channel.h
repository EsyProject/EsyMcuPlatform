/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_JSON_TCP_CHANNEL_H
#define ESY_PROTO_JSON_TCP_CHANNEL_H

/**
 * @file esy_proto_json_tcp_channel.h
 * @brief Caller-driven, length-prefixed JSON over an existing TCP connection.
 *
 * Wire format: four octets of unsigned big-endian JSON byte length, followed
 * by exactly that many UTF-8 JSON bytes. Length excludes the prefix and any
 * C terminator; zero is invalid. Any complete JSON root is supported, including
 * formatted documents with newlines. This is a documented platform framing
 * convention, not HTTP, WebSocket, NDJSON or an application request schema.
 *
 * One serialized non-ISR owner calls all APIs and provider callbacks. The
 * connection descriptor is copied; its provider/context, buffers and callback
 * context remain borrowed until successful deinit. The provider must already
 * expose OPEN or EOF and a nonzero session_id. stream.send, stream.recv and
 * get_status are required, progress is optional, send_all is unused. The channel
 * exclusively borrows this connection; nobody else may send/receive its bytes.
 * Connection establishment, listener/client policy, close and reconnect remain
 * with the endpoint owner. Plain TCP or a compatible TLS connection may be used.
 *
 * poll performs at most one progress call, one prefix-send of at most
 * tx_byte_budget bytes, and receives at most rx_byte_budget bytes while
 * delivering at most one message. A successful receive consumes at least one
 * byte, so receive callback attempts are bounded by rx_byte_budget+1. Status
 * observations add a bounded number of synchronous provider calls. Parsing and
 * callback time contribute to poll latency; this is not a hard-real-time bound.
 * There are no tasks, queues, timers, allocations, HAL calls, retries or hidden
 * deadlines. Owners must bound TX stalls, incomplete RX and EOF/ACK drain using
 * their own timebase and stop policy; poll count is not elapsed time.
 *
 * send validates and copies one complete message into one staging slot. No I/O
 * send occurs until poll. The slot stays BUSY until all prefix/body bytes have
 * been accepted by stream.send. Acceptance is NOT TCP acknowledgement or remote
 * application completion. Partial writes resume exactly once at their offset.
 * The message callback may send a reply; recursive poll/init/deinit returns
 * BUSY. Provider callbacks must not reenter the channel. get_status is a copied
 * local snapshot and may be queried in a message callback.
 *
 * Framing/JSON errors, over-limit RX, fatal provider errors and lost/changed
 * sessions are sticky. Admission closes and poll returns the first error until
 * deinit. There is no stream resynchronization or cross-session replay. EOF
 * drains buffered complete frames and permits replies; a truncated final frame
 * fails. peer_drained becomes true only after EOF, no buffered/partial RX, no
 * staged TX and a fresh same-session unacked_bytes==0 observation. This is local
 * channel quiescence, not completion of owner-managed response work. An explicit
 * successful send may stage more data on the still-live EOF session and clears
 * peer_drained. The owner also checks its own pending replies before close.
 * Deinit cancels only local work and never closes the provider
 * or revokes already accepted bytes. After failure or abandoning partial work,
 * the owner must retire residual peer traffic and establish a fresh session
 * before reusing storage; clearing local state does not repair a TCP stream.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/protocol/esy_proto_json.h"
#include "esy_middleware/network/esy_network_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Length-prefix bytes; the length field is unsigned big-endian. */
#define ESY_PROTO_JSON_TCP_HEADER_BYTES 4u
/** @brief Default per-poll RX and TX byte budgets when configured as zero. */
#define ESY_PROTO_JSON_TCP_DEFAULT_BYTE_BUDGET 256u
/** @brief Caller-owned handle capacity, not a stable ABI. */
#define ESY_PROTO_JSON_TCP_CHANNEL_STORAGE_BYTES 384u

/**
 * @brief Opaque caller-owned channel, requiring {0} before first init.
 * @note Do not copy, move or modify a live handle. Init failure preserves the
 * handle; deinit restores its zero state and permits init again.
 */
typedef struct {
    union {
        max_align_t alignment; /**< Private alignment. */
        uint8_t bytes[ESY_PROTO_JSON_TCP_CHANNEL_STORAGE_BYTES]; /**< Private state. */
    } storage; /**< Private in-place storage. */
} esy_proto_json_tcp_channel_t;

/** @brief Caller-owned buffers, borrowed exclusively until successful deinit. */
typedef struct {
    uint8_t *rx_buffer; /**< Complete incoming JSON bytes; no terminator needed. */
    size_t rx_buffer_size; /**< Positive message limit, at most UINT32_MAX. */
    uint8_t *tx_buffer; /**< One outgoing frame, including its four-byte prefix. */
    size_t tx_buffer_size; /**< At least header bytes + one JSON byte. */
} esy_proto_json_tcp_channel_static_resources_t;

/**
 * @brief Receive one validated root on the polling owner, never in ISR.
 * @param[in,out] channel Source channel; send/get_status are permitted here.
 * @param[in] root Borrowed view, including all descendant views, valid only
 * until this callback returns. Copy required content before return.
 * @param[in] user_ctx Borrowed context supplied in config; may be NULL.
 * @note No lifecycle, recursive poll or provider close/reconnect is allowed.
 * Callback runtime belongs to poll's execution budget.
 */
typedef void (*esy_proto_json_tcp_message_cb)(esy_proto_json_tcp_channel_t *channel,
                                              const esy_json_value_t *root,
                                              void *user_ctx);

/** @brief Configuration copied by init; referenced objects remain borrowed. */
typedef struct {
    esy_network_connection_t connection; /**< Exclusive connected peer descriptor. */
    const esy_proto_json_tcp_channel_static_resources_t *static_resources; /**< Required buffers. */
    esy_proto_json_tcp_message_cb on_message; /**< Required synchronous receiver. */
    void *user_ctx; /**< Optional borrowed callback context. */
    size_t rx_byte_budget; /**< Per-poll RX bytes; zero selects the named default. */
    size_t tx_byte_budget; /**< Per-poll TX bytes; zero selects the named default. */
} esy_proto_json_tcp_channel_config_t;

/** @brief Local observation, written only on successful get_status. */
typedef struct {
    uint64_t session_id; /**< Peer identity bound at successful init. */
    size_t rx_received; /**< Partial current JSON body bytes, excluding header. */
    uint32_t rx_expected; /**< Current declared body length, or zero during header. */
    size_t tx_remaining; /**< Staged prefix+body bytes not yet accepted by provider. */
    esy_err_t error; /**< First sticky failure, or ESY_OK. */
    bool tx_pending; /**< One staging slot is occupied. */
    bool peer_drained; /**< Observed local EOF/ACK drain; cleared by successful send. */
} esy_proto_json_tcp_channel_status_t;

/**
 * @brief Borrow one existing peer session and initialize channel state.
 * @param[in,out] channel Zero-initialized or successfully deinitialized handle.
 * @param[in] config Required configuration with provider, receiver and buffers.
 * @return OK, INVALID_ARG for pointers/resources/overlap, STATE for repeated init
 * or inactive/zero peer identity, BUSY for reentry/provider backpressure, or a
 * provider error. No resource is retained and no callback runs on failure.
 * @note Handle, config, resource descriptor and both complete buffer ranges must
 * be pairwise disjoint and nonwrapping. Init validates them before provider I/O.
 * The provider is observed but never progressed or otherwise modified here.
 */
esy_err_t esy_proto_json_tcp_channel_init(esy_proto_json_tcp_channel_t *channel,
                                         const esy_proto_json_tcp_channel_config_t *config);

/**
 * @brief Submit one complete JSON document by validation and copy.
 * @param[in,out] channel Initialized channel, including from its message callback.
 * @param[in] json Required complete document; no terminating NUL is transmitted.
 * @param[in] length Positive document length, at most UINT32_MAX and TX capacity.
 * @return OK for local staging, BUSY for occupied slot/reentry/provider status,
 * NO_MEM for capacity/depth, INVALID_ARG, FAIL for JSON syntax, STATE for an
 * inactive/changed session, or a sticky/provider error.
 * @note Caller data may be reused immediately after return. It may borrow the
 * current callback's RX bytes, but must not overlap channel or TX storage.
 * Caller input errors/invalid JSON/capacity failures do not poison the channel.
 * No provider input progress or send occurs; get_status is observed once.
 * Successful staging clears a prior peer_drained observation, allowing an
 * owner-delayed response on the same live EOF session before endpoint close.
 */
esy_err_t esy_proto_json_tcp_channel_send(esy_proto_json_tcp_channel_t *channel,
                                         const char *json, size_t length);

/**
 * @brief Perform one bounded owner slice under the file-level framing contract.
 * @param[in,out] channel Initialized channel.
 * @return OK (also when idle or drained), BUSY for transient provider pressure
 * or recursive entry, INVALID_ARG, STATE for inactive/changed session, or the
 * first sticky parse/framing/provider error. Zero recv is lack of data, not EOF.
 * @note Work already accepted/consumed before BUSY is never replayed. Fragment
 * state persists across calls. Stop this borrower before closing its provider.
 */
esy_err_t esy_proto_json_tcp_channel_poll(esy_proto_json_tcp_channel_t *channel);

/**
 * @brief Copy local status without provider calls or input progress.
 * @param[in] channel Initialized channel, healthy or failed.
 * @param[out] status Required output, disjoint from handle; unchanged on error.
 * @return OK, INVALID_ARG for NULL, or STATE for a zero/deinitialized handle.
 */
esy_err_t esy_proto_json_tcp_channel_get_status(const esy_proto_json_tcp_channel_t *channel,
                                               esy_proto_json_tcp_channel_status_t *status);

/**
 * @brief Cancel local work and release all borrows without touching the endpoint.
 * @param[in,out] channel Initialized or zero channel, serialized with every user.
 * @return OK including repeated deinit, INVALID_ARG for NULL, BUSY during poll
 * or callback, or STATE for invalid handle state. Failure retains all resources.
 * @note No callback runs. Provider sends copy bytes before return, so pending
 * peer ACKs do not retain these buffers. Provider/context remain owner-managed.
 */
esy_err_t esy_proto_json_tcp_channel_deinit(esy_proto_json_tcp_channel_t *channel);

#ifdef __cplusplus
}
#endif
#endif /* ESY_PROTO_JSON_TCP_CHANNEL_H */
