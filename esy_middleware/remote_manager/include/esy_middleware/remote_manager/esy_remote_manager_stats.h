/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_STATS_H
#define ESY_REMOTE_MANAGER_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum metric name bytes, excluding its trailing NUL. */
#define ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES 48u
/** @brief ENUMERATE argument content: one zero-based metric index. */
#define ESY_REMOTE_MANAGER_STATS_INDEX_BYTES 4u

/** @brief Read-only commands in the statistics management group. */
typedef enum {
    ESY_REMOTE_MANAGER_STATS_ENUMERATE = 1, /**< Read one named metric by index. */
    ESY_REMOTE_MANAGER_STATS_READ = 2, /**< Read one named metric by exact key. */
} esy_remote_manager_stats_command_t;

/** @brief Interpretation of an unsigned, unscaled 64-bit sample. */
typedef enum {
    ESY_REMOTE_MANAGER_STATS_COUNTER = 1, /**< Cumulative value within one epoch. */
    ESY_REMOTE_MANAGER_STATS_GAUGE = 2, /**< Current value or owner-defined watermark. */
} esy_remote_manager_stats_kind_t;

/** @brief Units of the exact integer value; no implicit tick or byte conversion. */
typedef enum {
    ESY_REMOTE_MANAGER_STATS_UNIT_NONE = 0, /**< Dimensionless owner-defined value. */
    ESY_REMOTE_MANAGER_STATS_UNIT_COUNT = 1, /**< Number of events or objects. */
    ESY_REMOTE_MANAGER_STATS_UNIT_BYTES = 2, /**< Bytes. */
    ESY_REMOTE_MANAGER_STATS_UNIT_MILLISECONDS = 3, /**< Milliseconds. */
    ESY_REMOTE_MANAGER_STATS_UNIT_WORDS = 4, /**< Provider-defined machine/stack words. */
    ESY_REMOTE_MANAGER_STATS_UNIT_PERCENT = 5, /**< Whole percentage points, zero to 100. */
} esy_remote_manager_stats_unit_t;

/**
 * @brief One coherent provider-owned sample, copied synchronously on query.
 * Zero is a valid available measurement. Unavailable samples return value zero
 * with available false. Epoch zero means continuity is unspecified; a nonzero
 * epoch identifies a measurement lifetime. Owners change it on counter reset,
 * wrap or producer restart, so clients only compare cumulative samples within
 * the same nonzero epoch. Saturate rather than silently wrapping at UINT64_MAX,
 * or start a new epoch. A volatile boot epoch does not imply durable identity
 * across device resets. The provider documents its actual epoch uniqueness.
 * No reset, sampling, overflow extension or persistence is performed here.
 */
typedef struct {
    uint64_t value; /**< Exact unsigned measurement; zero when unavailable. */
    uint64_t epoch; /**< Measurement lifetime, or zero when unspecified. */
    bool available; /**< Distinguishes missing data from a measured zero. */
} esy_remote_manager_stats_sample_t;

/**
 * @brief One metric and its sample, independent of wire/storage representation.
 * With count greater than zero, index is less than count, name_length is 1..48,
 * name contains only ASCII letters, digits, '_', '.', '-', or '/', and its
 * terminating NUL is at name_length. Trailing name storage is ignored. Kind
 * and unit are valid enumerators. PERCENT values must be at most 100 when
 * available. ENUMERATE index zero may return an entirely zero snapshot for an
 * empty inventory; other out-of-range indices return INVALID_ARG. READ never
 * succeeds with an empty inventory. Enumeration is live, not a retained or
 * cross-metric snapshot. Keep identities/order stable or restart enumeration.
 */
typedef struct {
    uint32_t count; /**< Number of published metrics; no fixed catalogue size. */
    uint32_t index; /**< Zero-based enumeration position. */
    char name[ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES + 1u]; /**< Copied canonical name. */
    size_t name_length; /**< Name bytes excluding the terminating NUL. */
    esy_remote_manager_stats_kind_t kind; /**< Cumulative or current measurement. */
    esy_remote_manager_stats_unit_t unit; /**< Unit of the unscaled integer. */
    esy_remote_manager_stats_sample_t sample; /**< Coherent value and continuity identity. */
} esy_remote_manager_stats_snapshot_t;

/**
 * @brief Optional synchronous, read-only statistics source copied by manager init.
 *
 * A single task/main-loop owner externally serializes calls with producer
 * updates and lifecycle changes. Never use from ISR. For ISR/task or other
 * asynchronous producers, the source must obtain a coherent snapshot using its
 * own supported handoff; a 64-bit read is not assumed atomic on a 32-bit MCU.
 * Each manager dispatch authorizes and calls at most one operation. Input and
 * output pointers are borrowed only until return; callbacks cannot reenter the
 * manager. Context and dependencies outlive all copied operations.
 *
 * ENUMERATE returns the requested position. READ matches the complete exact name
 * and returns that descriptor and sample; an unpublished name returns
 * NOT_SUPPORTED, while temporarily unavailable data succeeds with available
 * false/value zero. NULL callbacks are unsupported and not advertised. Outputs
 * are authoritative only on OK. No query clears or increments a measurement.
 * The provider owns collection, sampling cadence, kind/unit semantics and
 * reset/overflow/epoch handling. This API adds no task, timer, clock or storage
 * writes. In particular, never widen a wrapping 32-bit tick and claim 64-bit
 * uptime: extension and observation cadence belong to the time source owner.
 * Persistent reboot counts require an explicitly provisioned external source;
 * no Flash partition is selected, erased, shared or written by this module.
 */
typedef struct {
    esy_err_t (*enumerate)(void *context, uint32_t index,
                           esy_remote_manager_stats_snapshot_t *snapshot); /**< Read by index. */
    esy_err_t (*read)(void *context, const char *name, size_t name_length,
                      esy_remote_manager_stats_snapshot_t *snapshot); /**< Read by exact name. */
    void *context; /**< Borrowed statistics owner; NULL is permitted. */
} esy_remote_manager_stats_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_REMOTE_MANAGER_STATS_H */
