/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_BACKEND_PRINTF_H
#define ESY_LOGGER_BACKEND_PRINTF_H

/**
 * @file esy_logger_backend_printf.h
 * @brief Logger backend: standard printf-based implementation.
 *
 * Logs are printed using `printf()` or an equivalent standard output interface.
 * This backend is primarily intended for debugging, simulation, or host-based
 * environments.
 *
 * Characteristics:
 * - Conforms to @ref esy_logger_backend_t via acquire_buffer()/submit_buffer().
 * - Does not use DMA; log aggregation and printing are handled in a background task.
 * - Log buffers are managed via a fixed-size block pool
 *   (block_size * pool_block_count).
 * - The print task uses caller-owned static task control storage and stack
 *   buffer storage.
 */

#include "esy_logger_backend.h"
#include "esy_logger_backend_printf_core.h"

#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/os/esy_os_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned static resources for the printf logger backend.
 *
 * The resources must remain valid from successful init until deinit returns ESY_OK. Pool and
 * ready-ring storage is split by runtime core count, so each buffer must have
 * capacity for `runtime_core_count * pool_block_count` entries.
 */
typedef struct {
  uint8_t *pool_buffers; /**< Backing bytes for per-core log block pools. */
  size_t pool_buffer_size; /**< Total bytes in @ref pool_buffers. */
  uint32_t *pool_index_stacks; /**< Free-index stack entries for all pools. */
  size_t pool_index_stack_count; /**< Number of entries in @ref pool_index_stacks. */
  esy_logger_backend_printf_ready_slot_t
      *ready_ring_slots; /**< Backing slots for per-core ready rings. */
  size_t ready_ring_slot_count; /**< Number of entries in @ref ready_ring_slots. */
  void *task_stack_buffer; /**< Print task stack buffer. */
  uint32_t task_stack_buffer_size; /**< Print task stack buffer size in bytes. */
  esy_os_task_static_storage_t
      *task_control_storage; /**< Print task control storage. */
} esy_logger_backend_printf_static_resources_t;

/**
 * @brief Declare caller-owned printf backend storage as one app-owned object.
 *
 * The object declared by this macro must remain valid until the printf backend
 * deinit returns ESY_OK. Use
 * @ref ESY_LOGGER_BACKEND_PRINTF_BIND_STATIC_RESOURCES to bind the object to an
 * @ref esy_logger_backend_printf_static_resources_t instance.
 *
 * @param name             Object name to declare.
 * @param core_count       Maximum core count budget.
 * @param pool_block_count Pool blocks per core (power of two, 2..8).
 * @param block_size       Bytes per log block.
 * @param task_stack_size  Print task stack bytes.
 */
#define ESY_LOGGER_BACKEND_PRINTF_STATIC_STORAGE(                      \
    name, core_count, pool_block_count, block_size, task_stack_size)   \
  struct {                                                            \
    uint8_t pool_buffers[(core_count) * (pool_block_count) *          \
                         (block_size)];                               \
    uint32_t pool_index_stacks[(core_count) * (pool_block_count)];     \
    esy_logger_backend_printf_ready_slot_t                             \
        ready_ring_slots[(core_count) * (pool_block_count)];          \
    ESY_ALIGNED_STORAGE(task_stack_buffer, (task_stack_size));         \
    esy_os_task_static_storage_t task_control_storage;                 \
  } name

/**
 * @brief Bind app-owned printf backend storage to a resource descriptor.
 *
 * @param resources Pointer to
 *                  @ref esy_logger_backend_printf_static_resources_t.
 * @param storage   Object declared by
 *                  @ref ESY_LOGGER_BACKEND_PRINTF_STATIC_STORAGE.
 */
