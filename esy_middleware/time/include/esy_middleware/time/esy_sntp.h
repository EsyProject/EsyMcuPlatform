/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SNTP_H
#define ESY_SNTP_H

/**
 * @file esy_sntp.h
 * @brief Caller-driven, single-server SNTP exchange with explicit transport.
 *
 * One task/main-loop owner serializes this handle, its UTC clock and providers.
 * Calls are not ISR safe. There are no sockets, DNS, retries, workers or heap
 * allocation. Providers run synchronously, must be bounded and cannot reenter
 * this handle or retain packet buffers. The caller owns transport opening,
 * endpoint validation, polling frequency, retry/backoff and security policy.
 */

#include "esy_middleware/time/esy_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Supported unauthenticated NTP base-header datagram size. */
#define ESY_SNTP_PACKET_BYTES 48u
/** @brief Opaque handle capacity; not a stable ABI or persistent format. */
#define ESY_SNTP_STORAGE_BYTES 256u
/** @brief Largest supported exchange timeout in monotonic microseconds. */
#define ESY_SNTP_MAX_TIMEOUT_US UINT32_MAX

/**
 * @brief Send one complete UDP datagram to the pinned server, normally port 123.
 * @param[in] provider_ctx Borrowed transport context.
 * @param[in] packet Readable datagram, borrowed only until return.
 * @param[in] length Exactly ESY_SNTP_PACKET_BYTES.
 * @return ESY_OK if accepted in full, or a negative transport error.
 * @note A failure terminates this attempt even if the datagram may have left.
 */
typedef esy_err_t (*esy_sntp_send_fn)(void *provider_ctx,
                                    const uint8_t *packet, size_t length);

/**
 * @brief Receive at most one datagram from the configured server endpoint.
 * @param[in] provider_ctx Borrowed transport context.
 * @param[out] packet Destination for at most capacity bytes.
 * @param[in] capacity ESY_SNTP_PACKET_BYTES; never write beyond it.
 * @param[out] length Actual original datagram size, including any truncated tail.
 * @return ESY_OK for one datagram, ESY_ERR_BUSY when none is immediately ready,
 * or another negative transport error, which terminates the exchange.
 * @note The provider must reject packets from other addresses/ports. It must
 * not report a truncated oversized datagram as a valid 48-byte datagram.
 */
typedef esy_err_t (*esy_sntp_receive_fn)(void *provider_ctx, uint8_t *packet,
                                       size_t capacity, size_t *length);

/**
 * @brief Configuration copied by init; referenced contexts/clock remain borrowed.
 * @note Both monotonic providers must observe the same counter epoch and rate.
 * Deadline decisions use returned samples; sample near return and include any
 * remaining clock-provider latency in the consumer's execution budget.
 */
typedef struct {
    esy_time_t *time; /**< Initialized UTC service, invalid is permitted. */
    esy_time_monotonic_fn monotonic_us; /**< Same counter epoch/rate as the UTC service. */
    void *clock_ctx; /**< Borrowed monotonic provider context. */
    esy_sntp_send_fn send; /**< Required bounded datagram sender. */
    esy_sntp_receive_fn receive; /**< Required bounded, immediately returning receiver. */
    void *transport_ctx; /**< Borrowed, initialized and endpoint-pinned UDP transport. */
} esy_sntp_config_t;

/**
 * @brief Caller-owned handle requiring `{0}` before first init.
 * @note Do not copy, move or clear a live handle. Outputs/configuration/packets
 * must not overlap it. Successful deinit restores the zero state.
 */
typedef struct {
    union {
        uint8_t bytes[ESY_SNTP_STORAGE_BYTES]; /**< Private state. */
        max_align_t alignment; /**< Private state alignment. */
    } storage; /**< In-place state; no external buffers required. */
} esy_sntp_t;

/** @brief Explicit one-shot exchange state, changed only by public operations. */
typedef enum {
    ESY_SNTP_IDLE = 0, /**< Initialized without an attempt. */
    ESY_SNTP_WAITING = 1, /**< One request sent; awaiting a valid response. */
    ESY_SNTP_COMPLETE = 2, /**< UTC was updated successfully. */
    ESY_SNTP_CANCELLED = 3, /**< Explicitly cancelled; no automatic restart. */
    ESY_SNTP_FAILED = 4 /**< Timeout/provider failure; no automatic restart. */
} esy_sntp_state_t;

/** @brief Snapshot of the current/last attempt, without provider calls. */
typedef struct {
    esy_sntp_state_t state; /**< Current state. */
    esy_err_t last_error; /**< Last process/start error, ESY_OK on success/cancel. */
    esy_timestamp_t synchronized_utc; /**< UTC at completion; meaningful only in COMPLETE. */
    uint64_t round_trip_ns; /**< Network delay excluding server processing; COMPLETE only. */
    int64_t offset_ns; /**< Applied UTC minus previous local UTC; only if offset_valid. */
    bool offset_valid; /**< Prior clock was valid and in the supported SNTP era window. */
    uint8_t stratum; /**< Accepted server stratum 1..15; COMPLETE only. */
} esy_sntp_status_t;

