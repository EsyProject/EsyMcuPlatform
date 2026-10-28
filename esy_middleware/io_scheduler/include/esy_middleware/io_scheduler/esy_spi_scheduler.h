/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SPI_SCHEDULER_H
#define ESY_SPI_SCHEDULER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdalign.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "esy_common/ds/esy_block_ring.h"
#include "esy_spi_scheduler_core.h"
#include "esy_middleware/os/esy_os_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_spi_scheduler.h
 * @par Lifecycle and concurrency
 * Zero-initialize scheduler storage before first create. Preserve all storage on
 * destroy failure and retry; do not create over a live handle. All APIs are task
 * context only. Serialize producers and lifecycle operations. The worker owns
 * I/O and callbacks; callers own the HAL handle and must exclude other I/O.
 * Callbacks must return promptly and must not destroy/quiesce this scheduler.
 * Create requires idle I/O and cooperative parking support. Failed create with
 * no output retains no resources. In-flight DMA must complete before destruction.
 * Successful destroy confirms worker parking before releasing static resources.
 * Request result/transferred fields are valid during result_callback only; capture
 * the borrowed request in result_callback_arg to inspect them. The callback reports terminal
 * completion, including failure, rather than unconditional transfer success.
 * Every accepted request must drain before quiesce or destruction succeeds.

 * @brief SPI Scheduler middleware interface.
 *
 * This module provides an asynchronous SPI transfer queue driven by a background
 * task and DMA, supporting:
 * - Pre-allocated per-request TX/RX buffers (optional)
 * - Per-transfer preparation and terminal result callbacks
 * - Ring-based request storage (no dynamic resizing)
 *
 * Typical use cases:
 * - High-rate SPI sampling pipelines
 * - Display / touch controllers (LCD/Touch) that benefit from queued transfers
 *
 * Concurrency model:
 * - Producer: one application thread enqueues requests through get/submit.
 * - Consumer: a dedicated scheduler task dequeues requests and performs transfers.
 * - Multiple producers must be serialized by the caller before entering this API.
 */

/* -------------------------------------------------------------------------- */
/* Callback types                                                             */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Caller-owned static resources for an SPI scheduler instance.
 *
 * These resources must remain alive until the scheduler is destroyed.
 */
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

    /**
     * @brief Caller-owned background task stack buffer.
     *
     * Must be aligned to @ref ESY_OS_TASK_STACK_ALIGN.
     */
    void *task_stack_buffer;

    /** @brief Size of @ref task_stack_buffer in bytes. */
    size_t task_stack_buffer_size;

    /** @brief Caller-owned static task control storage. */
    esy_os_task_static_storage_t *task_control_storage;
} esy_spi_scheduler_static_resources_t;

/**
 * @brief Declare app-owned static storage for an SPI scheduler instance.
 *
 * The declared object aggregates request blocks, optional TX/RX payload buffer
 * backing storage, task stack, and task control storage so application context
 * structures can keep scheduler SRAM ownership in one field.
 *
 * @param name Object name to declare.
 * @param request_count Number of request blocks to allocate.
 * @param tx_buffer_bytes Total TX payload buffer bytes to allocate. Use 1 when
 *                        TX pre-allocation is disabled.
 * @param rx_buffer_bytes Total RX payload buffer bytes to allocate. Use 1 when
 *                        RX pre-allocation is disabled.
 * @param task_stack_bytes Background task stack size in bytes.
 */
#define ESY_SPI_SCHEDULER_STATIC_STORAGE(                                      \
    name, request_count, tx_buffer_bytes, rx_buffer_bytes, task_stack_bytes) \
    struct {                                                                 \
        esy_spi_scheduler_request_t request_storage[(request_count)];          \
        uint8_t tx_buffer_storage[(tx_buffer_bytes)];                        \
        uint8_t rx_buffer_storage[(rx_buffer_bytes)];                        \
        alignas(ESY_OS_TASK_STACK_ALIGN)                                     \
            uint8_t task_stack_buffer[(task_stack_bytes)];                   \
        esy_os_task_static_storage_t task_control_storage;                   \
    } name

/**
 * @brief SPI scheduler initialization configuration.
 */
typedef struct {
    /**
     * @brief Pre-allocated TX size per request block in bytes.
     *
     * If 0, no TX pre-allocation is performed.
     */
    uint32_t    tx_alloc_size;

    /**
     * @brief Pre-allocated RX size per request block in bytes.
     *
     * If 0, no RX pre-allocation is performed.
     */
    uint32_t    rx_alloc_size;

    /**
     * @brief Ring queue capacity (number of request blocks).
     *
     * Must be a power of two. The exact constraints are dictated by
     * the underlying ring implementation.
     */
    uint32_t    block_count;

    /**
     * @brief Caller-owned static resources for this scheduler.
     */
    esy_spi_scheduler_static_resources_t static_resources;

    /**
     * @brief Background task stack size in bytes.
     *
     * If 0, the default SPI scheduler stack size is used.
     */
    uint32_t    task_stack_size;

    /**
     * @brief Background task priority.
     */
    uint32_t    task_priority;

    /**
     * @brief Background task core affinity.
     *
     * May be ignored by single-core platforms.
     */
    uint8_t     task_core_affinity;

    /**
     * @brief Background task name (optional).
     *
     * May be NULL.
     */
    const char *task_name;
} esy_spi_scheduler_config_t;

/**
 * @brief Populate a configuration structure with recommended defaults.
 *
 * @param[out] cfg Output configuration pointer (must not be NULL).
 */
extern void esy_spi_scheduler_default_cfg(esy_spi_scheduler_config_t *cfg);

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create and initialize an SPI Scheduler instance.
 *
 * @param[in]  spi    SPI handle (must be initialized externally).
 * @param[in]  config Initialization configuration (must not be NULL).
 * @param[in]  storage Caller-owned scheduler storage.
 * @param[out] out_gw Output pointer receiving the scheduler instance.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
extern esy_err_t esy_spi_scheduler_create(
    esy_spi_t *spi,
    const esy_spi_scheduler_config_t *config,
    esy_spi_scheduler_storage_t *storage,
    esy_spi_scheduler_t **out_gw);

#ifdef __cplusplus
}
#endif

#endif /* ESY_SPI_SCHEDULER_H */
