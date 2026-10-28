/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_JSON_USB_CHANNEL_H
#define ESY_PROTO_JSON_USB_CHANNEL_H

/**
 * @file esy_proto_json_usb_channel.h
 * @brief Caller-driven, length-prefixed JSON over an exclusive USB CDC instance.
 *
 * Wire format matches JSON TCP: a four-byte unsigned big-endian JSON byte
 * length, then one complete UTF-8 JSON document. Length excludes the prefix
 * and any C terminator; zero is invalid. All JSON roots and formatting
 * whitespace are supported. There is no additional CRC, ACK, retry or schema.
 *
 * The consumer initializes CDC with discard_on_disconnect=true, leaves its RX
 * callback NULL (or notification-only), and keeps it initialized until channel
 * deinit. This channel borrows that handle exclusively for all RX/TX; no logger
 * or other protocol may use its byte stream. CDC, channel, callbacks and USB
 * stack pumping share one serialized non-ISR owner and provider threading model.
 * poll NEVER calls esy_usb_cdc_task: the existing stack owner pumps separately,
 * before channel polling. This avoids duplicate global pumps for multiple CDC
 * channels. No task, timer, HAL/OS dependency or dynamic allocation is created.
 * Native stack scheduling/blocking remains governed by its selected provider.
 *
 * Init may bind while disconnected. send validates/copies one outgoing frame
 * only while connected; no traffic is queued for a future peer. poll accepts
 * at most one prefix write of tx_byte_budget bytes, flushes accepted TX once,
 * consumes at most rx_byte_budget bytes and dispatches at most one JSON root.
 * The JSON callback may send a reply or read local status; recursive lifecycle
 * or poll calls return BUSY. Received views expire at callback return. Parsing,
 * provider and callback runtimes contribute to latency; byte budgets are not
 * hard-real-time bounds. Owners bound incomplete RX, TX stalls and shutdown
 * using their own clock and policy; this module has no deadline/rollover state.
 *
 * Short/zero CDC operations retain progress. tx_pending ends when every frame
 * byte is copied into the CDC provider, not when the host application reads it.
 * RX continues while TX is pending; required replies rejected with BUSY must
 * remain in owner-managed bounded storage for retry. No response queue exists.
 *
 * CDC session-generation inequality discards old local RX/TX and clears a prior
 * session's sticky error. The observing poll/send returns IO once with no new
 * session I/O; a later call may use the new peer. The CDC owner discards closed
 * software FIFOs before reopening, preserving new-peer RX. The channel never
 * flushes/discards new RX. Physical disconnects must be observed by the stack
 * pump or a DTR-close callback, and bus reset must retire native endpoint state.
 * Generations are compared only for inequality, not timestamp ordering.
 *
 * Malformed JSON/framing, oversized RX or impossible backend counts latch an
 * error until an observed new CDC generation or deinit. There is no same-session
 * resynchronization. Detached/disabled-isolation CDC is a lifecycle error.
 * Closing without a generation advance is also rejected rather than replaying
 * partial bytes after reopening. Stop the channel before CDC deinit/reinit;
 * generation reuse across CDC reinit cannot identify a new lifetime safely.
 * Deinit releases only local borrows, without closing CDC, pumping the stack,
 * clearing its FIFOs or recalling bytes already submitted to a USB endpoint.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/protocol/esy_proto_json.h"
#include "esy_middleware/usb/esy_usb_cdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Four-byte unsigned big-endian JSON body length. */
#define ESY_PROTO_JSON_USB_HEADER_BYTES 4u
/** @brief Per-poll RX/TX bytes when their configured budget is zero. */
#define ESY_PROTO_JSON_USB_DEFAULT_BYTE_BUDGET 256u
/** @brief Opaque channel capacity; not a stable ABI. */
#define ESY_PROTO_JSON_USB_CHANNEL_STORAGE_BYTES 256u

/**
 * @brief Caller-owned opaque handle requiring {0} before first init.
 * @note Do not copy, move or modify a live handle. Init failure preserves it;
 * successful deinit returns it to zero and permits init again.
 */
typedef struct {
    union {
        max_align_t alignment; /**< Private alignment. */
        uint8_t bytes[ESY_PROTO_JSON_USB_CHANNEL_STORAGE_BYTES]; /**< Private state. */
    } storage; /**< Private in-place storage. */
} esy_proto_json_usb_channel_t;

/** @brief Caller-owned, disjoint buffers retained until successful deinit. */
typedef struct {
    uint8_t *rx_buffer; /**< Required complete JSON body storage, no terminator. */
    size_t rx_buffer_size; /**< Positive body limit, at most UINT32_MAX. */
    uint8_t *tx_buffer; /**< Required one-frame staging, including prefix. */
    size_t tx_buffer_size; /**< At least header bytes + one JSON byte. */
} esy_proto_json_usb_channel_static_resources_t;

/**
 * @brief Receive a validated root on the polling owner, never in ISR.
 * @param[in,out] channel Source channel; send/get_status are permitted here.
 * @param[in] root Borrowed root/descendant views valid only until callback return.
 * @param[in] user_ctx Borrowed configuration context; may be NULL.
 * @note Do not pump the stack, close/rebind CDC, or call lifecycle/poll here.
 */
