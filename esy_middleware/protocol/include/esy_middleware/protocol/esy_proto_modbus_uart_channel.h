/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_proto_modbus_uart_channel.h
 * @brief Caller-driven RTU master/slave transport; no OS is required.
 *
 * Zero opaque storage before first create. Repeated create while owned returns
 * STATE. A failed create with a non-NULL output retains resources: retry destroy
 * before reuse. Failed destroy retains UART, buffers and callbacks for retry;
 * only successful destroy permits storage reuse. Configuration is copied; borrowed
 * UART, queue buffers and callback context must outlive ownership. UART must provide an
 * exclusive RX stream and DMA TX; initialize it externally before creation.
 *
 * Caller-created instances have one serialized non-ISR owner for all APIs.
 * Completion/request callbacks run synchronously on that owner. Master completion callbacks may submit/abort;
 * recursive poll and destroy from any callback return STATE. Submit copies
 * the request and enqueues immediately; request on_complete/user_ctx must be NULL.
 * Abort/destroy can synchronously call up to queue_depth completion callbacks.
 * Do not block callbacks waiting for progress of this same channel.
 *
 * Poll consumes at most ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN+16 RX bytes, dispatches at most
 * one frame, advances one protocol tick and runs two UART TX slices. Queue scans
 * are bounded by configured queue_depth. Providers and application callbacks add
 * synchronous execution time; this is a work bound, not a wall-clock guarantee.
 * Call frequently enough to observe every RTU idle gap and avoid RX overflow;
 * late calls cannot reconstruct frame boundaries or per-byte arrival times.
 * HAL millisecond ticks must be monotonic modulo 2^32; scheduling gaps and
 * configured deadlines must remain below 2^31 ms. Response timeouts start after
 * physical TX completion; broadcast turnaround includes the RTU gap plus one tick.
 *
 * RX transport loss/read errors discard the partial frame and retry stream
 * recovery on subsequent calls; poll reports IO or the provider error. Protocol/transport
 * request results go to completion callbacks. No DMA polling wait occurs. After
 * destroy returns BUSY, retry poll/destroy to drain terminal TX notifications;
 * RX stop failures propagate without releasing callback or DMA storage.
 */
#ifndef ESY_PROTO_MODBUS_UART_CHANNEL_H
#define ESY_PROTO_MODBUS_UART_CHANNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/protocol/esy_proto_modbus_manager.h"
#include "esy_hal/esy_hal_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for a Modbus UART master channel.
 */
typedef struct esy_proto_modbus_uart_master_channel_s esy_proto_modbus_uart_master_channel_t;

/** @brief Opaque master capacity; rebuild consumers when implementation changes. */
#define ESY_PROTO_MODBUS_UART_MASTER_CHANNEL_STORAGE_BYTES (4096u)

/**
 * @brief Caller-owned opaque storage for Modbus UART master creation.
 */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_PROTO_MODBUS_UART_MASTER_CHANNEL_STORAGE_BYTES];
} esy_proto_modbus_uart_master_channel_storage_t;

/**
 * @brief Master request completion callback.
 *
 * @note The response PDU pointer typically refers to a temporary frame buffer
 *       and is only valid during the callback.
 *
 * @param[in] channel      Master channel handle.
 * @param[in] status       Request result status.
 * @param[in] unit_id      RTU slave address (Unit ID).
 * @param[in] func_code    Base function code (exception responses are mapped).
 * @param[in] resp_pdu     Response PDU pointer (FUNC + DATA), may be NULL on error.
 * @param[in] resp_pdu_len Response PDU length in bytes, 0 on error.
 * @param[in] user_ctx     User context pointer from configuration.
 */
typedef void (*esy_proto_modbus_uart_master_channel_complete_cb)(
    esy_proto_modbus_uart_master_channel_t *channel,
    esy_proto_modbus_req_status_t     status,
    uint8_t                           unit_id,
    esy_proto_modbus_func_t           func_code,
    const uint8_t                    *resp_pdu,
    uint16_t                          resp_pdu_len,
    void                             *user_ctx);

/** @brief Caller-owned master queue buffers; retain until destroy succeeds. */
typedef struct {
  esy_prio_slot_t *slot_storage; /**< Aligned priority slots. */
  uint16_t slot_storage_count; /**< At least queue_depth entries. */
  esy_proto_modbus_payload_t *payload_storage; /**< Aligned request payloads. */
  uint16_t payload_storage_count; /**< At least queue_depth entries. */
} esy_proto_modbus_uart_master_channel_static_resources_t;

