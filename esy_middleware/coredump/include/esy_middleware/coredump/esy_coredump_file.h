/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_COREDUMP_FILE_H
#define ESY_COREDUMP_FILE_H

#include "esy_middleware/coredump/esy_coredump.h"
#include "esy_middleware/file/esy_file.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque caller-owned adapter storage, independent of file format. */
#define ESY_COREDUMP_FILE_STORAGE_BYTES 128u
/** @brief Maximum complete version-one record, including bounded stack bytes. */
#define ESY_COREDUMP_MAX_RECORD_BYTES (ESY_COREDUMP_HEADER_BYTES + ESY_FAULT_STACK_MAX_BYTES)

/** @brief Caller-owned zero-initialized handle; do not copy or move while live. */
typedef struct {
  union {
    uint8_t bytes[ESY_COREDUMP_FILE_STORAGE_BYTES]; /**< Private state. */
    max_align_t alignment; /**< Private state alignment. */
  } storage; /**< In-place implementation storage. */
} esy_coredump_file_t;

/** @brief Exclusive normal-context workspace borrowed until deinit. */
typedef struct {
  uint8_t *record_buffer; /**< At least MAX_RECORD_BYTES; disjoint from handles, retained RAM and file provider storage. */
  size_t record_capacity; /**< Accessible workspace bytes. */
} esy_coredump_file_static_resources_t;

/** @brief Copied binding of an initialized RAM store and one prebound file. */
typedef struct {
  esy_coredump_t *store; /**< Borrowed initialized store, outliving this adapter. */
  esy_file_t *file; /**< Borrowed initialized file with READ and CREATE capabilities. */
  const esy_coredump_file_static_resources_t *static_resources; /**< Required record workspace. */
} esy_coredump_file_config_t;

/**
 * @brief Bind initialized resources without querying file contents or changing data.
 * One non-ISR task/startup owner serializes every complete adapter operation
 * with capture, file writers and all provider lifecycle changes. The file
 * binding is exclusively used for this record while attached. The file facade
 * and backend own storage placement, durability and recovery, including any
 * physical Flash partition, program scratch or filesystem synchronization.
 * This adapter has no HAL, OS, clock, allocation or fault-context path.
 * @return OK; INVALID_ARG for invalid/overlapping storage; NO_MEM for short
 * workspace; STATE for detached/copied/live handles; NOT_SUPPORTED without
 * READ and CREATE. Failure preserves the handle and all borrowed data.
 */
esy_err_t esy_coredump_file_init(esy_coredump_file_t *adapter,
                               const esy_coredump_file_config_t *config);
/** @brief Detach without clearing RAM or the file; zero/repeated deinit succeeds.
 * @return OK, INVALID_ARG or STATE for an invalid/copied handle. */
esy_err_t esy_coredump_file_deinit(esy_coredump_file_t *adapter);
/** @brief Restore the authoritative complete file into RAM, preserving its identity.
 * @return OK; STATE if absent; CRC_FAIL/NOT_SUPPORTED for invalid contents;
 * NO_MEM, INVALID_ARG or a core/file error. Read failures leave RAM untouched.
 * An older uncollected file deliberately replaces a newer retained record. */
esy_err_t esy_coredump_file_restore(esy_coredump_file_t *adapter);
/** @brief Create only when absent; a byte-identical existing record succeeds.
 * @return OK; BUSY for another valid record; CRC_FAIL/NOT_SUPPORTED for invalid
 * existing contents; NO_MEM or a core/file error. No replacement/reclaim path
 * is used. RAM remains pending. A failed create may already have committed;
 * reconcile by inspecting/retrying the same record on the serialized owner. */
esy_err_t esy_coredump_file_persist(esy_coredump_file_t *adapter);
/**
 * @brief Remove only the identified authoritative record, then clear retained RAM.
 * @param adapter Initialized adapter with a REMOVE-capable file.
 * @param record_id Required nonzero identity matching RAM and any existing file.
 * @return OK; STATE for stale/empty RAM or a different file; NOT_SUPPORTED for
 * missing REMOVE/unknown content; CRC_FAIL or a core/file/identity-source error.
 * The expected identity is copied before workspace use. Existing malformed or
 * unreadable files fail closed before remove. An absent file is allowed only
 * while RAM still matches, including retry after a partially completed clear.
 * File removal precedes core successor-ID allocation; a later failure retains
 * RAM but the file may already be absent. No automatic retry or unconditional
 * discard is supplied. Check-plus-remove is protected by caller serialization,
 * not a cross-writer atomic primitive or an internal lock.
 */
esy_err_t esy_coredump_file_clear(esy_coredump_file_t *adapter,
                                const esy_coredump_record_id_t *record_id);

#ifdef __cplusplus
}
#endif
#endif /* ESY_COREDUMP_FILE_H */