typedef void (*esy_proto_json_usb_message_cb)(esy_proto_json_usb_channel_t *channel,
                                              const esy_json_value_t *root,
                                              void *user_ctx);

/** @brief Copied configuration; referenced resources and contexts are borrowed. */
typedef struct {
    esy_usb_cdc_t *cdc; /**< Required initialized session-isolated CDC borrower. */
    const esy_proto_json_usb_channel_static_resources_t *static_resources; /**< Required buffers. */
    esy_proto_json_usb_message_cb on_message; /**< Required synchronous receiver. */
    void *user_ctx; /**< Optional borrowed receiver context. */
    size_t rx_byte_budget; /**< Per-poll RX bytes; zero selects the named default. */
    size_t tx_byte_budget; /**< Per-poll TX bytes; zero selects the named default. */
} esy_proto_json_usb_channel_config_t;

/** @brief Last local observation; get_status does not query or progress CDC. */
typedef struct {
    uint32_t session_id; /**< Last observed nonzero CDC generation. */
    size_t rx_received; /**< Current partial JSON body bytes. */
    uint32_t rx_expected; /**< Declared body length, or zero while reading prefix. */
    size_t tx_remaining; /**< Locally staged bytes not accepted by the CDC FIFO. */
    esy_err_t error; /**< Current session's sticky error, or ESY_OK. */
    bool connected; /**< Last observed CDC/DTR connection state. */
    bool tx_pending; /**< One local outgoing frame is staged. */
} esy_proto_json_usb_channel_status_t;

/**
 * @brief Borrow an already initialized CDC instance and caller buffers.
 * @param[in,out] channel Zero or successfully deinitialized handle.
 * @param[in] config Required valid configuration; connection may be closed.
 * @return OK, INVALID_ARG for pointers/resources/overlap or missing session
 * isolation, STATE for repeated init/invalid handle state, or BUSY on reentry.
 * @note Handle, CDC handle, config, resource descriptor and both whole buffer
 * ranges must be pairwise disjoint and nonwrapping. Validation precedes CDC
 * queries and handle writes. Failure acquires no resources and changes nothing.
 * No callback, CDC lifecycle operation or stack pump is invoked.
 */
esy_err_t esy_proto_json_usb_channel_init(esy_proto_json_usb_channel_t *channel,
                                         const esy_proto_json_usb_channel_config_t *config);

/**
 * @brief Validate and copy one JSON document for the current connected peer.
 * @param[in,out] channel Initialized channel, including from its JSON callback.
 * @param[in] json Required complete JSON bytes, with no transmitted terminator.
 * @param[in] length Positive byte count, at most UINT32_MAX and TX body capacity.
 * @return OK for staging; BUSY for occupied slot/reentry; STATE while closed or
 * detached; IO once on generation change; INVALID_ARG, NO_MEM, FAIL for input;
 * or the current sticky error. Input errors do not poison the channel.
 * @note Copies before return; current callback RX bytes may be echoed, but
 * input must not overlap channel, CDC handle or TX storage. No CDC write/pump
 * occurs. A generation change cancels prior work and does not stage this call.
 */
esy_err_t esy_proto_json_usb_channel_send(esy_proto_json_usb_channel_t *channel,
                                         const char *json, size_t length);

/**
 * @brief Perform one bounded owner slice without pumping the USB stack.
 * @param[in,out] channel Initialized channel.
 * @return OK including closed idle; BUSY for zero/short-capacity TX or reentry;
 * IO once on generation change; INVALID_ARG, STATE, or a sticky parse/resource
 * error. Accepted/consumed bytes before a BUSY return are not replayed.
 * @note A session change takes precedence over the old session's sticky error.
 * No new-peer I/O is attempted in the call that reports that transition.
 */
esy_err_t esy_proto_json_usb_channel_poll(esy_proto_json_usb_channel_t *channel);

/**
 * @brief Copy local status, including a failed or disconnected channel.
 * @param[in] channel Initialized channel.
 * @param[out] status Required output, disjoint from channel; unchanged on error.
 * @return OK, INVALID_ARG for NULL or STATE for a zero/deinitialized handle.
 */
esy_err_t esy_proto_json_usb_channel_get_status(const esy_proto_json_usb_channel_t *channel,
                                               esy_proto_json_usb_channel_status_t *status);

/**
 * @brief Cancel local framing/staging and release borrows, leaving CDC intact.
 * @param[in,out] channel Initialized or zero handle, serialized with all users.
 * @return OK including repeated deinit, INVALID_ARG for NULL, BUSY during poll
 * or callback, or STATE for invalid handle contents. Failure retains resources.
 * @note Reuse on the same CDC session requires proving no residual/late partial
 * stream exists. After abandonment/error, close/discard the old peer session
 * before recreating. Bytes already accepted by CDC cannot be recalled here.
 */
esy_err_t esy_proto_json_usb_channel_deinit(esy_proto_json_usb_channel_t *channel);

#ifdef __cplusplus
}
#endif
#endif /* ESY_PROTO_JSON_USB_CHANNEL_H */
