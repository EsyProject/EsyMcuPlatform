/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_multicore.h
 * @brief Multicore communication and queue HAL backend interface.
 *
 * This interface provides basic multicore support, including:
 * - Secondary core startup via core ID and entry function.
 * - FIFO / queue-based inter-core communication.
 *
 * Design notes:
 * - FIFO mechanisms are intended for small, fast synchronization messages.
 * - Queues are intended for larger payloads or task-style communication.
 *
 * Concurrency and safety:
 * - Queue capacity is limited; non-blocking APIs return false on failure.
 * - FIFO/queue operations are internally thread-safe.
 * - Callers should avoid long blocking operations in high-priority or
 *   interrupt contexts.
 * - Communication primitives should be initialized before starting
 *   the secondary core.
 */

#ifndef ESY_HB_MULTICORE_H
#define ESY_HB_MULTICORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_multicore_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start a secondary CPU core.
 *
 * Pico permits one launch per boot, from an externally serialized core-0
 * startup/task owner. Core 1 must still be in its SDK reset state; do not launch
 * or reset it through another provider. A successful launch remains owned for
 * the boot, even if its entry returns or external code later resets the core.
 * Entry must not return. This API never resets a running core.
 *
 * @param[in] core_id Secondary core identifier (platform-defined;
 *                    e.g. core 1 on RP2040/Pico SDK)
 * @param[in] entry   Entry function executed on the secondary core
 *
 * @retval ESY_OK Success
 * @retval ESY_ERR_INVALID_ARG Invalid core identifier or NULL entry.
 * @retval ESY_ERR_STATE The caller is not a core-0 startup/task owner.
 * @retval ESY_ERR_BUSY Core 1 has already been launched during this boot.
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_multicore_start_core(uint8_t core_id,
                                                 void (*entry)(void));

/**
 * @brief Initialize a multicore communication queue.
 *
 * @param[out] queue     Queue control block (storage provided by caller)
 * @param[in]  item_size Size of one item in bytes, in 1..UINT16_MAX.
 * @param[in]  depth     Usable queue depth, in 1..UINT16_MAX.
 * @param[in]  buf       Caller-owned backing storage of item_size * (depth + 1)
 *                      bytes, including one reserved slot.
 * @param[in]  buf_size  Actual backing capacity in bytes.
 *
 * The first queue object may contain uninitialized storage. Stop both endpoints
 * before init/reinit/deinit. Successful init permits internally synchronized
 * data operations from multiple tasks/cores; blocking APIs are not ISR-safe.
 *
 * @retval ESY_OK Success
 * @retval ESY_ERR_NO_MEM Insufficient backing capacity; no state is changed.
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_multicore_queue_init(esy_multicore_queue_t *queue,
                                                 uint32_t item_size,
                                                 uint32_t depth,
                                                 uint8_t *buf,
                                                 size_t buf_size);

/**
 * @brief Deinitialize a multicore communication queue.
 *
 * Releases internal resources associated with the queue. The queue storage
 * itself is owned by the caller and is not freed.
 *
 * @param[in] queue Queue control block
 */
extern void esy_hb_multicore_queue_deinit(esy_multicore_queue_t *queue);

/**
 * @brief Send data to a queue (blocking).
 *
 * Blocks until space is available in the queue.
 *
 * @param[in] queue Queue control block
 * @param[in] data  Pointer to data to be enqueued
 */
extern void esy_hb_multicore_queue_send_blocking(
    esy_multicore_queue_t *queue,
    const void *data);

/**
 * @brief Attempt to send data to a queue (non-blocking).
 *
 * @param[in] queue Queue control block
 * @param[in] data  Pointer to data to be enqueued
 *
 * @retval true  Data successfully enqueued
 * @retval false Queue is full
 */
extern bool esy_hb_multicore_queue_try_send(
    esy_multicore_queue_t *queue,
    const void *data);

/**
 * @brief Receive data from a queue (blocking).
 *
 * Blocks until data becomes available.
 *
 * @param[in]  queue Queue control block
 * @param[out] data  Pointer to output buffer
 */
extern void esy_hb_multicore_queue_receive_blocking(
    esy_multicore_queue_t *queue,
    void *data);

/**
 * @brief Attempt to receive data from a queue (non-blocking).
 *
 * @param[in]  queue Queue control block
 * @param[out] data  Pointer to output buffer
 *
 * @retval true  Data successfully received
 * @retval false Queue is empty
 */
extern bool esy_hb_multicore_queue_try_receive(
    esy_multicore_queue_t *queue,
    void *data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_MULTICORE_H */
