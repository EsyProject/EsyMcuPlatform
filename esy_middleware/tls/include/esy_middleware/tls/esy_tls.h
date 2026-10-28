/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TLS_H
#define ESY_TLS_H

#include "esy_common/defs/base/esy_time_defs.h"
#include "esy_middleware/credentials/esy_credentials.h"
#include "esy_middleware/crypto/esy_crypto.h"
#include "esy_middleware/network/esy_network_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_tls.h
 * @brief Caller-driven authenticated TLS client over an already connected stream.
 *
 * One normal startup/task/main-loop owner serializes ALL calls and borrowed
 * providers. No ISR/fault support, worker, socket, OS or reconnect policy.
 * Providers must not reenter this handle. Keep the transport, random provider,
 * clock contexts and workspace alive until successful deinit. Disconnect and
 * destroy the transport only after deinit; a failed/closed TLS connection cannot
 * be reused. First use requires {0}; never copy/move/clear a live object.
 * The selected adapter header completes esy_tls_t and documents native resources.
 */

/** @brief Adapter-completed caller-owned client handle. */
typedef struct esy_tls esy_tls_t;

/** @brief Explicit connection states; only init activates a new handshake. */
typedef enum {
  ESY_TLS_HANDSHAKING = 0, /**< Waiting for authenticated handshake completion. */
  ESY_TLS_READY, /**< Authenticated application I/O is allowed. */
  ESY_TLS_CLOSING, /**< Flushing accepted bytes and sending close_notify. */
  ESY_TLS_CLOSED, /**< Peer/local orderly close; deinit before reconnecting. */
  ESY_TLS_FAILED /**< Sticky failure; only status/deinit remain useful. */
} esy_tls_state_t;

/** @brief Borrowed writable TX staging memory; initial contents may be arbitrary. */
typedef struct {
  uint8_t *tx_buffer; /**< Exclusive buffer, disjoint from live handles/providers. */
  size_t tx_buffer_size; /**< Capacity, 1..INT32_MAX; size from the memory budget. */
} esy_tls_static_resources_t;

/**
 * @brief Inputs copied by init; descriptor objects need not outlive init.
 *
 * monotonic_us is an unsigned 64-bit microsecond counter modulo 2^64. It must
 * not reset/change rate and must progress across sleep if timeouts should.
 * Handshake wait is 1..UINT32_MAX us; process must observe an active handshake
 * before a complete 2^64-us cycle (about 584,542 years), preferably within the
 * timeout. At elapsed >= timeout, timeout wins, including provider runtime.
 * Completion/close/failure permanently disarm that wait until another init.
 * utc_now supplies established, trusted UTC in the esy_timestamp_t range. It
 * is checked at init to select currently valid trust anchors and for EACH
 * certificate in the verified chain; failures reject authentication. The anchor
 * selection is an init-time snapshot. Reinitialize after clock corrections or
 * validity-window changes to select another set; verification still rejects
 * certificates that expire after init. UTC trust/freshness policy belongs to
 * the consumer. Peer-intermediate path selection remains provider-defined.
 */
typedef struct {
  esy_network_stream_t transport; /**< Connected plain stream, exclusively borrowed. */
  esy_credentials_t *credentials; /**< Bundle pinned against replacement until deinit. */
  esy_crypto_random_provider_t random; /**< Seeded cryptographic RNG, never a tick/UID. */
  const char *hostname; /**< Nonempty DNS verification/SNI name; copied by provider. */
  esy_err_t (*monotonic_us)(void *context, uint64_t *microseconds); /**< Monotonic clock. */
  esy_err_t (*utc_now)(void *context, esy_timestamp_t *timestamp); /**< Trusted UTC clock. */
  void *time_context; /**< Borrowed context for both clock callbacks. */
  uint32_t handshake_timeout_us; /**< Positive handshake budget including provider work. */
  const esy_tls_static_resources_t *static_resources; /**< Exclusive TX workspace. */
} esy_tls_config_t;

