/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_BIN_UART_CHANNEL_H
#define ESY_PROTO_BIN_UART_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/protocol/esy_proto_bin_session.h"
#include "esy_hal/esy_hal_uart.h"

/**
 * @par Lifecycle and execution context
 * Buffers, configuration and resource descriptors must be pairwise disjoint
 * from each other and the whole channel handle. Wrapping address ranges are
 * rejected before provider I/O or handle modification.
 * Zero-initialize every handle before first use. Serialize calls and callbacks
 * on one owner task; these APIs are not ISR-safe. Init on an active handle
 * returns ESY_ERR_STATE; failed init retains no resources unless documented
 * otherwise. Caller-owned storage and callback contexts outlive successful
 * deinit. Stop physical I/O before deinitializing a raw session.
 * Callbacks may reserve/commit/cancel TX, but must not recursively poll, reset,
 * init or deinit the object. Session-reset callbacks must not enqueue TX.
 * Timeouts are milliseconds in 1..INT32_MAX and require periodic polling.
 * Payload limits are 1..UINT16_MAX, further limited by the physical channel.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_proto_bin_uart_channel.h
 * @brief ProtoBin reliable transport channel over an asynchronous UART stream.
 *
 * The channel owns no task and allocates no memory. The caller provides all
 * storage, keeps it valid until deinit, and periodically calls poll from one
 * task context. Received frames and transport events are delivered
 * synchronously from poll.
 */

/** @brief Opaque context capacity including its private TX scheduler and queue.
 * Rebuild consumers after changing this size. */
#ifndef ESY_PROTO_BIN_UART_CHANNEL_CTX_BYTES
#define ESY_PROTO_BIN_UART_CHANNEL_CTX_BYTES 1280u
#endif

/** @brief Caller-owned opaque UART channel context. */
typedef struct {
  union {
    alignas(ESY_PROTO_BIN_SESSION_ALIGNMENT) max_align_t _align;
    uint8_t bytes[ESY_PROTO_BIN_UART_CHANNEL_CTX_BYTES];
  } priv;
} esy_proto_bin_uart_channel_t;

/** @brief Caller-owned buffers used by one UART channel instance. */
typedef struct {
  uint8_t *tx_queue_storage;
  size_t tx_queue_storage_size;
  uint8_t *tx_work_buf;
  size_t tx_work_buf_size;
  uint8_t *rx_work_buf;
  size_t rx_work_buf_size;
  uint8_t *rx_stream_storage;
  size_t rx_stream_storage_size; /**< Capacity accepted by
      @ref esy_hal_uart_rx_stream_start. */
} esy_proto_bin_uart_channel_static_resources_t;

/**
 * @brief Declare caller-owned UART channel storage as one object.
 *
 * The object declared by this macro must remain valid from successful init
 * until deinit. Use @ref ESY_PROTO_BIN_UART_CHANNEL_BIND_STATIC_RESOURCES to bind
 * the object to an @ref esy_proto_bin_uart_channel_static_resources_t instance.
 *
 * @param name                 Object name to declare.
 * @param tx_queue_block_count Number of queued transmit blocks.
 * @param tx_payload_max       Maximum transmit payload bytes.
 * @param rx_payload_max       Maximum receive payload bytes.
 * @param rx_stream_bytes      Continuous receive stream storage bytes; must
 *                            satisfy @ref esy_hal_uart_rx_stream_start capacity
 *                            requirements.
 */
#define ESY_PROTO_BIN_UART_CHANNEL_STATIC_STORAGE(name, tx_queue_block_count,     \
                                              tx_payload_max, rx_payload_max, \
                                              rx_stream_bytes)                \
  struct {                                                                    \
    uint8_t tx_queue_storage[(tx_queue_block_count) *                         \
                             ((tx_payload_max) +                              \
                              ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES)];        \
    uint8_t tx_work_buf[(tx_payload_max) + ESY_PROTO_BIN_OVERHEAD_BYTES];     \
    uint8_t rx_work_buf[(rx_payload_max) + ESY_PROTO_BIN_OVERHEAD_BYTES];     \
    uint8_t rx_stream_storage[(rx_stream_bytes)];                             \
  } name

/**
 * @brief Bind caller-owned UART channel storage to a resource descriptor.
 *
 * @param resources Pointer to
 *                  @ref esy_proto_bin_uart_channel_static_resources_t.
 * @param storage   Object declared by
 *                  @ref ESY_PROTO_BIN_UART_CHANNEL_STATIC_STORAGE.
 */
