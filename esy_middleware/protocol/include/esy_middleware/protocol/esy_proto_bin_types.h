/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_BIN_TYPES_H
#define ESY_PROTO_BIN_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/protocol/esy_proto_bin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------- Events -------------------------------- */

typedef enum {
  ESY_PROTO_BIN_TP_EVENT_ACK = 0,
  ESY_PROTO_BIN_TP_EVENT_NACK,
  ESY_PROTO_BIN_TP_EVENT_TIMEOUT,
  ESY_PROTO_BIN_TP_EVENT_RX_CRC_ERROR,
  ESY_PROTO_BIN_TP_EVENT_IO_ERROR,
  ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT,
} esy_proto_bin_tp_event_t;

#define ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES (2u * sizeof(uint32_t))
#define ESY_PROTO_BIN_INVALID_TRANSACTION_ID 0u
/** @brief Default maximum RX bytes consumed by one transport poll. */
#define ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET 256u

/**
 * @brief Receive a ProtoBin transport event.
 *
 * @param[in] event Transport event.
 * @param[in] transaction_id Related TX transaction ID, or zero for RX-only
 * events.
 * @param[in] user_data Callback context.
 */
typedef void (*esy_proto_bin_tp_event_cb_t)(
    esy_proto_bin_tp_event_t event, uint32_t transaction_id, void *user_data);

/**
 * @brief Data callback: received a nonduplicate complete ProtoBin DATA frame.
 *
 * Note: frame memory is only valid during callback (backed by rx_work_buf).
 */
typedef void (*esy_proto_bin_tp_data_cb_t)(
    const esy_proto_bin_frame_t *frame, void *user_data);

/* ----------------------------- I/O interface ------------------------------ */

/**
 * @brief Underlying byte-stream I/O adapter.
 *
 * send, receive and is_rx_ready are required. is_rx_ready reports whether one
 * byte can be read without blocking; receive consumes at most one byte.
 * send must either accept the complete frame or return an error without
 * borrowing its storage. ESY_ERR_BUSY rejects this attempt temporarily: the
 * queued payload, sequence and retry budget are retained. No timeout starts
 * until a send succeeds. A retry waiting for capacity keeps its expired ACK
 * wait inactive; an accepted retry explicitly arms a new wait.
 * If is_tx_busy is NULL, send must copy or finish using
 * all frame bytes before returning. Asynchronous senders provide is_tx_busy and
 * retain the borrow until it becomes false. All hooks run on the poll owner.
 */
typedef struct {
  /** Optional; NULL declares synchronous copy/completion by send. */
  bool (*is_tx_busy)(void *user_data);
  /** Required non-blocking receive-readiness query. */
  bool (*is_rx_ready)(void *user_data);

  esy_err_t (*send)(void *user_data, const uint8_t *data, uint32_t length);
  bool (*receive)(void *user_data, uint8_t *byte);
} esy_proto_bin_tp_io_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_PROTO_BIN_TYPES_H */
