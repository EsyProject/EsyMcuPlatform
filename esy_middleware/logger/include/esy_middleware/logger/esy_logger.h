/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_H
#define ESY_LOGGER_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "esy_common/defs/logging/esy_logger_defs.h"
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_logger_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- Convenience macros: log at different levels ---------------- */

#define ESY_LOG_E(tag, fmt, ...) \
  esy_log((tag), ESY_LOG_LEVEL_ERROR, (fmt), ##__VA_ARGS__)

#define ESY_LOG_W(tag, fmt, ...) \
  esy_log((tag), ESY_LOG_LEVEL_WARN, (fmt), ##__VA_ARGS__)

#define ESY_LOG_I(tag, fmt, ...) \
  esy_log((tag), ESY_LOG_LEVEL_INFO, (fmt), ##__VA_ARGS__)

#define ESY_LOG_D(tag, fmt, ...) \
  esy_log((tag), ESY_LOG_LEVEL_DEBUG, (fmt), ##__VA_ARGS__)

#define ESY_LOG_V(tag, fmt, ...) \
  esy_log((tag), ESY_LOG_LEVEL_VERBOSE, (fmt), ##__VA_ARGS__)

/**
 * @brief Log module index type.
 *
 * This type may be used when implementing module-based
 * log filtering or routing.
 */
typedef uint8_t esy_log_module_t;

/** @brief One caller-owned backend binding and its output threshold. */
typedef struct {
  /** Initialized backend object; NULL marks an inactive entry. */
  const esy_logger_backend_t *backend;
  /** Most verbose permitted level, from ERROR through VERBOSE. */
  esy_log_level_t filter_level;
} esy_logger_backend_binding_t;

/**
 * @brief Caller-owned storage borrowed by the multi-backend frontend.
 *
 * Fully initialize every entry before binding; at least one entry must be
 * active. Entries may initially be inactive. The logger exclusively owns
 * access to the array after a successful bind and clears backend pointers on
 * detach. Do not inspect, edit, move or free it until the final backend is
 * detached, or a successful init replaces the complete binding.
 * The resource descriptor itself is not retained.
 */
typedef struct {
  /** Mutable binding array with a lifetime covering the complete binding. */
  esy_logger_backend_binding_t *bindings;
  /** Number of entries in bindings, including inactive entries; must be nonzero. */
  size_t binding_count;
} esy_logger_static_resources_t;

/** @brief Initial configuration for a set of already initialized backends. */
typedef struct {
  /** Required caller-owned binding storage; the descriptor is not retained. */
  const esy_logger_static_resources_t *static_resources;
  /** Initial global threshold, from ERROR through VERBOSE. */
  esy_log_level_t filter_level;
} esy_logger_config_t;

/**
 * @brief Bind one or more initialized backends atomically (task context only).
 *
 * Each active backend must provide acquire_buffer and submit_buffer callbacks;
 * the same backend object may appear only once. Backend objects, contexts and
 * their resources must outlive their individual bindings. Callback fields and
 * context pointers must remain unchanged while bound. Different instances
 * of the same backend type are permitted if that backend supports them.
 *
 * All entries, including inactive entries, must have valid filter levels.
 * Log messages must pass both the global and the selected backend threshold.
 * There is no fixed frontend backend-count limit or allocation. The array size
 * and its address range must be representable. The new array must not overlap
 * the currently borrowed array: detach every backend or replace the binding
 * through another array before reusing its storage. Single-backend logging uses
 * the same configuration with one active entry.
 *
 * Validation failure leaves existing bindings and the global filter unchanged.
 * Success replaces every previous binding and releases its array; backends are
 * never initialized, stopped, drained or destroyed by this call. Only bindings
 * and binding_count are retained, not cfg or its resource descriptor. The caller
 * must not access the borrowed array until it is released as described by
 * esy_logger_static_resources_t.
 *
 * One externally serialized lifecycle owner performs bind/detach/backend
 * init/deinit; logging and global filter updates may run concurrently. The
 * operation never waits for an active frontend transaction.
 *
 * @param[in] cfg Complete configuration, non-NULL.
 * @retval ESY_OK All backends were bound and the global filter was set.
 * @retval ESY_ERR_INVALID_ARG Missing/empty storage, invalid level or callbacks,
 * duplicate backend, no active backend, unrepresentable array range, or storage
 * overlapping the currently borrowed array. Existing bindings are unchanged.
 * @retval ESY_ERR_BUSY A log or binding transaction is active; no state changed.
 */
extern esy_err_t esy_logger_init(const esy_logger_config_t *cfg);

/**
 * @brief Detach a backend before destroying its object or context (task only).
 *
 * The lifecycle owner must serialize this call with bind/init/deinit. Logging
 * may run concurrently. Only the matching backend is removed; all other
 * bindings and their filter levels remain unchanged. Success guarantees that
 * no frontend transaction still references this backend, but does not drain or
 * stop asynchronous backend output. The owner must still complete backend
 * deinit before releasing its resources. Detaching the final backend releases
 * the borrowed array; otherwise the logger retains exclusive access to it.
 *
 * @param[in] backend Backend object, non-NULL; never dereferenced by this call.
 * @retval ESY_OK Detached, already detached, or a different backend is bound.
 * @retval ESY_ERR_BUSY A frontend transaction is active; retain all resources
 * and retry later with a caller-defined deadline. No binding was changed.
 * @retval ESY_ERR_INVALID_ARG NULL backend.
 */
extern esy_err_t esy_logger_detach(const esy_logger_backend_t *backend);

/**
 * @brief Set the global log filter level.
 *
 * Log messages more verbose than the specified threshold will be dropped.
 *
 * Safe concurrently with logging. Values outside the public enum are ignored.
 *
 * @param[in] level Most verbose level to output (ERROR through VERBOSE).
 */
extern void esy_logger_set_filter_level(esy_log_level_t level);

/**
 * @brief Print a log message.
 *
 * Outputs a formatted log message according to the current
 * global and per-backend filter levels and the bound backend implementations.
 * This function behaves similarly to printf.
 *
 * @param[in] tag   Optional textual tag (module or subsystem name); may be NULL.
 * @param[in] level Log severity level.
 * @param[in] fmt   printf-style format string; NULL is treated as an empty body.
 * @param[in] ...   Variable arguments.
 *
 * @note This function is typically synchronous and may block.
 *       If the backend supports buffering or DMA, the exact behavior
 *       is backend-specific.
 *
 * @warning Task context only; formatting and logging are forbidden in ISRs.
 * @note Whole lines are serialized with a nonblocking transaction guard.
 * Concurrent/reentrant calls drop their complete line while another call owns
 * the guard. The guard does not mask interrupts. Buffer exhaustion or output
 * failure may truncate a line on that backend; subsequent backends are still
 * attempted. The line is formatted once, then delivered in array order with
 * backend-specific chunking. A synchronous backend may delay later backends
 * and the entire call. Do not mix direct buffer submissions with the
 * frontend on the same backend without external serialization.
 */
extern void esy_log(const char *tag,
                    esy_log_level_t level,
                    const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* ESY_LOGGER_H */
