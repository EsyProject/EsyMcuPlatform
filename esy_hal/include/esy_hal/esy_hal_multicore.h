/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_multicore.h
 * @brief HAL multicore communication interface.
 *
 * Provides:
 * - Secondary core boot/start.
 * - Lightweight 32-bit hardware FIFO communication (if supported by platform).
 * - A multicore-safe queue for multi-task bulk data exchange.
 *
 * Conventions:
 * - Core index @p core_id is platform-defined (e.g., RP2040: core0=0, core1=1).
 * - FIFO is suitable for small payloads / fast notifications.
 * - Queue is suitable for larger payloads and multi-task safe exchange.
 *
 * Blocking rules:
 * - Blocking APIs are explicitly marked. Do not call blocking APIs from ISR context.
 */

#ifndef ESY_HAL_MULTICORE_H
#define ESY_HAL_MULTICORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/system/esy_multicore_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start/boot a secondary core.
 *
 * Boots the specified core and sets its entry function.
 * Pico permits one launch per boot from an externally serialized core-0
 * startup/task owner. Core 1 must remain in its SDK reset state until this call;
 * do not independently launch or reset it. Entry must not return. Launch
 * ownership is never released during this boot, and this API never resets a
 * running core.
 *
 * @param[in] core_id  Target core id (platform-defined; typically 1 for secondary core).
 * @param[in] entry    Entry function executed on the started core.
 *
 * @retval ESY_OK               Started successfully.
 * @retval ESY_ERR_INVALID_ARG  Invalid core id or NULL entry.
 * @retval ESY_ERR_STATE        Invalid caller core or execution context.
 * @retval ESY_ERR_BUSY         Core already running / resource busy.
 * @retval ESY_ERR_NOT_SUPPORTED Platform does not support multicore start.
 * @retval ESY_ERR_FAIL         Other failures.
 *
 * @note Some platforms require @p entry to never return.
 * @warning This function may have platform-specific constraints (alignment, stack setup, etc.).
 */
extern esy_err_t esy_hal_multicore_start_core(uint8_t core_id, void (*entry)(void));

/**
 * @brief Initialize a multicore queue instance.
 *
 * Initializes a queue object using caller-provided backing buffer.
 *
 * @param[out] queue      Queue instance to initialize (must not be NULL).
 * @param[in]  item_size  Size of one item in bytes, in 1..UINT16_MAX.
 * @param[in]  depth      Usable queue depth, in 1..UINT16_MAX.
 * @param[in]  buf        Caller-owned backing storage. The required capacity is
 *                       item_size * (depth + 1) bytes, including one reserved slot.
 * @param[in]  buf_size   Actual capacity of buf in bytes.
 *
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  Invalid parameters (NULL pointers / zero sizes).
 * @retval ESY_ERR_NO_MEM       Backing capacity is smaller than required.
 * @retval ESY_ERR_NOT_SUPPORTED Multicore queue not supported on this platform.
 * @retval ESY_ERR_FAIL         Other failures.
 *
 * @note This interface does not perform dynamic allocation; buffer ownership remains with caller.
 * The first queue object may contain uninitialized storage. All producers and
 * consumers must be stopped before initialization, reinitialization or deinit;
 * data operations are internally safe across cores after successful init.
 * @warning The queue object must remain valid while any core uses it.
 */
extern esy_err_t esy_hal_multicore_queue_init(esy_multicore_queue_t* queue,
                                                  uint32_t item_size,
                                                  uint32_t depth,
                                                  uint8_t* buf,
                                                  size_t buf_size);

/**
 * @brief Deinitialize a multicore queue instance.
 *
 * Releases any platform resources associated with the queue (if any).
 *
 * @param[in,out] queue  Queue instance to deinitialize (may be NULL; behavior implementation-defined).
 *
 * @note Caller remains responsible for the backing buffer memory.
 * @note This function should be idempotent if possible.
 */
extern void esy_hal_multicore_queue_deinit(esy_multicore_queue_t* queue);

/**
 * @brief Send one item to the queue (blocking).
 *
 * Blocks until there is room to enqueue the item.
 *
 * @param[in,out] queue  Queue instance (must not be NULL).
 * @param[in]     data   Pointer to the item to send; length must be exactly item_size bytes.
 *
 * @note Do not call from ISR context.
 * @warning @p data must point to a valid buffer of at least item_size bytes.
 */
extern void esy_hal_multicore_queue_send_blocking(esy_multicore_queue_t* queue,
                                                      const void* data);

/**
 * @brief Try to send one item to the queue (non-blocking).
 *
 * @param[in,out] queue  Queue instance (must not be NULL).
 * @param[in]     data   Pointer to the item to send; length must be exactly item_size bytes.
 *
 * @retval true   Item enqueued successfully.
 * @retval false  Queue is full (or parameters invalid; implementation-defined).
 *
 * @warning @p data must point to a valid buffer of at least item_size bytes.
 */
extern bool esy_hal_multicore_queue_try_send(esy_multicore_queue_t* queue,
                                                 const void* data);

/**
 * @brief Receive one item from the queue (blocking).
 *
 * Blocks until an item becomes available.
 *
 * @param[in,out] queue  Queue instance (must not be NULL).
 * @param[out]    data   Output buffer to store the received item; must be at least item_size bytes.
 *
 * @note Do not call from ISR context.
 * @warning @p data must point to a valid writable buffer of at least item_size bytes.
 */
extern void esy_hal_multicore_queue_receive_blocking(esy_multicore_queue_t* queue,
                                                         void* data);

/**
 * @brief Try to receive one item from the queue (non-blocking).
 *
 * @param[in,out] queue  Queue instance (must not be NULL).
 * @param[out]    data   Output buffer; must be at least item_size bytes.
 *
 * @retval true   One item received and copied to @p data.
 * @retval false  Queue is empty (or parameters invalid; implementation-defined).
 *
 * @warning @p data must point to a valid writable buffer of at least item_size bytes.
 */
extern bool esy_hal_multicore_queue_try_receive(esy_multicore_queue_t* queue,
                                                    void* data);

#ifdef __cplusplus
}
#endif

#endif  /* ESY_HAL_MULTICORE_H */