/** @brief Allocate master queue resources inside the caller's context. */
#define ESY_PROTO_MODBUS_UART_MASTER_CHANNEL_STATIC_STORAGE(name, slot_count, payload_count) \
  struct { \
    esy_prio_slot_t slot_storage[(slot_count)]; \
    esy_proto_modbus_payload_t payload_storage[(payload_count)]; \
  } name

/** @brief RTU master configuration without task resources. */
typedef struct {
  esy_proto_modbus_frame_type_t frame_type; /**< RTU only. */
  esy_uart_t *uart; /**< Initialized exclusive UART with RX stream and DMA TX. */
  uint32_t timeout_ms; /**< Response timeout <= INT32_MAX; zero selects manager default. */
  uint8_t retry_max; /**< Default retry count. */
  uint32_t rtu_frame_timeout_ms; /**< Zero selects 10 ms; less than INT32_MAX. */
  uint16_t queue_depth; /**< Queue depth in 1..INT16_MAX. */
  esy_proto_modbus_uart_master_channel_static_resources_t static_resources; /**< Borrowed queues. */
  esy_proto_modbus_uart_master_channel_complete_cb on_complete; /**< Optional completion callback. */
  void *user_ctx; /**< Borrowed callback context. */
  uint32_t broadcast_turnaround_ms; /**< Zero selects 100 ms; less than INT32_MAX. */
} esy_proto_modbus_uart_master_channel_config_t;

/** @brief Return RTU defaults (100 ms timeout, 10 ms gap, 100 ms turnaround). */
esy_proto_modbus_uart_master_channel_config_t esy_proto_modbus_uart_master_channel_default_cfg(void);

/** @brief Create an owner-driven master; never creates or waits for a task.
 * @param cfg Valid UART, positive depth, and buffers of at least that depth.
 * @param storage Zero-initialized caller-owned storage; retain until destroyed.
 * @param out_gw Output, cleared on ordinary failure, retained if cleanup fails.
 * @return OK, INVALID_ARG, STATE, BUSY or provider initialization error.
 */
esy_err_t esy_proto_modbus_uart_master_channel_create(
    const esy_proto_modbus_uart_master_channel_config_t *cfg,
    esy_proto_modbus_uart_master_channel_storage_t *storage,
    esy_proto_modbus_uart_master_channel_t **out_gw);

/** @brief Advance one bounded owner slice, including pending stop cleanup.
 * @param gw Caller-driven master, called by its serialized non-ISR owner.
 * @return OK, INVALID_ARG, STATE on recursion, or RX/cleanup error.
 */
esy_err_t esy_proto_modbus_uart_master_channel_poll(esy_proto_modbus_uart_master_channel_t *gw);

/**
 * @brief Destroy a Modbus UART master channel and release resources.
 *
 * @param[in,out] gw Channel handle (may be NULL).
 *
 * @retval ESY_OK       Channel is fully destroyed, or @p gw is NULL.
 * @retval ESY_ERR_BUSY UART TX or its terminal notification remains pending.
 *                      Resources stay owned for poll/destroy retry.
 * @retval ESY_ERR_STATE Called recursively or from an owner callback.
 * @retval <0 Provider cleanup error; retain resources and retry.
 */
esy_err_t esy_proto_modbus_uart_master_channel_destroy(
    esy_proto_modbus_uart_master_channel_t *gw);

/**
 * @brief Check whether the master channel has been successfully initialized.
 *
 * @param[in] gw Channel handle.
 *
 * @return true if initialized; false otherwise.
 */
bool esy_proto_modbus_uart_master_channel_is_initialized(
    const esy_proto_modbus_uart_master_channel_t *gw);

/**
 * @brief Submit a Modbus request through the master channel.
 *
 * @param[in,out] gw  Master channel handle.
 * @param[in]     req Request descriptor (PDU must start from FUNC).
 *
 * @retval ESY_OK Success (request enqueued).
 * @retval <0     Failure (queue full, not ready, invalid args, etc.).
 */
esy_err_t esy_proto_modbus_uart_master_channel_submit(
    esy_proto_modbus_uart_master_channel_t *gw,
    const esy_proto_modbus_request_t *req);

/**
 * @brief Get the current request queue depth.
 *
 * @param[in] gw Master channel handle.
 *
 * @return Current number of queued requests.
 */
uint16_t esy_proto_modbus_uart_master_channel_get_queue_depth(
    const esy_proto_modbus_uart_master_channel_t *gw);

/**
 * @brief Get the current master protocol state.
 *
 * @param[in] gw Master channel handle.
 *
 * @return Current master state.
 */
esy_proto_modbus_master_state_t esy_proto_modbus_uart_master_channel_get_state(
    const esy_proto_modbus_uart_master_channel_t *gw);

