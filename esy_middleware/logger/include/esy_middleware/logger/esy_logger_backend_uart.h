/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_BACKEND_UART_H
#define ESY_LOGGER_BACKEND_UART_H

/**
 * @file esy_logger_backend_uart.h
 * @brief Logger backend: UART transport implementation
 *        (zero dynamic memory guaranteed by the implementation).
 *
 * Design highlights:
 * - Output formatted log blocks via UART, optionally using DMA.
 * - Adapts to @ref esy_logger_backend_t through acquire_buffer()/submit_buffer().
 * - Uses a background task for asynchronous transmission
 *   and buffer recycling, with configurable block pool parameters.
 */

#include "esy_logger_backend.h"
#include "esy_logger_backend_uart_core.h"

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#include "esy_middleware/os/esy_os_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned static resources for the UART logger backend.
 *
 * The resources must remain valid until deinit returns ESY_OK. Pool and ring
 * storage is shared by this instance's producers and requires capacity for
 * `pool_block_count` entries. It must not overlap another live instance's storage.
 */
typedef struct {
  esy_logger_backend_uart_context_storage_t
      *context_storage; /**< Zero-initialized state reserved for this instance. */
  uint8_t *pool_buffers; /**< Backing bytes for the shared log block pool. */
  size_t pool_buffer_size; /**< Total bytes in @ref pool_buffers. */
  uint32_t *pool_index_stacks; /**< Free-index stack entries for the pool. */
  size_t pool_index_stack_count; /**< Number of entries in @ref pool_index_stacks. */
  esy_logger_backend_uart_ready_slot_t
      *ready_ring_slots; /**< Backing slots for the shared ready ring. */
  size_t ready_ring_slot_count; /**< Number of entries in @ref ready_ring_slots. */
  void *task_stack_buffer; /**< Sender task stack buffer. */
  uint32_t task_stack_buffer_size; /**< Sender task stack buffer size in bytes. */
  esy_os_task_static_storage_t
      *task_control_storage; /**< Sender task control storage. */
} esy_logger_backend_uart_static_resources_t;

/**
 * @brief Declare caller-owned UART backend storage as one app-owned object.
 *
 * The object declared by this macro must remain valid until the UART backend is
 * deinitialized. Initialize the object with `{0}` before first use.
 * Each live instance needs a distinct object and UART TX resource. Use
 * @ref ESY_LOGGER_BACKEND_UART_BIND_STATIC_RESOURCES to bind
 * the object to an @ref esy_logger_backend_uart_static_resources_t instance.
 *
 * @param name             Object name to declare.
 * @param pool_block_count Pool blocks shared by all producers (power of two, 2..32).
 * @param block_size       Bytes per log block.
 * @param task_stack_size  Sender task stack bytes.
 */
#define ESY_LOGGER_BACKEND_UART_STATIC_STORAGE(                       \
    name, pool_block_count, block_size, task_stack_size)              \
  struct {                                                            \
    esy_logger_backend_uart_context_storage_t context_storage;        \
    uint8_t pool_buffers[(pool_block_count) * (block_size)];           \
    uint32_t pool_index_stacks[(pool_block_count)];                    \
    esy_logger_backend_uart_ready_slot_t                              \
        ready_ring_slots[(pool_block_count)];                         \
    ESY_ALIGNED_STORAGE(task_stack_buffer, (task_stack_size));         \
    esy_os_task_static_storage_t task_control_storage;                 \
  } name

/**
 * @brief Bind app-owned UART backend storage to a resource descriptor.
 *
 * @param resources Pointer to @ref esy_logger_backend_uart_static_resources_t.
 * @param storage   Object declared by
 *                  @ref ESY_LOGGER_BACKEND_UART_STATIC_STORAGE.
 */
