/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SETTINGS_H
#define ESY_SETTINGS_H

/**
 * @file esy_settings.h
 * @brief Caller-owned string/blob settings with explicit snapshot persistence.
 *
 * One startup/task owner externally serializes every call, including queries,
 * and all access to the borrowed file/provider. No operation is ISR/fault safe.
 * The core is synchronous and has no OS, allocation, worker, timer or automatic
 * save. Provider latency, persistence, Flash safety when applicable, multicore,
 * OTA arbitration, power and watchdog policy remain with the consumer.
 *
 * Borrow one initialized logical file exclusively until settings deinit. It
 * must provide READ and ATOMIC_REPLACE. Keep the handle, provider, workspace and
 * backing bytes alive and unchanged. Do not mutate the raw file to bypass the
 * settings schema/staging contract. Settings buffers must not overlap provider
 * workspace; this caller precondition cannot be checked through the file API.
 * Atomic replacement does not imply nonvolatile storage: RAM and Flash follow
 * their own persistence and interruption contracts.
 *
 * Keys are nonempty, case-sensitive NUL-terminated byte strings, bounded by
 * max_key_length excluding the terminator. '/' is an ordinary byte. Values are
 * opaque blobs; an empty value is distinct from an absent key. Set copies both
 * inputs before returning. Inputs/outputs must not alias the settings handle,
 * either settings buffer or the borrowed file handle. Bounds and
 * detectable overlaps are rejected before modifying the working snapshot.
 *
 * Get reads the staged RAM snapshot, including in UNCERTAIN state; it makes no
 * claim about the currently stored value. Commit persists all staged keys as
 * one file replacement. It does not apply product configuration or invoke callbacks.
 * Defaults, value encoding, validation, migration and application belong to
 * the consumer. Unknown keys are retained verbatim.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_middleware/file/esy_file.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle capacity; not a persistent-format or ABI size promise. */
#define ESY_SETTINGS_STORAGE_BYTES 128u

/**
 * @brief Caller-owned handle, requiring `{0}` before its first init.
 *
 * Do not copy/move a live handle, modify its bytes, or clear it to bypass deinit.
 * Successful deinit restores the zero state and permits another init.
 */
typedef struct {
  union {
    uint8_t bytes[ESY_SETTINGS_STORAGE_BYTES]; /**< Private implementation bytes. */
    max_align_t alignment; /**< Natural alignment for private state. */
  } storage; /**< In-place state. */
} esy_settings_t;

/** @brief Relationship between the working snapshot and confirmed storage. */
typedef enum {
  ESY_SETTINGS_CLEAN = 0, /**< No edits since successful init/reload/commit. */
  ESY_SETTINGS_DIRTY = 1, /**< Edits exist; no save has been attempted for them. */
  ESY_SETTINGS_UNCERTAIN = 2 /**< A save failed and may already have committed. */
} esy_settings_state_t;

/** @brief RAM borrowed for the complete initialized lifetime; no initial clearing required. */
typedef struct {
  uint8_t *working_buffer; /**< Writable packed working snapshot. */
  uint8_t *scratch_buffer; /**< Separate writable loading/editing workspace. */
  size_t buffer_size; /**< EACH buffer's capacity, from 16 through UINT32_MAX bytes. */
} esy_settings_static_resources_t;

/** @brief Configuration copied by init; descriptor objects need not remain alive. */
typedef struct {
  esy_file_t *file; /**< Exclusively borrowed initialized READ/ATOMIC_REPLACE file. */
  size_t max_key_length; /**< Positive byte limit, excluding NUL; at most UINT32_MAX and below SIZE_MAX. */
  const esy_settings_static_resources_t *static_resources; /**< Two disjoint buffers. */
} esy_settings_config_t;

/** @brief RAM-only query result; no provider access is performed. */
typedef struct {
  esy_settings_state_t state; /**< Current staging/persistence state. */
  bool has_snapshot; /**< A data snapshot was present at last successful load/save. */
  uint32_t entry_count; /**< Number of keys in the working snapshot. */
  size_t used_bytes; /**< Encoded working size, including its envelope. */
  size_t capacity; /**< Lesser of buffer capacity and file capacity. */
} esy_settings_info_t;

/**
 * @brief Validate resources and load the complete snapshot without replacing the file.
 * @param[in,out] settings Zero-initialized or successfully deinitialized handle.
 * @param[in] config Required resource and key-limit configuration.
 * @retval ESY_OK Loaded, or initialized empty with has_snapshot=false.
 * @retval ESY_ERR_INVALID_ARG NULL, invalid key limit or detectable storage overlap.
 * @retval ESY_ERR_NO_MEM Buffers/media cannot hold the envelope/existing snapshot,
 * or an existing key exceeds the configured key-length budget.
 * @retval ESY_ERR_STATE Repeated init, invalid handle or unusable file.
 * @retval ESY_ERR_NOT_SUPPORTED Missing READ/ATOMIC_REPLACE capability or
 * unknown snapshot magic or format version.
 * @retval ESY_ERR_CRC_FAIL Malformed snapshot, including duplicate/invalid keys.
 * @retval <0 Other file/provider failure, propagated without writing defaults.
 * @note Failure preserves the handle; backing buffers may change. An absent
 * file is reported as has_snapshot=false, never auto-formatted.
 * Provider errors and malformed existing snapshots are never treated as absence.
 */
