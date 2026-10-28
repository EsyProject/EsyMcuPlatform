/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HTTP_CLIENT_H
#define ESY_HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/network/esy_network_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief Caller-driven HTTP/1.1 transactions over an authenticated or plain stream.
 *
 * One non-ISR task/main loop serializes every call and the borrowed provider.
 * Callbacks run synchronously on that owner; only get_status may be reentered.
 * No endpoint, worker, allocator, TLS policy, retry or OTA policy is created.
 * Establish a fresh TCP/TLS stream before init. Exactly one submit is allowed
 * per initialized client. After completion, cancellation or failure, stop this
 * borrower, deinit it, then retire TLS/TCP in their documented order. No HTTP
 * operation closes the borrowed stream. Never copy/move/reset a live handle.
 */

/** @brief Private storage capacity, not a stable ABI; rebuild consumers on change. */
#define ESY_HTTP_CLIENT_STORAGE_BYTES 2048u

/** @brief Opaque caller-owned client; first use requires {0}. */
typedef struct {
    ESY_ALIGNED_STORAGE(priv, ESY_HTTP_CLIENT_STORAGE_BYTES);
} esy_http_client_t;

/** @brief Borrowed byte string; length excludes any optional NUL terminator. */
typedef struct {
    const char *data; /**< NULL is permitted only with zero length. */
    size_t length; /**< Byte length; no encoding or escaping is performed. */
} esy_http_string_t;

/** @brief Supported request methods. CONNECT and protocol upgrades are excluded. */
typedef enum {
    ESY_HTTP_GET = 0, /**< Retrieve a representation. */
    ESY_HTTP_HEAD, /**< Retrieve headers without a response body. */
    ESY_HTTP_POST, /**< Submit a known-length body. */
    ESY_HTTP_PUT, /**< Replace using a known-length body. */
    ESY_HTTP_DELETE, /**< Delete a resource. */
    ESY_HTTP_PATCH /**< Apply a known-length patch. */
} esy_http_method_t;

/** @brief Request header; framing and connection headers are generated internally. */
typedef struct {
    esy_http_string_t name; /**< Nonempty HTTP token. */
    esy_http_string_t value; /**< Field value without CR/LF, NUL or other controls. */
} esy_http_request_header_t;

/** @brief One request; submit copies headers, but borrows body until terminal state. */
typedef struct {
    esy_http_method_t method; /**< One supported method. */
    esy_http_string_t host; /**< Nonempty Host authority, including explicit port if needed. */
    esy_http_string_t target; /**< Encoded origin-form path/query beginning with '/', no fragment. */
    const esy_http_request_header_t *headers; /**< NULL only when header_count is zero. */
    size_t header_count; /**< Number of custom headers. */
    const uint8_t *body; /**< NULL only when body_size is zero; disjoint from workspaces. */
    size_t body_size; /**< Known byte count; no chunked request encoding. */
} esy_http_request_t;

/** @brief Header event; all views expire when the callback returns. */
typedef struct {
    esy_http_string_t name; /**< Complete field name; case is preserved. */
    esy_http_string_t value; /**< Complete field value; surrounding whitespace is provider-defined. */
    uint16_t status_code; /**< Response status, including informational responses. */
    bool trailer; /**< True for fields following the terminal body chunk. */
} esy_http_response_header_t;

/**
 * @brief Consume one decoded body block, borrowed only during this callback.
 * @return OK accepts the whole block; BUSY accepts none and retries identical
 * bytes on the next process call without receiving more input. Other errors
 * terminate the transaction. Do not retain the pointer, mutate/reenter the
 * client, or perform unbudgeted work. BUSY does not renew the idle timeout.
 */
typedef esy_err_t (*esy_http_body_cb_t)(void *context, const uint8_t *data,
                                       size_t size);

/**
 * @brief Observe a complete header on the owner, optionally rejecting it.
 * @return OK to continue; any error terminates the transaction. BUSY is not
 * retryable here. Copy needed values before returning. Framing trailers are
 * rejected before notification. No client mutation or retained views allowed.
 */
typedef esy_err_t (*esy_http_header_cb_t)(void *context,
                                         const esy_http_response_header_t *header);

