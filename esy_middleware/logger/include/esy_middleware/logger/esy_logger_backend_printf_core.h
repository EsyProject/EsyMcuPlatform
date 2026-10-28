/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file esy_logger_backend_printf_core.h
 * @brief Scheduling-independent buffered printf logger engine. */
#ifndef ESY_LOGGER_BACKEND_PRINTF_CORE_H
#define ESY_LOGGER_BACKEND_PRINTF_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "esy_logger_backend.h"
#include "esy_common/defs/base/esy_common_defs.h"
#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Opaque ready-ring slot for the printf logger backend.
 */
typedef struct {
#if defined(__cplusplus)
  alignas(4)
#else
  _Alignas(4)
#endif
      uint8_t bytes[8u]; /**< Opaque ready-ring slot bytes. */
} esy_logger_backend_printf_ready_slot_t;

/** @brief Borrowed pool and queue storage; no task resources. */
typedef struct {
  uint8_t *pool_buffers; /**< Backing bytes for per-core log block pools. */
  size_t pool_buffer_size; /**< Total bytes in @ref pool_buffers. */
  uint32_t *pool_index_stacks; /**< Free-index stack entries for all pools. */
  size_t pool_index_stack_count; /**< Number of entries in @ref pool_index_stacks. */
  esy_logger_backend_printf_ready_slot_t
      *ready_ring_slots; /**< Backing slots for per-core ready rings. */
  size_t ready_ring_slot_count; /**< Number of entries in @ref ready_ring_slots. */
} esy_logger_backend_printf_core_resources_t;
/** @brief Buffer and transport configuration. */
typedef struct {
  size_t   block_size;         /**< Maximum bytes per log entry (block size). */
  uint32_t pool_block_count;   /**< Number of blocks per core
                                    (power of two, 2..8). */
  esy_logger_backend_printf_core_resources_t
      *static_resources; /**< Caller-owned static storage resources. */
} esy_logger_backend_printf_core_config_t;
/** @brief Fill buffered backend defaults; static_resources remains NULL. */
void esy_logger_backend_printf_core_default_cfg(esy_logger_backend_printf_core_config_t *cfg);
/**
 * @brief Initialize the caller-driven backend without an OS worker.
 * @note Zero-initialize the backend object before first use. One owner core
 * serializes frontend logging, raw producers, process and lifecycle; direct ISR
 * acquire/submit is allowed only on that core. Other cores must not use this
 * instance. Local IRQ masking protects pool and completion state. Borrowed
 * buffers, HAL handles and resource storage survive until deinit succeeds.
 * Existing OS init retains its multi-producer synchronization contract.
 */
esy_err_t esy_logger_backend_printf_core_init(esy_logger_backend_t *backend,
    const esy_logger_backend_printf_core_config_t *config);
/**
 * @brief Attempt at most block_budget queued blocks from the owner context.
 * @note Positive budget; no task waits. Provider operations determine duration,
 * including blocking stdio write/flush.
 * Call often enough to avoid queue exhaustion. Output remains globally ordered across core pools. Stdio failures are best-effort, as in the OS adapter.
 * Do not call process or lifecycle recursively from providers/callbacks.
 * STATE means wrong mode, stopped or uninitialized; budget limits work count,
 * not wall-clock time. It is safe to retry deinit after BUSY without clearing storage.
 */
esy_err_t esy_logger_backend_printf_core_process(esy_logger_backend_t *backend, size_t block_budget);
/** @brief Detach and stop; BUSY retains outstanding leases/I/O for owner retry.
 * Queued output is discarded on successful cleanup. Non-ISR owner calls only. */
esy_err_t esy_logger_backend_printf_deinit(esy_logger_backend_t *backend);
#ifdef __cplusplus
}
#endif
#endif /* ESY_LOGGER_BACKEND_PRINTF_CORE_H */