#define ESY_LOGGER_BACKEND_PRINTF_BIND_STATIC_RESOURCES(resources, storage) \
  do {                                                                      \
    (resources)->pool_buffers = (storage).pool_buffers;                     \
    (resources)->pool_buffer_size = sizeof((storage).pool_buffers);         \
    (resources)->pool_index_stacks = (storage).pool_index_stacks;           \
    (resources)->pool_index_stack_count =                                   \
        sizeof((storage).pool_index_stacks) /                               \
        sizeof((storage).pool_index_stacks[0]);                             \
    (resources)->ready_ring_slots = (storage).ready_ring_slots;             \
    (resources)->ready_ring_slot_count =                                    \
        sizeof((storage).ready_ring_slots) /                                \
        sizeof((storage).ready_ring_slots[0]);                              \
    (resources)->task_stack_buffer = (storage).task_stack_buffer.bytes;     \
    (resources)->task_stack_buffer_size =                                   \
        sizeof((storage).task_stack_buffer.bytes);                          \
    (resources)->task_control_storage = &(storage).task_control_storage;    \
  } while (0)

/**
 * @brief printf logger backend configuration.
 */
typedef struct {
  uint32_t task_priority;      /**< Print task priority. */
  uint8_t  task_core_affinity; /**< CPU affinity mask for the print task
                                    (0xFF means no affinity restriction). */
  size_t   block_size;         /**< Maximum bytes per log entry (block size). */
  uint32_t pool_block_count;   /**< Number of blocks per core
                                    (power of two, 2..8). */
  esy_logger_backend_printf_static_resources_t
      *static_resources; /**< Caller-owned static storage resources. */
} esy_logger_backend_printf_config_t;

/**
 * @brief Populate printf logger backend configuration with recommended defaults.
 *
 * Current implementation defaults:
 * - task_priority = 6
 * - task_core_affinity = ESY_CORE_ALL
 * - block_size = 128
 * - pool_block_count = 8
 * - static_resources = NULL
 *
 * @param[out] cfg Configuration pointer (must not be NULL).
 */
extern void esy_logger_backend_printf_default_cfg(
    esy_logger_backend_printf_config_t *cfg);

/**
 * @brief Initialize the printf logger backend and bind it to a generic backend object.
 *
 * @param[in,out] logger_backend Output: backend object to be initialized
 *                               (its function pointers will be populated).
 * @param[in]     config         Input: initialization configuration.
 *
 * @note Task/boot context only, externally serialized lifecycle owner. The
 * handle must be `{0}` before first use or successfully deinitialized. Invalid
 * configuration/creation failure leaves it unchanged and safe to deinit/retry.
 * Repeated init returns ESY_ERR_STATE and preserves the active instance.
 * Storage and the backend object must survive until deinit returns ESY_OK.
 * The OS port must support cooperative task parking; otherwise init returns
 * ESY_ERR_NOT_SUPPORTED before creating a worker.
 * The returned raw acquire/submit callbacks support task/ISR producers, but
 * formatting/logging is task-only. Pass the backend object's address as self.
 * Output preserves global submission order across per-core queues. Stdio I/O
 * is best-effort; asynchronous output failures cannot be returned to submitters.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 *
 * @note This backend is typically used in PC or simulation environments.
 *       Performance may be limited in bare-metal or RTOS-based systems.
 */
extern esy_err_t esy_logger_backend_printf_init(
    esy_logger_backend_t *logger_backend,
    const esy_logger_backend_printf_config_t *config);

/**
 * @brief Stop the printf backend and release its resources when idle (task only).
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
 *
 * @param[in,out] logger_backend Zero-initialized or live backend handle.
 * @retval ESY_OK Fully released; the zero handle may be reused immediately.
 * @retval ESY_ERR_BUSY A frontend transaction, borrowed buffer, I/O or callback
 * is still active, or the worker has not parked. Retain resources and retry.
 * @retval ESY_ERR_STATE Attempt to release from the backend worker itself.
 * @retval ESY_ERR_INVALID_ARG NULL/foreign handle or ISR call.
 */
extern esy_err_t esy_logger_backend_printf_deinit(
    esy_logger_backend_t *logger_backend);

#ifdef __cplusplus
}
#endif

#endif /* ESY_LOGGER_BACKEND_PRINTF_H */