/** @brief Exclusive, pairwise-disjoint writable storage borrowed through deinit. */
typedef struct {
    uint8_t *request_buffer; /**< Encoded request headers; contents need not be initialized. */
    size_t request_buffer_size; /**< Positive, at most INT32_MAX bytes. */
    uint8_t *receive_buffer; /**< Raw incremental parser input. */
    size_t receive_buffer_size; /**< Positive, at most INT32_MAX bytes. */
    uint8_t *body_buffer; /**< Holds a decoded block across consumer backpressure. */
    size_t body_buffer_size; /**< At least receive_buffer_size, at most INT32_MAX. */
    uint8_t *header_buffer; /**< One complete response field name and value. */
    size_t header_buffer_size; /**< At least two, at most INT32_MAX bytes. */
} esy_http_client_static_resources_t;

/** @brief Declare one aggregate of caller-owned HTTP buffers. */
#define ESY_HTTP_CLIENT_STATIC_STORAGE(name, request_bytes, receive_bytes, header_bytes) \
    struct { \
        uint8_t request_buffer[(request_bytes)]; \
        uint8_t receive_buffer[(receive_bytes)]; \
        uint8_t body_buffer[(receive_bytes)]; \
        uint8_t header_buffer[(header_bytes)]; \
    } name

/** @brief Bind aggregate storage; init copies the resource descriptor. */
#define ESY_HTTP_CLIENT_BIND_STATIC_RESOURCES(resources, storage) \
    do { \
        (resources)->request_buffer = (storage).request_buffer; \
        (resources)->request_buffer_size = sizeof((storage).request_buffer); \
        (resources)->receive_buffer = (storage).receive_buffer; \
        (resources)->receive_buffer_size = sizeof((storage).receive_buffer); \
        (resources)->body_buffer = (storage).body_buffer; \
        (resources)->body_buffer_size = sizeof((storage).body_buffer); \
        (resources)->header_buffer = (storage).header_buffer; \
        (resources)->header_buffer_size = sizeof((storage).header_buffer); \
    } while (0)

/**
 * @brief Inputs copied by init; referenced providers, buffers and contexts remain borrowed.
 *
 * monotonic_us is a continuous modulo-2^64 microsecond clock (period about
 * 584,542 years). It must advance during provider/callback execution and sleep
 * when waits should elapse, and must not reset/change rate during a transaction.
 * Both budgets are 1..UINT32_MAX us. Observe active waits at least once within
 * min(total_timeout_us, idle_timeout_us), and before a complete counter cycle;
 * meet any shorter network/provider cadence too. Expiry at elapsed >= budget
 * wins, including work that just completed. Submit arms both waits; accepted
 * I/O/body progress refreshes only idle after checking its old deadline.
 * Terminal states permanently disarm both waits, including across full wraps.
 */
typedef struct {
    esy_network_stream_t transport; /**< Connected stream, exclusively borrowed. */
    const esy_http_client_static_resources_t *static_resources; /**< Copied at init. */
    esy_err_t (*monotonic_us)(void *context, uint64_t *microseconds); /**< Required clock. */
    void *clock_context; /**< Borrowed clock context; may be NULL. */
    uint32_t total_timeout_us; /**< Positive entire-transaction budget, including body backpressure. */
    uint32_t idle_timeout_us; /**< Positive no-progress budget. */
    uint32_t io_budget_bytes; /**< Positive maximum send or receive bytes per process call. */
    uint32_t max_header_bytes; /**< Positive status-reason/name/value byte budget, plus one per message/field; includes 1xx/trailers. */
    uint64_t max_body_bytes; /**< Positive decoded response-body limit. */
    esy_http_body_cb_t body_cb; /**< Required synchronous whole-block consumer. */
    esy_http_header_cb_t header_cb; /**< Optional synchronous header observer. */
    void *callback_context; /**< Borrowed context shared by both callbacks. */
} esy_http_client_config_t;

