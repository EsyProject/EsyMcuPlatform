/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_queue.h
 * @brief OS abstraction layer - message queue interface.
 *
 * This module provides an OS-agnostic message queue abstraction, similar in
 * semantics to FreeRTOS queues.
 *
 * Features:
 * - Supports static queue creation
 * - Fixed item size and maximum item count defined at creation time
 * - Blocking send/receive with timeout support in task context
 * - Non-blocking ISR variants with optional context-switch request
 *
 * Concurrency model:
 * - Safe for use by multiple producers and multiple consumers, subject to the
 *   guarantees of the underlying OS implementation.
 *
 * Context rules:
 * - Task-context APIs may block.
 * - ISR-context APIs must not block and use *_from_isr() variants only.
 *
 * Static allocation:
 * - Static queue creation uses caller-provided data buffer and opaque storage,
 *   allowing the implementation to remain fully opaque while avoiding heap
 *   allocation for the queue control block.
 */

#ifndef ESY_OS_QUEUE_H
#define ESY_OS_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/os/esy_os_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Compile-time constants                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Static queue storage size in bytes.
 *
 * This is an ABI contract for the OS port layer.
 * Increase this value if the internal implementation grows.
 *
 * The storage is used by @ref esy_os_queue_create_static and must persist for
 * the lifetime of the queue.
 */
#define ESY_OS_QUEUE_STATIC_STORAGE_BYTES 256u

/**
 * @brief Static queue storage alignment (bytes).
 *
 * Choose 8 for ESP32 / common 32-bit MCUs, generally safe.
 * If your platform needs stronger alignment, bump this value.
 */
#define ESY_OS_QUEUE_STATIC_STORAGE_ALIGN 8u

/* -------------------------------------------------------------------------- */
/* Opaque handle types                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Queue handle (opaque).
 *
 * The internal representation and OS-specific data structures are hidden from the caller.
 */
typedef struct esy_os_queue_s esy_os_queue_t;

/**
 * @brief Static queue storage object (opaque-bytes).
 *
 * The caller owns this object and must keep it alive for the lifetime of the queue.
 * The internal layout is implementation-defined.
 */
typedef struct esy_os_queue_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_QUEUE_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_QUEUE_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_QUEUE_STATIC_STORAGE_BYTES];
} esy_os_queue_static_storage_t;

/* -------------------------------------------------------------------------- */
/* Creation / destruction                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Create a statically allocated queue (no dynamic allocation for control block).
 *
 * Uses caller-provided memory for the underlying data buffer, and caller-provided
 * opaque storage for the internal static control block.
 *
 * @param[in]  item_size     Size of a single queue item in bytes.
 * @param[in]  max_items     Maximum number of items the queue can hold (must be > 0).
 * @param[in]  queue_buffer  Caller-provided data buffer.
 *                           Size must be at least (item_size * max_items) bytes.
 * @param[in]  storage       Caller-provided opaque static storage object.
 *                           Must persist for the lifetime of the queue.
 * @param[out] out_queue     Output: created queue handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid arguments, unsupported, OS error, etc.).
 */
extern esy_err_t esy_os_queue_create_static(size_t item_size,
                                           size_t max_items,
                                           void *queue_buffer,
                                           esy_os_queue_static_storage_t *storage,
                                           esy_os_queue_t **out_queue);

/**
 * @brief Delete / deinitialize a queue.
 *
 * Only internal state is deinitialized; caller-provided storage is not freed.
 *
 * @param[in] queue Queue handle.
 */
extern void esy_os_queue_delete(esy_os_queue_t *queue);

/* -------------------------------------------------------------------------- */
/* Task-context APIs                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send an item to the queue (task context).
 *
 * If the queue is full, the calling task will block until space becomes available
 * or the timeout expires.
 *
 * @param[in] queue       Queue handle.
 * @param[in] item        Pointer to the item to be copied into the queue.
 * @param[in] timeout_ms  Timeout in milliseconds:
 *                        - 0          : return immediately
 *                        - UINT32_MAX : wait indefinitely
 *
 * @retval ESY_OK          Success (item enqueued).
 * @retval ESY_ERR_TIMEOUT Timeout expired before space became available (if supported).
 * @retval <0              Other errors (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_queue_send(esy_os_queue_t *queue,
                                  const void *item,
                                  uint32_t timeout_ms);

/**
 * @brief Receive an item from the queue (task context).
 *
 * If the queue is empty, the calling task will block until an item becomes available
 * or the timeout expires.
 *
 * @param[in]  queue       Queue handle.
 * @param[out] item        Output buffer to receive the item.
 * @param[in]  timeout_ms  Timeout in milliseconds:
 *                        - 0          : return immediately
 *                        - UINT32_MAX : wait indefinitely
 *
 * @retval ESY_OK          Success (item received).
 * @retval ESY_ERR_TIMEOUT Timeout expired before an item became available (if supported).
 * @retval <0              Other errors (invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_queue_receive(esy_os_queue_t *queue,
                                     void *item,
                                     uint32_t timeout_ms);

/* -------------------------------------------------------------------------- */
/* ISR-context APIs                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Send an item to the queue from interrupt context.
 *
 * This function is non-blocking and must only be called from ISR context.
 *
 * @param[in]  queue      Queue handle.
 * @param[in]  item       Pointer to the item to be copied into the queue.
 * @param[out] need_yield Output: set to true if a context switch should be requested
 *                        when exiting the interrupt. May be NULL if not needed.
 *
 * @retval ESY_OK Success (item enqueued).
 * @retval <0     Failure (queue full, invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_queue_send_from_isr(esy_os_queue_t *queue,
                                           const void *item,
                                           bool *need_yield);

/**
 * @brief Receive an item from the queue from interrupt context.
 *
 * This function is non-blocking and must only be called from ISR context.
 *
 * @param[in]  queue      Queue handle.
 * @param[out] item       Output buffer to receive the item.
 * @param[out] need_yield Output: set to true if a context switch should be requested
 *                        when exiting the interrupt. May be NULL if not needed.
 *
 * @retval ESY_OK Success (item received).
 * @retval <0     Failure (queue empty, invalid arguments, OS error, etc.).
 */
extern esy_err_t esy_os_queue_receive_from_isr(esy_os_queue_t *queue,
                                              void *item,
                                              bool *need_yield);

#ifdef __cplusplus
}
#endif

#endif /* ESY_OS_QUEUE_H */
