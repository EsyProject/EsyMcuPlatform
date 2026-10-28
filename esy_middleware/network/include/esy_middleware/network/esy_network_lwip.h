/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_NETWORK_LWIP_H
#define ESY_NETWORK_LWIP_H

#include "esy_middleware/network/esy_network_connection.h"

/**
 * @file
 * @brief Plain TCP byte-stream provider using the lwIP 2.x raw API.
 *
 * This adapter owns its TCP PCB and at most one accepted pbuf chain; lwIP may
 * allocate native resources during DNS, connect, send, and close. It does not
 * access hardware, initialize a network interface, or drive lwIP timers/input.
 * The consumer provides and continuously drives its configured lwIP instance.
 * No TLS, certificate validation, reconnect, task, or timer is provided here.
 * A listener accepts one peer at a time. Retire that peer explicitly before
 * accepting the next one; this keeps borrowed session state observable.
 *
 * Every API and the returned transport callbacks must execute in the same lwIP
 * core context as raw TCP/DNS callbacks, serialized by one owner. With an OS,
 * use the provider's documented core execution/locking mechanism. No API is
 * ISR-safe, and no callback may concurrently access this handle. A different
 * core/task must post a bounded request to the owner rather than call directly.
 *
 * begin is asynchronous. The owner imposes its own DNS/connection deadline and
 * calls close to cancel; this adapter creates no independent timebase. Native
 * provider calls remain synchronous and determine execution time. Polling the
 * state does not drive lwIP. Keep the handle and RX storage alive until deinit
 * succeeds, including after an unsuccessful close or cancelled DNS lookup.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief lwIP endpoint lifecycle in the provider's serialized owner context. */
typedef enum {
  ESY_NETWORK_UNINITIALIZED = 0,
  ESY_NETWORK_IDLE = 1,
  ESY_NETWORK_RESOLVING = 2,
  ESY_NETWORK_CONNECTING = 3,
  ESY_NETWORK_LISTENING = 4,
  ESY_NETWORK_CONNECTED = 5,
  ESY_NETWORK_CLOSING = 6,
  ESY_NETWORK_CLOSED = 7, /**< Peer receive EOF; buffered RX and sending remain valid. */
  ESY_NETWORK_ERROR = 8
} esy_network_state_t;

/** @brief Private handle capacity; rebuild consumers when it changes. */
#define ESY_NETWORK_LWIP_STORAGE_BYTES 256u

/** @brief Opaque caller-owned handle. Initialize with {0}; never copy it. */
typedef struct {
  ESY_ALIGNED_STORAGE(priv, ESY_NETWORK_LWIP_STORAGE_BYTES);
} esy_network_lwip_t;

/** @brief Caller-owned receive storage, borrowed until successful deinit. */
typedef struct {
  uint8_t *rx_buffer; /**< Writable backing bytes; no initial contents needed. */
  uint32_t rx_buffer_size; /**< Nonzero power of two, at most INT32_MAX bytes. */
} esy_network_lwip_static_resources_t;

/** @brief Initialization inputs; the descriptor is copied during init. */
typedef struct {
  const esy_network_lwip_static_resources_t *static_resources; /**< Required. */
  /**
   * @brief Optional owner-context callback for explicit stream.progress.
   *
   * A polling provider supplies this callback so an owner or synchronous
   * protocol consumer can advance input/timers explicitly. It performs one
   * bounded provider iteration; independently driven stacks may use NULL.
   * It must not recursively drive its borrower or this adapter, or stop/delete
   * the provider. Raw TCP/DNS callbacks may run inside it. Its synchronous
   * latency contributes to the caller's execution budget.
   */
  void (*progress)(void *context);
  void *progress_context; /**< Borrowed until successful deinit; may be NULL. */
  /** @brief Optional paired native core entry/exit hooks around raw API calls.
   * They wrap raw API calls independently of explicit progress, must not recurse,
   * make concurrent use of this handle safe. NULL requires no native locking. */
  void (*owner_enter)(void *context);
  void (*owner_leave)(void *context); /**< Must be paired with owner_enter. */
  void *owner_context; /**< Borrowed until successful deinit. */
} esy_network_lwip_config_t;

/**
 * @brief Initialize a zero-initialized handle without network I/O/allocation.
 * @return OK, INVALID_ARG, or STATE for a repeated init. Failure preserves the
 * input handle and borrows nothing. RX storage is exclusive until deinit.
 */
esy_err_t esy_network_lwip_init(esy_network_lwip_t *handle,
                            const esy_network_lwip_config_t *config);

/**
 * @brief Start connecting from IDLE to a nonempty host and nonzero TCP port.
 *
 * Numeric addresses use the configured lwIP IPv4/IPv6 support. Other hostnames
 * require LWIP_DNS. Host text is borrowed only for this call. OK means accepted;
 * inspect get_state until CONNECTED or ERROR. DNS/native PCB allocation may
 * occur. DNS completion is an owner-context continuation of this lifecycle
 * operation and may create/connect the PCB; it is not a message-data callback.
 * Cancellation retains the handle until that continuation has retired.
 * No timeout or automatic retry is imposed. A failed call may leave ERROR
 * and must be followed by close before retrying; init remains valid.
 * @return OK, INVALID_ARG, STATE, BUSY on progress-callback reentry, NO_MEM,
 * NOT_SUPPORTED (DNS disabled), or IO.
 */
