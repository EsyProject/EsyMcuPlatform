/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_spi_scheduler_core.h
 * @brief Caller-driven SPI request engine, shared by the optional OS adapter.
 */
#ifndef ESY_SPI_SCHEDULER_CORE_H
#define ESY_SPI_SCHEDULER_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/ds/esy_block_ring.h"
#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_dma.h"
#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief SPI scheduler pre-transfer callback.
 *
 * Called right before the SPI transfer is started (e.g., before asserting CS,
 * starting DMA, etc.). The callback should be lightweight.
 *
 * @param[in] arg User-provided callback argument.
 */
typedef void (*esy_spi_scheduler_pre_callback_t)(void *arg);

/* -------------------------------------------------------------------------- */
/* Request type                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief SPI scheduler transfer request.
 *
 * The request describes a single SPI transfer. Buffers can be:
 * - Externally provided by the caller via tx_buf/rx_buf, or
 * - Caller-owned when configured and exposed via tx_alloc_buf/rx_alloc_buf.
 *
 * If pre-allocation is enabled, the scheduler may automatically set
 * tx_buf/rx_buf to point to the corresponding alloc buffers when a write block
 * is acquired.
 */
typedef struct {
    /**
     * @brief Actual TX buffer pointer for the transfer.
     *
     * May point to @ref tx_alloc_buf if pre-allocation is enabled,
     * or to a caller-provided buffer.
     *
     * May be NULL if this transfer is RX-only (implementation-dependent).
     */
    uint8_t *tx_buf;

    /**
     * @brief Actual RX buffer pointer for the transfer.
     *
     * May point to @ref rx_alloc_buf if pre-allocation is enabled,
     * or to a caller-provided buffer.
     *
     * May be NULL if this transfer is TX-only (implementation-dependent).
     */
    uint8_t *rx_buf;

    /**
     * @brief Pre-allocated TX buffer for this request block (optional).
     *
     * Non-NULL only if the scheduler is configured with tx_alloc_size > 0.
     * Ownership remains with the caller; do not free while the scheduler is alive.
     */
    uint8_t *tx_alloc_buf;

    /**
     * @brief Pre-allocated RX buffer for this request block (optional).
     *
     * Non-NULL only if the scheduler is configured with rx_alloc_size > 0.
     * Ownership remains with the caller; do not free while the scheduler is alive.
     */
    uint8_t *rx_alloc_buf;

    /**
     * @brief Transfer length in bytes.
     *
     * Must be > 0. The maximum length may be limited by the SPI/DMA HAL backend.
     */
    size_t   len;

    /**
     * @brief DMA transfer unit size.
     *
     * Specifies DMA element size used by the transfer (byte/halfword/word, etc.).
     * Must be supported by the DMA/SPI backend.
     */
    esy_dma_transfer_size_t transfer_size;

    /**
     * @brief Pre-transfer callback (optional).
     *
     * Called in the scheduler owner context right before starting the transfer.
     */
    esy_spi_scheduler_pre_callback_t  pre_callback;

    /**
     * @brief Argument passed to @ref pre_callback.
     */
    void    *pre_callback_arg;

    /** @brief Optional terminal callback reporting status and received bytes. */
    esy_spi_transfer_done_cb_t result_callback;
    /** @brief Borrowed terminal callback context. */
    void *result_callback_arg;

    /**
     * @brief Optional owner-context preparation; error skips the payload.
     * Requires a TX-only request (tx_buf non-NULL, rx_buf NULL) and prepared_done.
     */
    esy_spi_prepare_cb_t prepare;
    /** @brief Terminal callback for a prepared transaction; required with prepare. */
    esy_spi_prepared_done_cb_t prepared_done;
    /** @brief Caller-owned context for prepare and prepared_done. */
    void *prepared_arg;
  /** @brief Terminal status, readable only during result_callback; STATE while pending. */
  esy_err_t result;
  /** @brief Completed bytes, readable during result_callback; zero if unknown on error. */
  size_t transferred;
} esy_spi_scheduler_request_t;

/**
 * @brief SPI scheduler opaque handle.
 */
typedef struct esy_spi_scheduler_s esy_spi_scheduler_t;

#define ESY_SPI_SCHEDULER_STORAGE_BYTES (384u)

