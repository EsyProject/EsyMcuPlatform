/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_MODBUS_TCP_CHANNEL_H
#define ESY_PROTO_MODBUS_TCP_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>

#include "esy_middleware/network/esy_network_connection.h"
#include "esy_middleware/protocol/esy_proto_modbus_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_proto_modbus_tcp_channel.h
 * @brief Caller-driven Modbus TCP master/client and slave/server channels.
 *
 * A channel copies one connection descriptor and borrows its exclusively owned,
 * already connected provider. send_all, get_status and stream.recv are required;
 * stream.progress is optional. Create accepts OPEN or EOF with a nonzero session
 * identifier and never connects, listens or closes an endpoint. One channel binds
 * one peer session until destroy. A changed session, INACTIVE state, malformed
 * MBAP or fatal provider error is sticky: poll keeps returning its first error,
 * admission closes before failure callbacks. Recovery requires the endpoint owner
 * to retire the old connection/residual stream and establish a new session
 * before recreating the channel; destroy alone does not make an old peer safe.
 * is_initialized reports local lifetime, including a failed live channel.
 *
 * Storage requires {0} before first create. Keep live handles at stable addresses;
 * do not copy/move/clear them. Configuration and master queue buffers must be
 * mutually disjoint and disjoint from the complete opaque storage/output pointer.
 * Create validates all ranges before writes and retains no resources on failure.
 * Successful destroy permits reuse and cancels queued requests/local unsent TX;
 * it does not close the endpoint or revoke bytes already copied to the provider.
 * Keep one channel for the peer session across transactions. After destroy with
 * unanswered provider-accepted requests or unread/partial RX, retire the old
 * connection and use a fresh session before creating another channel. Reusing
 * the same session requires proving there is no residual or late traffic;
 * recreation resets transaction identifiers and cannot distinguish stale replies.
 * Borrowed buffers, provider and callback context outlive successful destroy.
 *
 * One serialized non-ISR owner drives all APIs. poll/destroy cannot be called
 * recursively or from callbacks and return STATE; master completion callbacks
 * may submit/abort. Data/response buffers passed to callbacks are call-lifetime.
 * Each poll progresses the provider at most once, accepts at most one whole TX,
 * receives at most rx_byte_budget bytes, and dispatches at most one complete ADU.
 * Staged TX prevents further RX dispatch, preserving slave backpressure. Provider
 * callbacks are synchronous; their runtime is part of poll latency, not a hard
 * realtime guarantee. No OS objects, workers, allocations or native provider
 * headers are required by the channel.
 *
 * MBAP protocol ID must be zero and LEN must be 2..codec maximum PDU length+1.
 * Invalid framing is not resynchronized: poll returns CRC_FAIL and master
 * requests complete with BAD_FRAME. Prefix send_all success or overreported recv
 * returns IO; other fatal provider errors propagate. Session loss returns STATE.
 * Fatal master failures abort each accepted request once with TRANSPORT_ERROR,
 * except malformed/truncated frames use BAD_FRAME. Explicit abort/destroy retain
 * the ordinary ABORTED completion status. Request on_complete/user_ctx must be
 * NULL; the channel's typed callback routes every request completion.
 *
 * TX waits begin when a manager ADU is copied into channel staging. They use a
 * uint32_t monotonic HAL millisecond clock, wrapping after 2^32 ms. Effective
 * waits are 1..INT32_MAX ms and owner observation intervals stay below 2^31 ms.
 * The counter must not reset during a wait; a stopped clock freezes it. Expiry is
 * checked at poll entry even if progress/status is BUSY. Whole send_all acceptance
 * plus successful same-session confirmation permanently disarms that wait and
 * starts the master's manager response timeout;
 * an accepted write awaiting BUSY status confirmation is never sent twice and
 * remains covered by its original TX deadline. Peer ACK is required only by
 * is_peer_drained. Synchronous provider time may
 * extend call latency. Abort/failure/destroy also disarm the local TX wait.
 *
 * EOF permits buffered RX drain and slave replies. It prevents new master TX.
 * An empty recv returning IO is treated as EOF only when a fresh same-session
 * status confirms EOF with no readable bytes; other provider errors propagate.
 * Truncated EOF frames fail with CRC_FAIL; a master whose buffered responses are
 * exhausted with outstanding requests fails with IO. A healthy peer is drained
 * only after EOF, no buffered/partial RX, no staged TX or pending master request,
 * and freshly observed unacked_bytes==0. Endpoint close/reconnect stays with its
 * owner, which must bound the EOF/ACK drain wait externally. Master responses
 * must match the request UID; slaves serve their one
 * configured UID. All TCP UIDs 0..255 are accepted, with UID zero still replying.
 */