esy_err_t esy_network_lwip_begin(esy_network_lwip_t *handle, const char *host,
                             uint16_t port);

/**
 * @brief Start a single-peer listener from IDLE; backlog must be nonzero.
 * @return OK, INVALID_ARG, STATE, BUSY, NO_MEM or IO. Failed setup retires its
 * native PCB and leaves ERROR; close before retry. Extra peers are rejected.
 * The listener remains while a peer is connected, closed or retired.
 */
esy_err_t esy_network_lwip_listen(esy_network_lwip_t *handle, uint16_t port,
                                 uint8_t backlog);

/**
 * @brief Retire the current peer while retaining an existing listener.
 * @return OK (LISTENING or IDLE), BUSY/IO preserving callbacks and storage, or
 * INVALID_ARG/STATE. Stop stream borrowers first; retry while progressing lwIP.
 * This also cancels an outgoing client attempt, with the same DNS retention as
 * close. It never forcibly aborts a failed native close.
 */
esy_err_t esy_network_lwip_close_peer(esy_network_lwip_t *handle);

/**
 * @brief Observe state without progressing input, timers, DNS, or TCP.
 * @return UNINITIALIZED for NULL/uninitialized handles. Calls share the owner
 * context; this is not a cross-task synchronization primitive.
 */
esy_network_state_t esy_network_lwip_get_state(const esy_network_lwip_t *handle);

/**
 * @brief Obtain borrowed stream callbacks after endpoint initialization.
 *
 * Send/recv never progress input or timers implicitly. The owner or borrowing
 * consumer calls stream.progress explicitly as its scheduling contract requires.
 * I/O operations are usable only after CONNECTED. Peer EOF enters CLOSED (TCP receive half-close): recv drains
 * accepted bytes and then returns a negative value; send remains usable until
 * explicit close. CLOSED does not retire the native PCB. A zero result means
 * temporary
 * backpressure/no data. send copies accepted bytes into lwIP (TCP_WRITE_FLAG_COPY)
 * before returning, may accept a prefix, and never resubmits accepted bytes on
 * output failure. It bounds one write by tcp_sndbuf and UINT16_MAX. recv bounds
 * one transfer by INT32_MAX and acknowledges only bytes consumed by the caller.
 *
 * RX uses the supplied ring plus at most one owned pbuf chain (up to the lwIP
 * 16-bit chain length). Additional chains are refused with ERR_MEM and remain
 * owned by lwIP for redelivery. Accepted chains are freed after copying or on
 * close/error. This supports buffers smaller than one received chain.
 *
 * Stop and detach the protocol borrower before releasing/reusing the adapter. The returned context is borrowed until successful adapter deinit.
 * @return OK, INVALID_ARG, or STATE. Output is written only on success.
 */
esy_err_t esy_network_lwip_get_stream(esy_network_lwip_t *handle,
                                     esy_network_stream_t *transport);

/**
 * @brief Obtain the borrowed stream plus whole-write and peer-status callbacks.
 *
 * The base stream has the same behavior and lifetime as get_stream. Both
 * extension callbacks use connection.stream.context. Status maps CONNECTED to
 * OPEN, CLOSED to EOF and all other endpoint states to INACTIVE. Peer ACK counts,
 * receive availability and session identity retain their transport meaning.
 * Whole writes reject requests above UINT16_MAX; send_capacity observes native
 * capacity and may exceed that request limit.
 * Closing/resetting the endpoint invalidates earlier connection evidence;
 * neither INACTIVE nor EOF permits reclaiming native resources or caller storage.
 * @return OK, INVALID_ARG, or STATE. Output is written only on success.
 */
esy_err_t esy_network_lwip_get_connection(esy_network_lwip_t *handle,
                                         esy_network_connection_t *connection);

/**
 * @brief Cancel connection establishment or close the TCP stream; discard RX.
 *
 * Stop the protocol borrower first. OK establishes IDLE and removes adapter callbacks/references;
 * lwIP may retain its closing PCB internally. It does not guarantee peer receipt
 * or flushing: native TCP close may reset a connection with unread RX data.
 * repeated close is OK. lwIP cannot cancel a pending DNS request: close returns
 * BUSY and enters CLOSING until its callback retires, then IDLE. A TCP close
 * returning ERR_MEM also returns BUSY, retains the PCB/callbacks, and requires a
 * later close/deinit retry while lwIP keeps progressing. Other close failures
 * return IO and also retain resources. No failed close permits freeing/resetting
 * the handle, RX storage, or provider. There is no forced tcp_abort fallback.
 */
esy_err_t esy_network_lwip_close(esy_network_lwip_t *handle);

/**
 * @brief Close and release borrowed storage, zeroing the handle on success.
 * @return OK, INVALID_ARG, STATE if uninitialized, or the close error. Failure
 * retains initialization and all still-owned resources; retry after progress.
 * Successful deinit permits init again. Repeated deinit returns STATE.
 */
esy_err_t esy_network_lwip_deinit(esy_network_lwip_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* ESY_NETWORK_LWIP_H */
