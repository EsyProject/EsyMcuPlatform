/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_COREDUMP_H
#define ESY_REMOTE_MANAGER_COREDUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/diagnostics/esy_coredump_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Coredump collection commands. */
typedef enum {
    ESY_REMOTE_MANAGER_COREDUMP_INFO = 1, /**< Query whether a record is available. */
    ESY_REMOTE_MANAGER_COREDUMP_READ = 2, /**< Read an explicitly bounded range. */
    ESY_REMOTE_MANAGER_COREDUMP_CLEAR = 3, /**< Explicit authorized discard. */
} esy_remote_manager_coredump_command_t;

/** @brief READ argument content: record ID, offset:u32 and length:u32. */
#define ESY_REMOTE_MANAGER_COREDUMP_READ_BYTES (ESY_COREDUMP_RECORD_ID_BYTES + 8u)

/** @brief Record identity; the record itself carries provider-specific metadata. */
typedef struct {
    uint32_t record_size; /**< Complete readable byte length; zero when unavailable. */
    uint32_t record_crc; /**< Integrity CRC, not record identity; zero when unavailable. */
    bool available; /**< False means no collected record, not a validation failure. */
    esy_coredump_record_id_t record_id; /**< Nonzero immutable generation ID; zero when unavailable. */
} esy_remote_manager_coredump_info_t;

/**
 * @brief Optional coredump owner operations; no capture or persistence lifecycle.
 *
 * All calls run synchronously on one serialized non-ISR owner, without
 * concurrent fault capture or provider mutation. Context remains borrowed
 * until manager deinit. INFO/READ never clear a record. READ copies exactly
 * length bytes at offset on OK, validates the range, and retains no output.
 * READ/CLEAR compare the complete nonzero record ID with the authoritative
 * current record before copying output or changing storage. A stale ID returns
 * STATE without touching another record. This includes zero-length reads.
 * Identical fault bytes and CRCs from separate captures have distinct IDs.
 * NULL output is valid at length zero. CLEAR is optional: its owner must clear
 * every authoritative persistence source in the correct order or leave it
 * unsupported. A failed clear may have changed state; no implicit retry occurs.
 * Callback cost belongs to dispatch latency, including any storage operations.
 */
typedef struct {
    esy_err_t (*info)(void *context, esy_remote_manager_coredump_info_t *info); /**< Query record. */
    esy_err_t (*read)(void *context, const esy_coredump_record_id_t *record_id,
                      uint32_t offset, uint8_t *output,
                      size_t length); /**< Copy one record range. */
    esy_err_t (*clear)(void *context, const esy_coredump_record_id_t *record_id); /**< Discard exact record. */
    void *context; /**< Borrowed provider context; NULL is allowed. */
} esy_remote_manager_coredump_ops_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_REMOTE_MANAGER_COREDUMP_H */