/**
 * @brief Borrow providers and an initialized UTC service without network access.
 * @param[in,out] client Zero-initialized or successfully deinitialized handle.
 * @param[in] config Required configuration with all callbacks and time non-NULL.
 * @return ESY_OK, INVALID_ARG, or STATE for repeated init/uninitialized UTC
 * service. Failure preserves the handle. Keep borrowed resources alive until
 * deinit; do not deinitialize or externally step UTC during an active attempt.
 */
esy_err_t esy_sntp_init(esy_sntp_t *client, const esy_sntp_config_t *config);

/**
 * @brief Cancel locally and release providers without closing caller transport.
 * @param[in,out] client Initialized or zero handle.
 * @return ESY_OK including repeated deinit, INVALID_ARG or STATE.
 * @note No provider callback occurs; no retained asynchronous access exists.
 * Late UDP replies must be rejected by a subsequent attempt's unique token.
 */
esy_err_t esy_sntp_deinit(esy_sntp_t *client);

/**
 * @brief Send one SNTPv4 request and arm one explicit bounded wait.
 * @param[in,out] client Initialized, non-WAITING handle.
 * @param[in] timeout_us Timeout 1..ESY_SNTP_MAX_TIMEOUT_US; elapsed >= timeout
 * expires, including time spent in providers. Time continues during sleep if
 * the injected monotonic provider does. Invoke process before a full 2^64-us
 * counter cycle; recommended polling interval is shorter than timeout_us.
 * @param[in] request_token Nonzero unique 64-bit token copied verbatim into
 * transmit timestamp. Supply an unpredictable token for replay resistance;
 * never reuse it across attempts, including failed sends and reinitialization.
 * @return ESY_OK, INVALID_ARG, BUSY if already waiting, STATE, or a propagated
 * clock/send error. Provider errors finish in FAILED. Same-as-last token is
 * rejected. No automatic retry or timer exists.
 * @note An opaque token is used for correlation; local T1 is sampled separately.
 * The supported server epoch is fixed at Unix seconds 0..UINT32_MAX, spanning
 * 1970-01-01 through 2106-02-07 06:28:15 UTC and the NTP 2036 era rollover.
 */
esy_err_t esy_sntp_start(esy_sntp_t *client, uint32_t timeout_us,
                         uint64_t request_token);

/**
 * @brief Perform at most one receive, validate it and optionally step UTC once.
 * @param[in,out] client Initialized handle.
 * @return ESY_OK while waiting/completed/cancelled, STATE before first start,
 * TIMEOUT at expiry, CRC_FAIL for a discarded malformed/unmatched packet
 * (wait stays active), NOT_SUPPORTED for a matched stratum-zero/KoD response
 * (FAILED), or another propagated provider error (FAILED).
 * @note Terminal states never sample providers or rearm a wait, even after an
 * entire clock cycle. FAILED returns its terminal error on subsequent calls.
 * Timeout is checked again at the final UTC anchor sample before committing;
 * a timed-out attempt preserves the previous UTC anchor and validity.
 * Exact 48-byte replies, version 4, mode 4 and matching originate are required.
 * Matched stratum-zero/KoD replies terminate even with LI == 3. Time samples
 * additionally require LI != 3, stratum 1..15 and nonzero receive/transmit
 * timestamps. A server processing interval longer than the measured exchange
 * is rejected. Fraction
 * decoding truncates below one nanosecond. No authentication or NTP filtering
 * is provided. UTC uses server T3 plus half the estimated network delay; with
 * a valid initial clock this equals the four-timestamp offset calculation.
 * Provider/poll latency contributes to synchronization error; this API makes
 * no hard-real-time or clock-accuracy guarantee.
 */
esy_err_t esy_sntp_process(esy_sntp_t *client);

/**
 * @brief Disarm an active attempt without transport/provider calls.
 * @param[in,out] client Initialized handle.
 * @return ESY_OK, INVALID_ARG or STATE. Only WAITING changes to CANCELLED;
 * terminal/idle states are preserved. Only a new explicit start rearms timeout.
 */
esy_err_t esy_sntp_cancel(esy_sntp_t *client);

/**
 * @brief Copy current status without sampling providers.
 * @param[in] client Initialized handle.
 * @param[out] status Required output, unchanged on failure.
 * @return ESY_OK, INVALID_ARG or STATE.
 */
esy_err_t esy_sntp_get_status(const esy_sntp_t *client, esy_sntp_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* ESY_SNTP_H */