esy_err_t esy_settings_init(esy_settings_t *settings,
                            const esy_settings_config_t *config);

/**
 * @brief Release borrowed resources, intentionally discarding all staged edits.
 * @param[in,out] settings Initialized handle or its zero initial state.
 * @return ESY_OK, ESY_ERR_INVALID_ARG for NULL, or ESY_ERR_STATE for an invalid/copied handle.
 * @note No save or provider access occurs, including for DIRTY/UNCERTAIN state.
 * Repeated deinit of a zero handle succeeds. Borrowed file/provider lifecycle
 * remains with its owner. Commit before deinit when edits must be retained.
 */
esy_err_t esy_settings_deinit(esy_settings_t *settings);

/**
 * @brief Copy a staged value; valid also in UNCERTAIN state.
 * @param[in] settings Initialized handle.
 * @param[in] key Bounded, nonempty NUL-terminated key.
 * @param[out] buffer Destination; NULL only when capacity is zero.
 * @param[in] capacity Writable destination bytes.
 * @param[out] length Required, disjoint output for actual/required value length.
 * @param[out] found Required, disjoint output distinguishing absent and empty values.
 * @retval ESY_OK Found value copied, or absent key (found=false, length=0).
 * @retval ESY_ERR_NO_MEM Found value needs more bytes: found=true and length is
 * required size; buffer remains unchanged. NULL/zero can therefore query size.
 * @retval ESY_ERR_INVALID_ARG Invalid key/outputs or detectable overlap.
 * @retval ESY_ERR_STATE Invalid/uninitialized/copied handle.
 * @note Other errors leave outputs unchanged. Output ranges must not overlap
 * one another. Get borrows no output memory after returning.
 */
esy_err_t esy_settings_get(const esy_settings_t *settings, const char *key,
                           void *buffer, size_t capacity, size_t *length,
                           bool *found);

/**
 * @brief Insert/replace a copied value in RAM without replacing the file.
 * @param[in,out] settings Initialized CLEAN or DIRTY handle.
 * @param[in] key Bounded nonempty NUL-terminated key, copied without its terminator.
 * @param[in] value Readable immutable bytes; NULL only when length is zero.
 * @param[in] length Value length in bytes.
 * @return ESY_OK, INVALID_ARG, NO_MEM or STATE. Errors preserve the working snapshot.
 * @note Unchanged values do not mark a clean snapshot dirty. Successful changes
 * mark DIRTY conservatively; reverting changes does not necessarily clear it.
 * UNCERTAIN rejects mutation until a successful reload or deinit/reinit.
 */
esy_err_t esy_settings_set(esy_settings_t *settings, const char *key,
                           const void *value, size_t length);

/**
 * @brief Remove a staged key; removing an absent key succeeds without changes.
 * @param[in,out] settings Initialized CLEAN or DIRTY handle.
 * @param[in] key Bounded nonempty NUL-terminated key.
 * @return ESY_OK, INVALID_ARG or STATE. Errors preserve the working snapshot.
 * @note No provider access; a successful removal marks DIRTY. UNCERTAIN rejects it.
 */
esy_err_t esy_settings_remove(esy_settings_t *settings, const char *key);

/**
 * @brief Persist the entire working snapshot as one atomic file replacement.
 * @param[in,out] settings Initialized CLEAN or DIRTY handle.
 * @retval ESY_OK No edits, or all staged keys accepted by the file provider.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_STATE Invalid/copied handle or unresolved UNCERTAIN state.
 * @retval <0 File/provider failure. A failed save may already have committed:
 * state becomes UNCERTAIN and set/remove/commit are rejected until reload succeeds.
 * @note Working bytes are retained on failure. CLEAN commit never writes, even
 * without a prior snapshot. Removing every key commits a valid empty snapshot.
 * This call is synchronous and may block according to the selected provider.
 */
esy_err_t esy_settings_commit(esy_settings_t *settings);

/**
 * @brief Reload provider state, discarding staged changes only after full validation.
 * @param[in,out] settings Initialized handle in any state.
 * @return ESY_OK, INVALID_ARG, NO_MEM, STATE, NOT_SUPPORTED, CRC_FAIL or a propagated
 * file/provider error. Errors preserve the working bytes and all query state.
 * @note Success publishes CLEAN with has_snapshot indicating whether a data
 * snapshot exists. After a failed save it may load either the old or new snapshot;
 * query values again. Scratch bytes may change on error. No file replacement occurs.
 */
esy_err_t esy_settings_reload(esy_settings_t *settings);

/**
 * @brief Query staging state and resource use without calling the file provider.
 * @param[in] settings Initialized handle.
 * @param[out] info Required output disjoint from all module-owned/borrowed storage.
 * @return ESY_OK, ESY_ERR_INVALID_ARG or ESY_ERR_STATE; output unchanged on error.
 * @note has_snapshot describes last confirmed state; in UNCERTAIN it does not
 * establish which snapshot is currently durable.
 */
esy_err_t esy_settings_get_info(const esy_settings_t *settings,
                                esy_settings_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* ESY_SETTINGS_H */
