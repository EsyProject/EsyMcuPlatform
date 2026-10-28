/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_BIN_SESSION_H
#define ESY_PROTO_BIN_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/protocol/esy_proto_bin_types.h"
#include "esy_middleware/protocol/esy_proto_bin_sender.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Caller-owned private session capacity; rebuild consumers if changed. */
#define ESY_PROTO_BIN_SESSION_STORAGE_BYTES 512u
/** @brief Alignment required by the private session state. */
#define ESY_PROTO_BIN_SESSION_ALIGNMENT 32u

/**
 * @brief Reliable stop-and-wait ProtoBin session. Initialize with {0}; never copy
 * or clear a live session. One serialized task/loop owns every call and callback;
 * no API is ISR-safe. The session creates no task and performs no allocation.
 *
 * DATA is framed, acknowledged and delivered once per received sequence;
 * retransmissions are acknowledged again. Only a matching ProtoBin ACK completes
 * an outbound transaction. NACK requests retry after physical TX retires.
 * Each poll reads at most rx_byte_budget bytes, then advances TX and deadlines.
 * Synchronous I/O/application callbacks contribute to execution time.
 *
 * Time uses the modulo-32-bit HAL millisecond tick (about 49.7 days per wrap).
 * Both waits are 1..INT32_MAX ms; owner observation intervals stay below 2^31 ms.
 * A stopped tick freezes waits. Quiesce before clock resets or unbounded pauses.
 * Completed/cancelled waits stay inactive until an explicitly accepted attempt.
 * The physical completion wait starts after successful send if TX remains busy;
 * the ProtoBin ACK wait starts after observed physical idle. A physical timeout
 * never retries a frame whose I/O borrow remains active.
 *
 * Callbacks may reserve/commit/cancel TX, but may not recursively poll/reset or
 * perform lifecycle operations. Session-reset callbacks cannot enqueue TX.
 * Buffers, I/O and callback contexts outlive successful deinit. Stop physical
 * I/O before reset/deinit; BUSY cleanup retains all borrowed references.
 */
typedef struct {
  alignas(ESY_PROTO_BIN_SESSION_ALIGNMENT)
      ESY_ALIGNED_STORAGE(priv, ESY_PROTO_BIN_SESSION_STORAGE_BYTES);
} esy_proto_bin_session_t;

/** @brief Backing storage exclusively borrowed until successful deinit.
 * Buffers need no initial contents. Init rejects overlapping buffers, wrapping
 * address ranges, and overlap with session storage. */
typedef struct {
  uint8_t *tx_queue_buf; /**< Block ring storage. */
  size_t tx_queue_buf_size; /**< At least count * (payload + block overhead). */
  uint8_t *tx_work_buf; /**< Frame build workspace. */
  size_t tx_work_buf_size; /**< At least TX payload plus frame overhead. */
  uint8_t *rx_work_buf; /**< Frame parse workspace. */
  size_t rx_work_buf_size; /**< At least RX payload plus frame overhead. */
} esy_proto_bin_session_static_resources_t;

/** @brief Initialization inputs copied during init, including resource descriptor. */
typedef struct {
  const esy_proto_bin_session_static_resources_t *static_resources;
  uint32_t tx_queue_block_count; /**< Power of two >=2; capacity count-1;
      count * block bytes must fit both UINT32_MAX and the supplied storage. */
  uint32_t tx_payload_max; /**< 1..UINT16_MAX payload bytes. */
  uint32_t rx_payload_max; /**< 1..UINT16_MAX payload bytes. */
  uint32_t rx_byte_budget; /**< Zero selects ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET. */
  uint32_t tx_completion_timeout_ms; /**< 1..INT32_MAX, after accepted async TX. */
  uint32_t acknowledgement_timeout_ms; /**< 1..INT32_MAX, after physical idle. */
  uint32_t retries; /**< Retries after the initial attempt. */
  esy_proto_bin_tp_data_cb_t data_cb; /**< Optional synchronous DATA callback. */
  esy_proto_bin_tp_event_cb_t event_cb; /**< Optional synchronous outcome callback. */
  esy_proto_bin_tp_io_t io; /**< Required send/receive/readiness, optional TX busy. */
  void *user_data; /**< Borrowed I/O and callback context. */
} esy_proto_bin_session_config_t;

/** @brief Clear configuration; select the default RX byte budget. NULL is a no-op. */
void esy_proto_bin_session_default_cfg(esy_proto_bin_session_config_t *config);
/** @brief Initialize {0} storage. Failure borrows nothing and preserves storage.
 * Returns INVALID_ARG for invalid config/storage, STATE if already initialized. */