/**
 * @note Initialize this storage to {0} before first creation. Repeated create
 * returns ESY_ERR_STATE while resources are owned. Never clear live storage.
 * Successful destroy permits reuse; a failed destroy retains ownership.
 * Serialize create/destroy and producer operations; do not destroy in callbacks.
 * Rebuild all consumers after storage alignment or request layout changes.
 * @brief Caller-owned opaque storage for SPI scheduler creation.
 */
typedef struct {
    alignas(esy_block_ring_t) uint8_t bytes[ESY_SPI_SCHEDULER_STORAGE_BYTES];
} esy_spi_scheduler_storage_t;

/** @brief Caller-owned queue and optional per-request payload storage. */
typedef struct {
    /** @brief Caller-owned request block storage. */
    esy_spi_scheduler_request_t *request_storage;

    /** @brief Number of entries available in @ref request_storage. */
    uint32_t request_storage_count;

    /** @brief Caller-owned TX buffer storage. */
    uint8_t *tx_buffer_storage;

    /** @brief Size of @ref tx_buffer_storage in bytes. */
    size_t tx_buffer_storage_size;

    /** @brief Caller-owned RX buffer storage. */
    uint8_t *rx_buffer_storage;

    /** @brief Size of @ref rx_buffer_storage in bytes. */
    size_t rx_buffer_storage_size;

} esy_spi_scheduler_core_resources_t;

/** @brief Scheduling-independent storage configuration. */
typedef struct {
  uint32_t tx_alloc_size; /**< Per-request TX bytes; zero disables allocation. */
  uint32_t rx_alloc_size; /**< Per-request RX bytes; zero disables allocation. */
  uint32_t block_count; /**< Power-of-two request capacity, at least two. */
  esy_spi_scheduler_core_resources_t static_resources; /**< Borrowed storage. */
} esy_spi_scheduler_core_config_t;

/** @brief Fill defaults; caller supplies queue and optional payload storage. */
void esy_spi_scheduler_core_default_cfg(esy_spi_scheduler_core_config_t *cfg);
/**
 * @brief Create a caller-driven instance with no worker task.
 * @note One non-ISR owner serializes create/process/destroy and producer calls.
 * DMA callbacks only publish completion. Keep SPI, requests, payloads and callback
 * contexts alive until destroy succeeds. All application callbacks run inside
 * process; do not recursively process/destroy the instance from them.
 * @param spi Initialized, borrowed SPI handle with exclusive transport ownership.
 * @param cfg Queue and optional per-request payload configuration.
 * @param storage Zero-initialized opaque handle backing storage.
 * @param out_gw Receives the instance on success.
 * @retval ESY_OK Core is ready for submissions and processing.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration, storage or ISR call.
 * @retval ESY_ERR_STATE Storage already belongs to a live instance.
 * @retval ESY_ERR_BUSY SPI transport is already active.
 * @retval <0 Queue initialization failure; no resources are retained.
 */
esy_err_t esy_spi_scheduler_core_create(esy_spi_t *spi,
    const esy_spi_scheduler_core_config_t *cfg, esy_spi_scheduler_storage_t *storage,
    esy_spi_scheduler_t **out_gw);
/**
 * @brief Poll once, retire at most one prior request and start at most one next.
 * @note May also complete that next request synchronously. Provider calls,
 * including hardware cleanup/abort, blocking fallback and callbacks determine
 * call duration: this is a
 * request budget, not a time deadline. Poll regularly to retire DMA and meet
 * transport latency. No delay, wait or OS operation is performed by the engine.
 * Returns OK for a valid idle/quiesced instance; STATE for an OS-owned instance
 * or reentrant call. During a BUSY destroy, continue processing to drain active
 * I/O, then retry destroy. Every accepted request must finish before stop is accepted.
 * A failed DMA submission that remains busy retains its buffers, prepared
 * resources and queue slot. Later steps poll for terminal retirement, then try
 * abort once if no terminal callback arrived. Failed cleanup retains ownership;
 * successful abort cancels silently and completes the request with its submit
 * error. Only an idle unsupported rejection may use the blocking fallback.
 * @param gw Caller-driven scheduler.
 * @retval ESY_OK A valid idle, quiesced or active step completed.
 * @retval ESY_ERR_INVALID_ARG NULL handle or ISR call.
 * @retval ESY_ERR_STATE Inactive, OS-owned or reentrant instance.
 */