/** @brief Independent caller-driven master handle. */
typedef struct esy_proto_modbus_tcp_master_channel_s esy_proto_modbus_tcp_master_channel_t;
/** @brief Independent caller-driven slave handle. */
typedef struct esy_proto_modbus_tcp_slave_channel_s esy_proto_modbus_tcp_slave_channel_t;
/** @brief Fixed master capacity; includes framing, manager and TX staging. */
#define ESY_PROTO_MODBUS_TCP_MASTER_CHANNEL_STORAGE_BYTES (2048u)
/** @brief Fixed slave capacity; includes framing, manager and TX staging. */
#define ESY_PROTO_MODBUS_TCP_SLAVE_CHANNEL_STORAGE_BYTES (2048u)
/** @brief Zero-initialized master storage, retained through successful destroy. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_PROTO_MODBUS_TCP_MASTER_CHANNEL_STORAGE_BYTES];
} esy_proto_modbus_tcp_master_channel_storage_t;
/** @brief Zero-initialized slave storage, retained through successful destroy. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_PROTO_MODBUS_TCP_SLAVE_CHANNEL_STORAGE_BYTES];
} esy_proto_modbus_tcp_slave_channel_storage_t;

/** @brief Synchronous master completion; response is borrowed until return. */
typedef void (*esy_proto_modbus_tcp_master_channel_complete_cb)(
    esy_proto_modbus_tcp_master_channel_t *channel,
    esy_proto_modbus_req_status_t status, uint8_t unit_id,
    esy_proto_modbus_func_t function, const uint8_t *response,
    uint16_t response_length, void *user_ctx);
/** @brief Synchronous slave request; fill at most response_capacity bytes. */
typedef esy_err_t (*esy_proto_modbus_tcp_slave_channel_request_cb)(
    esy_proto_modbus_tcp_slave_channel_t *channel, uint8_t unit_id,
    esy_proto_modbus_func_t function, const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t *response_length, uint16_t response_capacity,
    void *user_ctx);

/** @brief Caller-owned aligned queue buffers, borrowed until successful destroy. */
typedef struct {
  esy_prio_slot_t *slot_storage; /**< Priority slot array. */
  uint16_t slot_storage_count; /**< At least queue_depth entries. */
  esy_proto_modbus_payload_t *payload_storage; /**< Request payload array. */
  uint16_t payload_storage_count; /**< At least queue_depth entries. */
} esy_proto_modbus_tcp_master_channel_static_resources_t;
/** @brief Declare independent master queue buffers with the requested capacities. */
#define ESY_PROTO_MODBUS_TCP_MASTER_CHANNEL_STATIC_STORAGE(name, slot_count, payload_count) \
  struct { \
    esy_prio_slot_t slot_storage[(slot_count)]; \
    esy_proto_modbus_payload_t payload_storage[(payload_count)]; \
  } name

/** @brief Copied master configuration; the provider and queue arrays are borrowed. */
typedef struct {
  esy_network_connection_t connection; /**< Exclusive connected peer descriptor. */
  uint32_t timeout_ms; /**< Manager response timeout; zero selects 100 ms. */
  uint8_t retry_max; /**< Manager retry count; zero disables retries. */
  uint16_t queue_depth; /**< 1..INT16_MAX, within both resource capacities. */
  esy_proto_modbus_tcp_master_channel_static_resources_t static_resources; /**< Queue arrays. */
  esy_proto_modbus_tcp_master_channel_complete_cb on_complete; /**< Optional completion. */
  void *user_ctx; /**< Borrowed completion context. */
  uint32_t tx_timeout_ms; /**< Local staging timeout; zero selects 1000 ms. */
  uint32_t rx_byte_budget; /**< Positive byte limit; zero selects 256. */
} esy_proto_modbus_tcp_master_channel_config_t;
/** @brief Copied slave configuration; the provider and callback context are borrowed. */
typedef struct {
  esy_network_connection_t connection; /**< Exclusive connected peer descriptor. */
  uint8_t unit_id; /**< Exact accepted TCP UID; all values 0..255 are valid. */
  esy_proto_modbus_tcp_slave_channel_request_cb on_request; /**< Required request handler. */
  void *user_ctx; /**< Borrowed request context. */
  uint32_t tx_timeout_ms; /**< Local staging timeout; zero selects 1000 ms. */
  uint32_t rx_byte_budget; /**< Positive byte limit; zero selects 256. */
} esy_proto_modbus_tcp_slave_channel_config_t;