/** @brief Transaction lifecycle; terminal states permit only status/cancel/deinit. */
typedef enum {
    ESY_HTTP_IDLE = 0, /**< Initialized; one request may be submitted. */
    ESY_HTTP_SENDING, /**< Sending request headers/body in bounded prefixes. */
    ESY_HTTP_RECEIVING, /**< Parsing response or waiting for its body consumer. */
    ESY_HTTP_COMPLETE, /**< Complete framing and all body blocks accepted. */
    ESY_HTTP_FAILED, /**< Sticky I/O, timeout, parser or consumer failure. */
    ESY_HTTP_CANCELLED /**< Explicitly cancelled; error is STATE. */
} esy_http_client_state_t;

/** @brief Owner-context status; HTTP status is independent of transaction errors. */
typedef struct {
    esy_http_client_state_t state; /**< Current lifecycle state. */
    esy_err_t error; /**< Sticky terminal error, OK otherwise. */
    uint16_t status_code; /**< Final response code, zero before final headers. */
    uint64_t body_bytes; /**< Decoded bytes accepted by body_cb. */
    int parser_error; /**< Native parser diagnostic, zero unless parsing failed. */
} esy_http_client_status_t;

/**
 * @brief Initialize a zero/deinitialized handle without I/O or allocation.
 * @return OK, INVALID_ARG for invalid/overlapping resources, STATE for repeated
 * init, BUSY for callback reentry. Failure preserves the handle. All referenced
 * resources remain alive and disjoint from the handle until successful deinit.
 */
esy_err_t esy_http_client_init(esy_http_client_t *client,
                               const esy_http_client_config_t *config);

/**
 * @brief Validate/copy one request and start its deadlines without network I/O.
 * @return OK, INVALID_ARG, NO_MEM for request-header capacity, STATE if not IDLE,
 * BUSY on reentry, or a clock error. Argument/capacity/clock failures leave IDLE.
 * Host, Content-Length, Connection, Transfer-Encoding, Expect, Upgrade, Trailer
 * and TE custom headers are rejected. Connection: close is generated. Request
 * strings/arrays need live only through submit; the body remains borrowed until
 * terminal state. Inputs must not alias the handle or any workspace.
 */
esy_err_t esy_http_client_submit(esy_http_client_t *client,
                                 const esy_http_request_t *request);

/**
 * @brief Advance at most one provider iteration and one bounded send or receive.
 *
 * A pending body block is retried before any provider/input progress. Parsed
 * body is delivered at most once per call; BUSY retains it and process returns
 * OK. Only explicit Content-Length/chunked framing or bodyless responses are
 * supported. Ambiguous lengths, unsupported transfer/content coding, upgrade,
 * excess bytes in the same input block after a final response and truncated
 * responses fail closed. Later unread bytes are retired with the connection.
 * Informational responses are consumed until a final response. Header callbacks
 * and native TLS/provider work are synchronous: byte limits do not establish a
 * wall-clock/hard realtime bound. Cancel before an unbounded pause or stopped
 * clock. COMPLETE returns OK; FAILED returns its error; IDLE/CANCELLED return STATE.
 * @return OK for progress, temporary no-data/backpressure or completion; sticky
 * IO/INVALID_ARG/NOT_SUPPORTED/NO_MEM/TIMEOUT or propagated provider/consumer error.
 */
esy_err_t esy_http_client_process(esy_http_client_t *client);

/** @brief Copy status without I/O; output unchanged on INVALID_ARG/STATE. Callback-safe. */
esy_err_t esy_http_client_get_status(const esy_http_client_t *client,
                                     esy_http_client_status_t *status);

/**
 * @brief Stop an initialized transaction without provider I/O or closing its stream.
 * @return OK, INVALID_ARG, STATE if uninitialized, BUSY on callback reentry.
 * Terminal states are preserved; repeated cancel succeeds. IDLE/active becomes
 * CANCELLED, releases request-body borrow and permanently disables deadlines.
 */
esy_err_t esy_http_client_cancel(esy_http_client_t *client);

/**
 * @brief Detach an IDLE/terminal client and restore {0}; performs no provider I/O.
 * @return OK (also for zero state), INVALID_ARG, BUSY on callback reentry or
 * STATE while active. Failure retains all borrows; cancel first if active.
 */
esy_err_t esy_http_client_deinit(esy_http_client_t *client);

#ifdef __cplusplus
}
#endif
#endif /* ESY_HTTP_CLIENT_H */