/**
 * @brief Abort all pending Modbus transactions.
 *
 * @param[in,out] gw            Master channel handle.
 * @param[in]     abort_current Whether to abort the in-flight transaction.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
esy_err_t esy_proto_modbus_uart_master_channel_abort_all(
    esy_proto_modbus_uart_master_channel_t *gw,
    bool                              abort_current);

/**
 * @brief Opaque handle for a Modbus UART slave channel.
 */
typedef struct esy_proto_modbus_uart_slave_channel_s esy_proto_modbus_uart_slave_channel_t;

/** @brief Opaque slave capacity; rebuild consumers when implementation changes. */
#define ESY_PROTO_MODBUS_UART_SLAVE_CHANNEL_STORAGE_BYTES (3072u)

/**
 * @brief Caller-owned opaque storage for Modbus UART slave creation.
 */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_PROTO_MODBUS_UART_SLAVE_CHANNEL_STORAGE_BYTES];
} esy_proto_modbus_uart_slave_channel_storage_t;

/** @brief Synchronous slave request on the channel owner; response storage is borrowed.
 * The callback may fill at most response_capacity bytes and must not poll or destroy
 * the channel. Callback context remains borrowed until successful destroy.
 */
typedef esy_err_t (*esy_proto_modbus_uart_slave_channel_request_cb)(
    esy_proto_modbus_uart_slave_channel_t *channel, uint8_t unit_id,
    esy_proto_modbus_func_t function, const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t *response_length, uint16_t response_capacity,
    void *user_ctx);

/** @brief RTU slave configuration; all working buffers reside in opaque storage. */
typedef struct {
  esy_proto_modbus_frame_type_t frame_type; /**< RTU only. */
  esy_uart_t *uart; /**< Initialized exclusive UART with RX stream and DMA TX. */
  uint8_t unit_id; /**< RTU unit address; manager validation applies. */
  uint32_t rtu_frame_timeout_ms; /**< Zero selects 10 ms; at most INT32_MAX. */
  esy_proto_modbus_uart_slave_channel_request_cb on_request; /**< Mandatory bounded request callback. */
  void *user_ctx; /**< Borrowed callback context. */
} esy_proto_modbus_uart_slave_channel_config_t;

/** @brief Return RTU defaults (unit 1 and 10 ms idle gap). */
esy_proto_modbus_uart_slave_channel_config_t esy_proto_modbus_uart_slave_channel_default_cfg(void);

/** @brief Create an owner-driven slave without task resources or OS calls.
 * @param cfg Valid exclusive UART and request handler.
 * @param storage Zero-initialized storage retained until successful destroy.
 * @param out_gw Output; non-NULL on failure requires destroy retry.
 * @return OK, INVALID_ARG, STATE, BUSY or provider initialization error.
 */
esy_err_t esy_proto_modbus_uart_slave_channel_create(
    const esy_proto_modbus_uart_slave_channel_config_t *cfg,
    esy_proto_modbus_uart_slave_channel_storage_t *storage,
    esy_proto_modbus_uart_slave_channel_t **out_gw);

/** @brief Advance one bounded slave slice, including pending stop cleanup.
 * @param gw Caller-driven slave, called by its serialized non-ISR owner.
 * @return OK, INVALID_ARG, STATE on recursion, or RX/cleanup error.
 */
esy_err_t esy_proto_modbus_uart_slave_channel_poll(esy_proto_modbus_uart_slave_channel_t *gw);

/**
 * @brief Destroy a Modbus UART slave channel and release resources.
 *
 * @param[in,out] gw Channel handle (may be NULL).
 *
 * @retval ESY_OK       Channel is fully destroyed, or @p gw is NULL.
 * @retval ESY_ERR_BUSY UART TX or its terminal notification remains pending.
 *                      Resources stay owned for poll/destroy retry.
 * @retval ESY_ERR_STATE Called recursively or from an owner callback.
 * @retval <0 Provider cleanup error; retain resources and retry.
 */
esy_err_t esy_proto_modbus_uart_slave_channel_destroy(
    esy_proto_modbus_uart_slave_channel_t *gw);

/**
 * @brief Check whether the slave channel has been successfully initialized.
 *
 * @param[in] gw Channel handle.
 *
 * @return true if initialized; false otherwise.
 */
bool esy_proto_modbus_uart_slave_channel_is_initialized(
    const esy_proto_modbus_uart_slave_channel_t *gw);

/**
 * @brief Get the configured slave address (unit id).
 *
 * @param[in] gw Slave channel handle.
 *
 * @return RTU slave address (unit id). Returns 0 if @p gw is NULL or not initialized.
 */
uint8_t esy_proto_modbus_uart_slave_channel_get_addr(
    const esy_proto_modbus_uart_slave_channel_t *gw);

#ifdef __cplusplus
}
#endif
#endif