#define ESY_LOGGER_BACKEND_UART_BIND_STATIC_RESOURCES(resources, storage) \
  do {                                                                    \
    (resources)->context_storage = &(storage).context_storage;           \
    (resources)->pool_buffers = (storage).pool_buffers;                   \
    (resources)->pool_buffer_size = sizeof((storage).pool_buffers);       \
    (resources)->pool_index_stacks = (storage).pool_index_stacks;         \
    (resources)->pool_index_stack_count =                                 \
        sizeof((storage).pool_index_stacks) /                             \
        sizeof((storage).pool_index_stacks[0]);                           \
    (resources)->ready_ring_slots = (storage).ready_ring_slots;           \
    (resources)->ready_ring_slot_count =                                  \
        sizeof((storage).ready_ring_slots) /                              \
        sizeof((storage).ready_ring_slots[0]);                            \
    (resources)->task_stack_buffer = (storage).task_stack_buffer.bytes;   \
    (resources)->task_stack_buffer_size =                                 \
        sizeof((storage).task_stack_buffer.bytes);                        \
    (resources)->task_control_storage = &(storage).task_control_storage;  \
  } while (0)

/**
 * @brief UART logger backend configuration.
 *
 * Notes:
 * - `block_size` / `pool_block_count` describe the output buffer pool
 *   (fixed-size blocks; LIFO/FIFO policy is implementation-defined).
 * - `pool_block_count` must be at least two, a power of two, and must not exceed the
 *   implementation compiled maximum.
 * - `dma_short_threshold`: when the submitted length is less than or equal to
 *   this threshold, a short-path send (polling/interrupt) is used; otherwise
 *   DMA is preferred.
 * - The background task is used for asynchronous transmission and resource
 *   reclamation; implementations that rely purely on interrupt-driven sending
 *   may ignore task-related fields.
 */
typedef struct {
  /**
   * @brief Initialized, caller-owned UART handle; retain until successful deinit.
   * Each live instance requires a distinct UART TX resource. Binding the same
   * handle or physical UART to two instances violates the caller contract and
   * is not detected. Other TX clients require exclusive use or external arbitration.
   */
  esy_uart_t *uart;

  /* Background task */
  uint32_t task_priority;     /**< Task priority. */
  uint8_t  task_core_affinity; /**< Core affinity on multi-core platforms (ignored on single-core). */

  /* Transmit buffer pool */
  size_t   block_size;         /**< Size of each buffer block in bytes. */
  uint32_t pool_block_count;   /**< Pool blocks (power of two, 2..32). */

  /* DMA / short-packet strategy */
  uint32_t dma_short_threshold; /**< Length at or below which blocking TX is used. */

  esy_logger_backend_uart_static_resources_t
      *static_resources; /**< Caller-owned static storage resources. */
} esy_logger_backend_uart_config_t;

/**
 * @brief Populate UART logger backend configuration with recommended defaults.
 *
 * @param[out] cfg Configuration pointer (must not be NULL).
 *
 * Current implementation defaults:
 * - task_priority = 6
 * - task_core_affinity = ESY_CORE_ALL
 * - block_size = 128
 * - pool_block_count = 32
 * - dma_short_threshold = 6
 * - static_resources = NULL
 */
extern void esy_logger_backend_uart_default_cfg(
    esy_logger_backend_uart_config_t* cfg);