esy_err_t esy_proto_bin_session_init(esy_proto_bin_session_t *session,
                                     const esy_proto_bin_session_config_t *config);
/** @brief Release references and discard protocol work without callbacks.
 * Stop physical I/O first; reset_session beforehand to report queued failures.
 * Returns INVALID_ARG for NULL, BUSY during poll/reset or outstanding physical
 * TX, otherwise OK (also for inactive storage). Failure retains every resource. */
esy_err_t esy_proto_bin_session_deinit(esy_proto_bin_session_t *session);
/**
 * @brief Optionally report the last accepted physical TX result after retirement.
 *
 * The same non-ISR owner calls this after the I/O provider releases all frame
 * storage and observes physical wire idle, before polling/querying the session
 * again or accepting another physical TX. Notify exactly once, in send order;
 * duplicate old notifications must never be delivered after a later send.
 * Providers without notifications retain the existing is_tx_busy completion
 * path. A notification with no remaining borrow is ignored successfully.
 *
 * Success starts the matching DATA acknowledgment wait at this observation.
 * A negative result fails only the matching, still-transmitting DATA transaction
 * once with IO_ERROR. A DATA transaction already ACKed or physically timed out
 * is not reported again. Control failure reports IO_ERROR with the invalid
 * transaction ID and restores that ACK ahead of any subsequently pending ACK;
 * it preserves queued DATA, RX state and sequence deduplication.
 *
 * Completion callbacks run synchronously here and may reserve/commit/cancel TX.
 * Do not call from poll/reset or any application callback, or recursively.
 * @param[in,out] session Initialized session with caller-owned stable storage.
 * @param[in] result ESY_OK for completed TX, or a negative terminal I/O status.
 * @return OK if consumed/ignored; INVALID_ARG for NULL, positive result or ISR;
 * STATE if inactive; BUSY during poll/reset/reentry or while physical TX is busy.
 * BUSY preserves the outstanding result for owner retry after retirement.
 */
esy_err_t esy_proto_bin_session_on_tx_complete(esy_proto_bin_session_t *session,
                                              esy_err_t result);
/** @brief Query initialized state on the owner; NULL returns false. */
bool esy_proto_bin_session_is_initialized(const esy_proto_bin_session_t *session);
/** @brief True only when initialized with no reservation, queued/control TX,
 * protocol acknowledgment or physical TX pending. NULL/inactive returns false. */
bool esy_proto_bin_session_is_idle(const esy_proto_bin_session_t *session);
/** @brief Reserve a payload buffer; NULL means inactive/busy/full. Optional
 * capacity is written zero on failure. Call commit/cancel before another reserve. */
uint8_t *esy_proto_bin_session_get_tx_buffer(esy_proto_bin_session_t *session,
                                             uint32_t *capacity);
/** @brief Commit the reservation. Optional ID is zeroed before validation and
 * nonzero on success. INVALID_ARG/STATE retains the reservation; no I/O occurs. */
esy_err_t esy_proto_bin_session_commit_tx(esy_proto_bin_session_t *session,
                                          uint32_t length, uint32_t *transaction_id);
/** @brief Cancel an uncommitted reservation without affecting committed TX.
 * Returns INVALID_ARG for NULL, STATE if inactive/resetting, otherwise OK. */
esy_err_t esy_proto_bin_session_cancel_tx(esy_proto_bin_session_t *session);
/** @brief Perform one bounded state-machine step. NULL/inactive/recursive calls
 * are ignored; I/O/application callbacks run synchronously on the owner. */
void esy_proto_bin_session_poll(esy_proto_bin_session_t *session);
/** @brief Discard incomplete RX after byte-stream loss. Owner only; ignored
 * while inactive or inside poll/reset. */
void esy_proto_bin_session_reset_rx(esy_proto_bin_session_t *session);
/** @brief After stopping physical I/O, reset RX/dedup and fail each committed
 * transaction once with IO_ERROR. Cancels reservations. Callback submissions are
 * rejected until complete. Ignored while inactive or inside poll/reset. */
void esy_proto_bin_session_reset_session(esy_proto_bin_session_t *session);
/** @brief Obtain a copied narrow sender borrowing this initialized session.
 * Returns INVALID_ARG for NULL arguments, STATE if inactive, otherwise OK.
 * Output is unchanged on failure. Detach borrowers before session deinit; an old
 * descriptor is invalid after deinit and must not be reused after reinitializing. */
esy_err_t esy_proto_bin_session_get_sender(esy_proto_bin_session_t *session,
                                           esy_proto_bin_sender_t *sender);

#ifdef __cplusplus
}
#endif
#endif /* ESY_PROTO_BIN_SESSION_H */