/** @brief Return defaults: response 100 ms, no retries, TX 1000 ms, RX budget 256. */
esy_proto_modbus_tcp_master_channel_config_t esy_proto_modbus_tcp_master_channel_default_cfg(void);
/** @brief Return defaults: UID 0xff, TX 1000 ms and RX budget 256. */
esy_proto_modbus_tcp_slave_channel_config_t esy_proto_modbus_tcp_slave_channel_default_cfg(void);
/** @brief Validate, bind one existing peer and initialize local master state.
 * Returns OK, INVALID_ARG, STATE, BUSY or a provider error. out_channel is cleared
 * on valid nonaliasing calls; failure preserves retryable, unowned storage.
 */
esy_err_t esy_proto_modbus_tcp_master_channel_create(
    const esy_proto_modbus_tcp_master_channel_config_t *config,
    esy_proto_modbus_tcp_master_channel_storage_t *storage,
    esy_proto_modbus_tcp_master_channel_t **out_channel);
/** @brief Validate and initialize a local slave; same failure contract as master create. */
esy_err_t esy_proto_modbus_tcp_slave_channel_create(
    const esy_proto_modbus_tcp_slave_channel_config_t *config,
    esy_proto_modbus_tcp_slave_channel_storage_t *storage,
    esy_proto_modbus_tcp_slave_channel_t **out_channel);
/** @brief Perform one bounded owner slice; returns OK, BUSY or first sticky error. */
esy_err_t esy_proto_modbus_tcp_master_channel_poll(esy_proto_modbus_tcp_master_channel_t *channel);
/** @brief Perform one bounded slave slice under the master poll contract. */
esy_err_t esy_proto_modbus_tcp_slave_channel_poll(esy_proto_modbus_tcp_slave_channel_t *channel);
/** @brief Cancel local work and release ownership; NULL/repeated calls return OK.
 * No provider close is performed; recursive/ISR calls return STATE/INVALID_ARG.
 */
esy_err_t esy_proto_modbus_tcp_master_channel_destroy(esy_proto_modbus_tcp_master_channel_t *channel);
/** @brief Release local slave work under the master destroy contract. */
esy_err_t esy_proto_modbus_tcp_slave_channel_destroy(esy_proto_modbus_tcp_slave_channel_t *channel);
/** @brief Enqueue a copied request on this owner; user callback fields must be NULL. */
esy_err_t esy_proto_modbus_tcp_master_channel_submit(esy_proto_modbus_tcp_master_channel_t *channel,
    const esy_proto_modbus_request_t *request);
/** @brief Abort the entry snapshot; callback submissions survive. abort_current
 * also cancels the current locally staged ADU before notifying callbacks.
 */
esy_err_t esy_proto_modbus_tcp_master_channel_abort_all(esy_proto_modbus_tcp_master_channel_t *channel,
    bool abort_current);
/** @brief True while this local master instance is owned, including sticky failure. */
bool esy_proto_modbus_tcp_master_channel_is_initialized(const esy_proto_modbus_tcp_master_channel_t *channel);
/** @brief True while this local slave instance is owned, including sticky failure. */
bool esy_proto_modbus_tcp_slave_channel_is_initialized(const esy_proto_modbus_tcp_slave_channel_t *channel);
/** @brief Return pending master queue depth; zero for NULL/inactive instances. */
uint16_t esy_proto_modbus_tcp_master_channel_get_queue_depth(const esy_proto_modbus_tcp_master_channel_t *channel);
/** @brief Return manager state; IDLE for NULL/inactive instances. */
esy_proto_modbus_master_state_t esy_proto_modbus_tcp_master_channel_get_state(
    const esy_proto_modbus_tcp_master_channel_t *channel);
/** @brief Return configured TCP slave UID; zero for NULL/inactive instances. */
uint8_t esy_proto_modbus_tcp_slave_channel_get_addr(const esy_proto_modbus_tcp_slave_channel_t *channel);
/** @brief Refresh peer status and report healthy EOF drain; no input progress.
 * Returns false on BUSY, sticky failure, reentry, or unretired RX/TX/requests/ACKs.
 */
bool esy_proto_modbus_tcp_master_channel_is_peer_drained(esy_proto_modbus_tcp_master_channel_t *channel);
/** @brief Refresh and report slave EOF drain under the master drain contract. */
bool esy_proto_modbus_tcp_slave_channel_is_peer_drained(esy_proto_modbus_tcp_slave_channel_t *channel);

#ifdef __cplusplus
}
#endif
#endif /* ESY_PROTO_MODBUS_TCP_CHANNEL_H */