/** @brief Side-effect-free snapshot; outputs are unchanged on query failure. */
typedef struct {
  esy_tls_state_t state; /**< Current state. */
  esy_err_t error; /**< Sticky public error, ESY_OK otherwise. */
  int native_error; /**< Adapter diagnostic; never includes secret material. */
  size_t pending_tx_bytes; /**< Accepted bytes awaiting native write completion. */
} esy_tls_status_t;

/**
 * @brief Acquire credentials, parse certificates/key and prepare a client handshake.
 * @param tls Zero-initialized or successfully deinitialized adapter handle.
 * @param config Required transport, credentials, cryptographic RNG, clocks and memory.
 * @return OK, INVALID_ARG, STATE (repeated init), NO_MEM, NOT_SUPPORTED,
 * HASH_FAIL when no trust anchor is valid at the supplied UTC, or a propagated
 * provider failure. Invalid/malformed credentials fail closed.
 * @note No transport handshake is performed. Failure frees all native resources
 * and releases the lease, leaving the handle reusable. Native setup/parsing may
 * allocate, block or call RNG; budget provider resources. Workspace may change.
 * CA verification and hostname checking are mandatory; no insecure mode exists.
 */
esy_err_t esy_tls_init(esy_tls_t *tls, const esy_tls_config_t *config);

/**
 * @brief Advance transport once and perform one handshake/write/close operation.
 * @param tls Initialized handle on its owning execution context.
 * @return OK for a successful step, including temporary backpressure; STATE for
 * invalid/uninitialized/copied handle; TIMEOUT or another sticky provider error.
 * @note Native cryptography/provider callbacks are synchronous and may block or
 * allocate. A step is not a hard realtime guarantee. Call regularly until READY
 * and while TX/close is pending. CLOSED stays closed; FAILED returns its error.
 */
esy_err_t esy_tls_process(esy_tls_t *tls);

/**
 * @brief Get the authenticated stream after READY, with lifetime bounded by deinit.
 * @param tls Initialized READY handle.
 * @param stream Disjoint output, unchanged on failure.
 * @return OK, INVALID_ARG or STATE. Callback context is the borrowed TLS handle.
 * @note send copies and accepts at most one workspace-sized prefix; pending TX
 * makes subsequent sends return zero. progress flushes it with stable native
 * retry arguments. recv never waits for input. Peer close returns IO and marks
 * CLOSED, since network streams reserve zero for temporary lack of data.
 * A successful send proves local acceptance, not wire delivery or peer ACK.
 * Post-handshake control writes requiring a retry fail closed; renegotiation
 * and asynchronous post-handshake cryptography are outside this adapter.
 */
esy_err_t esy_tls_get_stream(esy_tls_t *tls, esy_network_stream_t *stream);

/**
 * @brief Query state without provider calls.
 * @param tls Initialized, non-copied handle.
 * @param status Disjoint output snapshot.
 * @return OK, INVALID_ARG or STATE.
 */
esy_err_t esy_tls_get_status(const esy_tls_t *tls, esy_tls_status_t *status);

/**
 * @brief Begin orderly close; process flushes accepted data then sends close_notify.
 * @param tls Initialized READY/CLOSING/CLOSED handle.
 * @return OK, INVALID_ARG or STATE. Repeated closing/closed calls succeed.
 * @note No peer-close acknowledgement wait. The owner budgets progress and may
 * call deinit to abort a stalled close; there is no hidden close timeout.
 */
esy_err_t esy_tls_close(esy_tls_t *tls);

/**
 * @brief Abort remaining I/O, free native resources and release the credential lease.
 * @param tls Initialized handle or zero state; providers must be quiescent.
 * @return OK, INVALID_ARG or STATE for an invalid/copied handle.
 * @note Never calls transport callbacks or closes its connection. Successful
 * cleanup restores {0}; zero-state deinit is idempotent. Accepted pending data
 * may be discarded. Existing stream borrowers must stop before this operation.
 */
esy_err_t esy_tls_deinit(esy_tls_t *tls);

#ifdef __cplusplus
}
#endif
#endif /* ESY_TLS_H */