#define ESY_PROTO_BIN_UART_CHANNEL_BIND_STATIC_RESOURCES(resources, storage)       \
  do {                                                                         \
    (resources)->tx_queue_storage = (storage).tx_queue_storage;                \
    (resources)->tx_queue_storage_size = sizeof((storage).tx_queue_storage);   \
    (resources)->tx_work_buf = (storage).tx_work_buf;                          \
    (resources)->tx_work_buf_size = sizeof((storage).tx_work_buf);             \
    (resources)->rx_work_buf = (storage).rx_work_buf;                          \
    (resources)->rx_work_buf_size = sizeof((storage).rx_work_buf);             \
    (resources)->rx_stream_storage = (storage).rx_stream_storage;              \
    (resources)->rx_stream_storage_size = sizeof((storage).rx_stream_storage); \
  } while (0)

/** @brief UART channel configuration. */
typedef struct {
  esy_uart_t *uart;
  uint32_t tx_payload_max;
  uint32_t rx_payload_max;
  uint32_t tx_completion_timeout_ms; /**< Queue acceptance to UART wire idle. */
  uint32_t acknowledgement_timeout_ms; /**< UART line idle to ProtoBin ACK. */
  uint32_t retries;
  uint32_t tx_queue_block_count;
  esy_proto_bin_tp_data_cb_t data_cb;
  esy_proto_bin_tp_event_cb_t event_cb;
  void *callback_user_data;
  esy_proto_bin_uart_channel_static_resources_t *static_resources;
} esy_proto_bin_uart_channel_config_t;

/**
 * @brief Clear a UART channel configuration.
 *
 * @param[out] config Configuration to clear; NULL is accepted.
 */
void esy_proto_bin_uart_channel_default_cfg(
    esy_proto_bin_uart_channel_config_t *config);

/**
 * @brief Initialize a channel and start continuous UART reception.
 *
 * The serialized UART owner must supply an idle RX stream. An existing stream
 * is rejected without stopping it. Failed starts may retain storage/callbacks:
 * cleanup stops RX before release. If cleanup fails, retain the channel and all
 * backing resources and retry deinit; poll/transport access remain unavailable.
 *
 * @param[in,out] channel Caller-owned opaque channel storage.
 * @param[in] config Channel configuration and static resources.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration or storage.
 * @retval <0 Underlying transport or UART stream startup failure.
 */
esy_err_t esy_proto_bin_uart_channel_init(
    esy_proto_bin_uart_channel_t *channel,
    const esy_proto_bin_uart_channel_config_t *config);

/**
 * @brief Process available RX bytes, retries, timeouts, and queued TX.
 *
 * @param[in,out] channel Initialized channel.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_STATE Channel is not active.
 * @note A private UART TX scheduler owns physical queueing, DMA and retirement.
 * One slice retires prior TX before protocol processing; a second slice runs
 * only to start a newly enqueued frame. Each call performs at most two HAL polls,
 * one new DMA start attempt and one abort attempt. Queue acceptance remains busy until
 * the terminal callback and wire idle; only then does the session start its ACK
 * wait. Asynchronous failures complete the matching session transmission with
 * IO_ERROR after the scheduler callback has returned and propagate the first
 * error to this caller. A first-slice TX failure ends this poll before RX or new
 * TX; later queued protocol transactions resume on a subsequent successful poll.
 * Failed cleanup retains all backing storage. After deinit has requested stop,
 * poll advances only one bounded cleanup slice and never drives the session;
 * stop protocol producers until cleanup succeeds. An I/O event does not permit
 * reclaiming channel storage before successful cleanup.
 * @retval <0 UART retirement, asynchronous TX or stream recovery failure.
 */
esy_err_t esy_proto_bin_uart_channel_poll(esy_proto_bin_uart_channel_t *channel);

/**
 * @brief Request TX cancellation, retire one bounded slice and stop UART RX.
 *
 * Stop all session/sender users before calling deinit. Once closing begins,
 * get_session returns NULL; previously borrowed pointers/descriptors must no
 * longer enqueue or drive work. Preserve their storage until cleanup succeeds.
 * A callback-rejected deinit leaves the active channel unchanged.
 *
 * @param[in,out] channel Channel to stop.
 *
 * @retval ESY_OK Stopped or already inactive.
 * @retval <0 UART TX retirement/abort or RX stream stop failure; channel and
 * storage remain owned for poll/deinit retry. Each call runs at most one TX
 * scheduler slice; queued frames are cancelled without starting DMA. Closing
 * suppresses session completion notifications and does not fabricate I/O errors.
 */
esy_err_t esy_proto_bin_uart_channel_deinit(esy_proto_bin_uart_channel_t *channel);

/**
 * @brief Get the channel transport facade.
 *
 * The returned pointer is borrowed and remains valid only while the channel is
 * active and has not begun closing. Previously borrowed session/sender users
 * must stop before deinit and retain backing storage through cleanup.
 *
 * @param[in,out] channel Initialized channel.
 * @return Transport pointer, or NULL when inactive.
 */
esy_proto_bin_session_t *esy_proto_bin_uart_channel_get_session(
    esy_proto_bin_uart_channel_t *channel);

#ifdef __cplusplus
}
#endif

#endif
