/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FILE_FS_H
#define ESY_FILE_FS_H

#include "esy_middleware/file/esy_file.h"
#include "esy_middleware/fs/esy_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind one existing filesystem volume and a reserved path.
 *
 * Paths are borrowed, canonical volume-relative absolute paths and remain
 * unchanged/alive until successful deinit. capacity is the binding's maximum
 * complete file size, not the currently available filesystem space. The owner
 * serializes all access to this target, temporary path and volume. No other
 * actor may mutate either path while a domain consumer borrows this binding.
 * A writable atomic backend requires a distinct temporary_path reserved solely
 * for staging this target; stale staging bytes may be overwritten. A backend
 * without atomic replacement exposes READ and REMOVE only and ignores no
 * reliability failure by substituting in-place writes.
 */
typedef struct {
    esy_fs_t *volume; /**< Borrowed, mounted volume. */
    const char *path; /**< Borrowed target path. */
    const char *temporary_path; /**< Reserved staging path, NULL for read/remove only. */
    uint32_t capacity; /**< Maximum allowed content bytes, including zero. */
} esy_file_fs_config_t;

/**
 * @brief Caller-owned binding requiring {0}; fields private by contract.
 * The volume is retained until successful deinit. Detach domain borrowers and
 * esy_file before this binding, then unmount/deinitialize the volume. Never copy,
 * move or reset a live binding. There is no hidden heap, task or OS dependency.
 */
typedef struct esy_file_fs {
    const struct esy_file_fs *self; /**< Private ownership identity. */
    esy_file_fs_config_t config; /**< Private copied binding. */
    esy_fs_file_t active_file; /**< Private close-retry resource. */
    bool atomic_replace; /**< Private backend capability snapshot. */
    bool busy; /**< Private synchronous reentry guard. */
} esy_file_fs_t;

/**
 * @brief Retain a mounted volume and validate paths without storage mutation.
 * Backend atomic capability and temporary_path together enable CREATE/REPLACE.
 * A missing temporary path deliberately binds READ/REMOVE only. Invalid config
 * returns INVALID_ARG, repeated/copied handles STATE, and volume errors propagate.
 * Failure leaves the zero handle available for a retry.
 */
esy_err_t esy_file_fs_init(esy_file_fs_t *adapter, const esy_file_fs_config_t *config);
/**
 * @brief Retry pending close then release the volume lease; repeated deinit succeeds.
 * Close/release errors retain this binding and resources for retry. Do not clear
 * it to bypass cleanup. A failed close may consume its file handle; mutation
 * effects still require authoritative requery before any retry.
 */
esy_err_t esy_file_fs_deinit(esy_file_fs_t *adapter);
/** @brief Copy operations without media I/O; binding must outlive all copies. */
esy_err_t esy_file_fs_get_ops(esy_file_fs_t *adapter, esy_file_ops_t *ops);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FILE_FS_H */