esy_err_t esy_spi_scheduler_core_process(esy_spi_scheduler_t *gw);
/**
 * @brief Destroy an SPI Scheduler instance.
 *
 * @param[in,out] gw Scheduler instance pointer (may be NULL).
 *
 * @retval ESY_OK       Instance is fully destroyed, or @p gw is NULL.
 * @retval ESY_ERR_BUSY A transfer could not be stopped within the current processing step; resources are left owned by @p gw and
 *                      the caller may retry destroy later. Accepted requests must finish before destroy; BUSY leaves the execution owner able to drain.
 * @note Stop producers first. Do not call destroy from a scheduler callback.
 */
extern esy_err_t esy_spi_scheduler_destroy(esy_spi_scheduler_t *gw);

/** @brief Atomically prevent new transfers for a system suspend attempt. */
extern esy_err_t esy_spi_scheduler_quiesce(esy_spi_scheduler_t *gw);

/** @brief Resume queued transfer processing after wake or suspend rollback. */
extern esy_err_t esy_spi_scheduler_resume(esy_spi_scheduler_t *gw);

/* -------------------------------------------------------------------------- */
/* Request enqueue API                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Acquire a writable request block.
 *
 * Returns a pointer to a request object that can be filled by the caller.
 * If pre-allocation is enabled, the scheduler will automatically set:
 * - req->tx_alloc_buf / req->rx_alloc_buf to the pre-allocated buffers, and
 * - req->tx_buf / req->rx_buf to point to those buffers by default.
 *
 * @param[in,out] gw Scheduler instance pointer.
 *
 * @return Pointer to a writable request block; NULL if the queue is full or
 *         @p gw is invalid/not initialized.
 *
 * @note The returned block must be submitted via @ref esy_spi_scheduler_submit
 *       to become visible to the execution owner.
 */
extern esy_spi_scheduler_request_t *
esy_spi_scheduler_get_write_block(esy_spi_scheduler_t *gw);

/**
 * @brief Submit a request and trigger asynchronous transfer.
 *
 * @param[in,out] gw  Scheduler instance pointer.
 * @param[in]     req Request block pointer previously obtained from
 *                    @ref esy_spi_scheduler_get_write_block.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (e.g. ESY_ERR_INVALID_ARGS, ESY_ERR_STATE, ESY_ERR_BUSY).
 *
 * @warning The request object belongs to the scheduler ring. Do not reuse or
 *          modify the request after submission unless the implementation
 *          explicitly allows it.
 */
extern esy_err_t esy_spi_scheduler_submit(esy_spi_scheduler_t *gw,
                                       esy_spi_scheduler_request_t *req);

/* -------------------------------------------------------------------------- */
/* Convenience APIs (for LCD/Touch-like use cases)                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get pre-allocated TX/RX buffers and return an error status.
 *
 * If the scheduler instance (identified by @p arg) is configured with pre-allocated
 * buffers, this function returns pointers that the caller can fill directly.
 *
 * @param[out] tx_buf Returned TX buffer pointer (may be NULL if TX is not allocated).
 * @param[out] rx_buf Returned RX buffer pointer (may be NULL if RX is not allocated).
 * @param[in]  arg    User argument that associates the call with an internal
 *                    scheduler instance (multi-instance support is implementation-defined).
 *
 * @retval ESY_OK              Writable block reserved successfully.
 * @retval ESY_ERR_INVALID_ARG Invalid scheduler or output pointer arguments.
 * @retval ESY_ERR_STATE       A writable block is already reserved and must be
 *                             submitted first.
 * @retval ESY_ERR_BUSY        No writable block is currently available.
 */
extern esy_err_t esy_spi_scheduler_get_write_buffer_with_arg(
    uint8_t **tx_buf,
    uint8_t **rx_buf,
    void *arg);

