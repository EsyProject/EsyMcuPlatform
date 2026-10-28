/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_BIN_USB_CHANNEL_H
#define ESY_PROTO_BIN_USB_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/protocol/esy_proto_bin_session.h"
#include "esy_middleware/usb/esy_usb_cdc.h"

/**
 * @par Lifecycle and execution context
 * Zero-initialize every handle before first use. Serialize calls and callbacks
 * on one owner task; these APIs are not ISR-safe. Init on an active handle
 * returns ESY_ERR_STATE; failed init retains no resources unless documented
 * otherwise. Caller-owned storage and callback contexts outlive successful
 * deinit. Stop physical I/O before deinitializing a raw session.
 * Callbacks may reserve/commit/cancel TX, but must not recursively poll, reset,
 * init or deinit the object. Session-reset callbacks must not enqueue TX.
 * Timeouts are milliseconds in 1..INT32_MAX and require periodic polling.
 * Payload limits are 1..UINT16_MAX, further limited by the physical channel.
 * The caller must configure the exclusive CDC TX FIFO to hold tx_payload_max
 * plus ESY_PROTO_BIN_OVERHEAD_BYTES. Capacity cannot be checked through CDC's
 * current-free-byte API; temporary FIFO occupancy is ordinary backpressure.
 * Leave the CDC RX callback NULL to preserve bytes for owner-task polling. A
 * custom RX callback must leave protocol bytes in the FIFO for this owner.
 * Initialize CDC with discard_on_disconnect=true and keep it initialized until
 * this channel has been deinitialized. DTR close or an observed
 * physical disconnect ends the peer session: queued/in-flight TX receive
 * IO_ERROR, reservations and partial RX are discarded, and RX deduplication
 * resets. Old CDC software FIFOs are cleared at close, before a new peer's
 * bytes arrive. Opening never flushes new RX. Already submitted USB packets
 * cannot be recalled; physical bus reset must also discard provider state.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_proto_bin_usb_channel.h
 * @brief Persistent ProtoBin transport channel over USB CDC.
 *
 * USB provides reliable byte delivery, but CDC remains a byte stream without
 * application frame boundaries. This channel preserves the existing ProtoBin
 * wire format while separating USB ownership from OTA. Products that do not
 * require protocol-level retransmission should configure retries as zero.
 */

#ifndef ESY_PROTO_BIN_USB_CHANNEL_CTX_BYTES
#define ESY_PROTO_BIN_USB_CHANNEL_CTX_BYTES 1024u
#endif

typedef struct {
  union {
    alignas(ESY_PROTO_BIN_SESSION_ALIGNMENT) max_align_t _align;
    uint8_t bytes[ESY_PROTO_BIN_USB_CHANNEL_CTX_BYTES];
  } priv;
} esy_proto_bin_usb_channel_t;

typedef struct {
  uint8_t *tx_queue_storage;
  size_t tx_queue_storage_size;
  uint8_t *tx_work_buf;
  size_t tx_work_buf_size;
  uint8_t *rx_work_buf;
  size_t rx_work_buf_size;
} esy_proto_bin_usb_channel_static_resources_t;

typedef struct {
  esy_usb_cdc_t *cdc;
  uint32_t tx_payload_max;
  uint32_t rx_payload_max;
  uint32_t tx_completion_timeout_ms; /**< CDC write acceptance to queue release. */
  uint32_t acknowledgement_timeout_ms; /**< CDC queue release to ProtoBin ACK. */
  uint32_t retries;
  uint32_t tx_queue_block_count;
  esy_proto_bin_tp_data_cb_t data_cb;
  esy_proto_bin_tp_event_cb_t event_cb;
  void *callback_user_data;
  esy_proto_bin_usb_channel_static_resources_t *static_resources;
} esy_proto_bin_usb_channel_config_t;

void esy_proto_bin_usb_channel_default_cfg(
    esy_proto_bin_usb_channel_config_t *config);

/** @brief Bind an initialized CDC instance with discard_on_disconnect enabled.
 * Buffers, configuration and resource descriptors must be pairwise disjoint
 * from each other and the whole channel handle. Wrapping address ranges are
 * rejected before provider I/O or handle modification.
 * @param[in,out] channel Zero-initialized caller-owned handle.
 * @param[in] config Configuration and caller-owned backing storage.
 * @return ESY_OK, ESY_ERR_INVALID_ARG for invalid storage/configuration or a CDC
 * instance without session isolation, or ESY_ERR_STATE for an active handle. */
esy_err_t esy_proto_bin_usb_channel_init(
    esy_proto_bin_usb_channel_t *channel,
    const esy_proto_bin_usb_channel_config_t *config);

/** @brief Pump USB and advance one bounded ProtoBin poll on its owner task.
 * A disconnected channel does not consume RX or advance TX timeouts. Session
 * reset reports at most tx_queue_block_count queued transaction callbacks;
 * ordinary RX work is limited by ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET. USB
 * provider and application callback durations remain the owner's responsibility.
 * @param[in,out] channel Initialized channel.
 * @return ESY_OK, ESY_ERR_STATE for NULL/inactive, or BUSY on recursive poll. */
esy_err_t esy_proto_bin_usb_channel_poll(esy_proto_bin_usb_channel_t *channel);

/** @brief Stop protocol progression and release copied-FIFO references.
 * Returns OK for NULL/inactive handles, BUSY during poll/callback reentry.
 * Failed cleanup retains the channel and every backing buffer. This does not
 * close CDC or flush/acknowledge its pending bytes. */
esy_err_t esy_proto_bin_usb_channel_deinit(esy_proto_bin_usb_channel_t *channel);

/** @brief Borrow the initialized session, or NULL. Use it for payload
 * reservation/submission/cancellation and queries; let the channel alone drive,
 * reset and deinitialize its session. */
esy_proto_bin_session_t *esy_proto_bin_usb_channel_get_session(
    esy_proto_bin_usb_channel_t *channel);

#ifdef __cplusplus
}
#endif

#endif