/**
 * @brief Initialize the UART logger backend and bind it to a generic backend object.
 *
 * @param[in,out] logger_backend Output: backend object to be initialized
 *                               (its function pointers will be populated).
 * @param[in]     config         Input: initialization configuration.
 *
 * @note Task/boot context only, externally serialized lifecycle owner. The
 * handle and context storage must be `{0}` before first use or successfully
 * deinitialized. Every instance owns a separate worker and borrows distinct
 * context/pool/ring/task resources; the implementation has no instance limit.
 * Live context storage cannot be reused by another handle; init returns STATE.
 * Lifecycle calls sharing a storage object must be externally serialized. Invalid
 * configuration/creation failure leaves it unchanged and safe to deinit/retry.
 * Repeated init returns ESY_ERR_STATE and preserves the active instance.
 * Storage and the backend object must survive until deinit returns ESY_OK.
 * The OS port must support cooperative task parking; otherwise init returns
 * ESY_ERR_NOT_SUPPORTED before creating a worker.
 * The returned raw acquire/submit callbacks support task/ISR producers, but
 * formatting/logging is task-only. Pass the backend object's address as self.
 * BUSY/STATE TX errors are retried by the sender after a 10 ms delay; other
 * TX errors discard the block. DMA completion/error may run in task or ISR context.
 *
 * @retval ESY_OK                 Success.
 * @retval <0                     Failure (e.g. ESY_ERR_INVALID_ARG,
 *                               ESY_ERR_BUSY, ESY_ERR_FAIL,
 *                               ESY_ERR_NOT_SUPPORTED).
 *
 * @note This function performs no heap allocation.
 *       All runtime resources must be placed in static storage or
 *       externally provided memory managed by the implementation.
 */
extern esy_err_t esy_logger_backend_uart_init(
    esy_logger_backend_t* logger_backend,
    const esy_logger_backend_uart_config_t* config);

/**
 * @brief Stop the uart backend and release its resources when idle (task only).
 *
 * The lifecycle owner must serialize init/bind/deinit and stop new direct
 * callback calls. This first detaches the frontend; if a frontend transaction
 * is active it returns BUSY without stopping that transaction. Once stopping,
 * new acquire calls fail; submit returns any outstanding buffer without output.
 * Pending queued output is discarded on success. A BUSY result retains the
 * handle and every resource; retry from the owner task with an application
 * deadline. Timeout does not authorize clearing or releasing storage.
 * With the scheduler running, the worker must acknowledge stop by parking
 * before deletion. Before scheduler startup there is no worker to acknowledge.
 * Physical UART TX must also be idle: source completion can release its log
 * block before the FIFO/shift register drains. No forced abort or drain wait
 * occurs; retain the UART and retry BUSY until physical output has stopped.
 *
 * @param[in,out] logger_backend Zero-initialized or live backend handle.
 * @retval ESY_OK Fully released; the zero handle may be reused immediately.
 * @retval ESY_ERR_BUSY A frontend transaction, borrowed buffer, I/O or callback
 * is still active, or the worker has not parked. Retain resources and retry.
 * @retval ESY_ERR_STATE Attempt to release from the backend worker itself.
 * @retval ESY_ERR_INVALID_ARG NULL/foreign handle or ISR call.
 */
extern esy_err_t esy_logger_backend_uart_deinit(
    esy_logger_backend_t* logger_backend);

/**
 * @brief Prevent new transfers while preserving queued logs (owner task only).
 * @param[in,out] logger_backend Live UART backend; serialize lifecycle/control calls.
 * @retval ESY_OK Quiesced, with no I/O/callback or physical TX active; producers may enqueue.
 * @retval ESY_ERR_BUSY I/O/callback or physical TX active; backend remains running.
 * Retry later; DMA source completion alone does not establish wire idle. The
 * physical-busy query runs outside critical sections under a sender admission
 * gate and does not wait for drain. Exclusive TX use or external arbitration
 * must remain in effect throughout quiescence.
 * @retval ESY_ERR_STATE Already quiesced or stopping.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or ISR call.
 */
extern esy_err_t esy_logger_backend_uart_quiesce(
    esy_logger_backend_t* logger_backend);

/**
 * @brief Resume queued delivery after wake/rollback (owner task only).
 * @param[in,out] logger_backend Quiesced UART backend; serialize control calls.
 * @retval ESY_OK Resumed and sender notified.
 * @retval ESY_ERR_STATE Not quiesced or stopping.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or ISR call.
 * @retval <0 Notification failure; the backend is already resumed.
 */
extern esy_err_t esy_logger_backend_uart_resume(
    esy_logger_backend_t* logger_backend);

#ifdef __cplusplus
}
#endif

#endif /* ESY_LOGGER_BACKEND_UART_H */