/**
 * @brief Reserve full-duplex buffers after checking both writable capacities.
 *
 * Use this adapter for sized touch-provider callbacks. It checks the scheduler's
 * per-request TX/RX allocation sizes before reserving a request. On success,
 * fill the returned buffers and submit through
 * @ref esy_spi_scheduler_submit_result_with_arg. Buffers remain provider-owned
 * through the terminal callback. Serialize with the scheduler producer endpoint;
 * do not call from ISR. An error clears both outputs, reserves nothing new and
 * preserves any existing reservation.
 *
 * @param[out] tx_buf Required output for at least minimum_size writable bytes.
 * @param[out] rx_buf Required output for at least minimum_size writable bytes.
 * @param[in] minimum_size Nonzero required capacity in each buffer.
 * @param[in] arg Initialized scheduler instance.
 * @retval ESY_OK Buffers reserved successfully.
 * @retval ESY_ERR_INVALID_ARG NULL argument or zero minimum_size.
 * @retval ESY_ERR_STATE Inactive scheduler or an existing producer reservation.
 * @retval ESY_ERR_NO_MEM Either allocation is smaller than minimum_size.
 * @retval ESY_ERR_BUSY No writable request is available or the scheduler is stopping.
 */
extern esy_err_t esy_spi_scheduler_get_read_write_buffer_with_arg(
    uint8_t **tx_buf, uint8_t **rx_buf, size_t minimum_size, void *arg);

/** @brief Queue a byte transfer, including RX-only/full duplex.
 * done is required; uses the same buffer reservation and ownership rules.
 * Accepted requests receive exactly one terminal callback, including failure.
 * A failed DMA submit that stays busy delays this callback until poll retirement
 * or successful abort; buffers/context remain borrowed throughout cleanup.
 * Destroy/quiesce return BUSY until these requests drain. Errors returned
 * directly retain no internally reserved block. Buffers/context remain borrowed
 * through callback return, and callbacks run in the scheduler owner. Serialize
 * producers and lifecycle; never call from ISR.
 * @param[in] tx TX bytes, or NULL for RX only; unchanged until completion.
 * @param[out] rx RX bytes, or NULL for TX only; valid only when done receives OK.
 * @param[in] len Nonzero byte count, at most INT32_MAX and any allocated capacity.
 * @param[in] pre Optional bounded preparation callback.
 * @param[in] pre_arg Borrowed preparation context.
 * @param[in] done Required terminal callback; RX is NULL on failure.
 * @param[in] done_arg Borrowed terminal context.
 * @param[in] arg Initialized scheduler instance.
 * @return ESY_OK on acceptance, INVALID_ARG for invalid inputs, STATE for a
 * conflicting reservation, or BUSY for a full queue/quiesced scheduler.
 */
esy_err_t esy_spi_scheduler_submit_result_with_arg(uint8_t *tx, uint8_t *rx, uint32_t len,
    void (*pre)(void *), void *pre_arg, esy_spi_transfer_done_cb_t done,
    void *done_arg, void *arg);

/**
 * @brief Queue one TX-only prepare/payload/completion transaction.
 *
 * Implements esy_spi_submit_prepared_t. Reserves one ring slot without using
 * preallocated payload buffers. The owner invokes prepare once under exclusive
 * bus ownership, sends the payload only on success, then invokes done exactly
 * once, including failures. Both callbacks run in the scheduler owner. There is no
 * retry after preparation: an error terminates the transaction. A completion
 * may enqueue another request but must not destroy the scheduler.
 *
 * @param[in] tx_buf Caller-owned immutable bytes retained until done starts.
 * @param[in] len Nonzero payload length in bytes.
 * @param[in] prepare Required bounded owner-context bus preparation callback.
 * @param[in] done Required terminal callback reporting ESY_OK or an error.
 * @param[in] callback_arg Caller-owned context retained until done starts.
 * @param[in] arg Scheduler instance. Submit calls must be externally serialized.
 * @retval ESY_OK Entire transaction accepted; notification errors do not revoke it.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer, length, or stopped provider.
 * @retval ESY_ERR_STATE A writable block is already reserved.
 * @retval ESY_ERR_BUSY Queue full or provider quiesced; no callbacks will run.
 * @note Drain all accepted requests before quiesce/destroy. Direct SPI
 * users must also serialize against the execution owner. No ISR call is permitted.
 */
extern esy_err_t esy_spi_scheduler_submit_prepared_with_arg(
    const uint8_t *tx_buf, uint32_t len, esy_spi_prepare_cb_t prepare,
    esy_spi_prepared_done_cb_t done, void *callback_arg, void *arg);

#ifdef __cplusplus
}
#endif
#endif /* ESY_SPI_SCHEDULER_CORE_H */
