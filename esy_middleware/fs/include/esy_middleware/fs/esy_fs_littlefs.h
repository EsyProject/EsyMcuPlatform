/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FS_LITTLEFS_H
#define ESY_FS_LITTLEFS_H

#include "esy_middleware/fs/esy_fs.h"
#include "lfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Caller-owned native file pool slot; fields are private by contract. */
typedef struct {
    lfs_file_t native; /**< Native object, retained until close even after I/O errors. */
    struct lfs_file_config config; /**< Persistent per-file cache configuration. */
    bool in_use; /**< Private pool ownership. */
} esy_fs_littlefs_file_t;

/** @brief Caller-owned native directory pool slot; fields are private by contract. */
typedef struct {
    lfs_dir_t native; /**< Native iterator. */
    bool in_use; /**< Private pool ownership. */
} esy_fs_littlefs_dir_t;

/**
 * @brief Exclusive, caller-owned littlefs resources; need not be initialized.
 * All spans are disjoint, naturally aligned for their types and outlive backend
 * deinit. Cache buffers are byte arrays; file caches hold file_count*cache_size
 * bytes. A zero directory count permits file-only applications.
 */
typedef struct {
    esy_fs_littlefs_file_t *files; /**< Typed file pool, at least one slot. */
    size_t file_count; /**< Number of file slots. */
    esy_fs_littlefs_dir_t *directories; /**< Typed directory pool, or NULL for zero count. */
    size_t directory_count; /**< Number of directory slots. */
    void *read_buffer; /**< Shared read cache. */
    size_t read_capacity; /**< Read buffer capacity in bytes. */
    void *program_buffer; /**< Shared program cache, accessible to the media provider. */
    size_t program_capacity; /**< Program buffer capacity in bytes. */
    void *lookahead_buffer; /**< Block allocation bitmap. */
    size_t lookahead_capacity; /**< Bitmap capacity in bytes. */
    void *file_buffers; /**< Contiguous per-file caches, accessible to the media provider. */
    size_t file_buffers_capacity; /**< Combined file-cache capacity in bytes. */
} esy_fs_littlefs_static_resources_t;

/**
 * @brief littlefs-specific configuration; native ABI intentionally exposed.
 * Tested with official littlefs 2.11.2, built with LFS_NO_MALLOC and without
 * LFS_READONLY, LFS_THREADSAFE or LFS_MULTIVERSION. All consumers must use the
 * same provider target and configuration. Device is copied; its media context
 * and callback code outlive backend deinit. read/prog/erase/sync, explicit
 * geometry, cache_size, lookahead_size and block_cycles (-1 or 1..INT32_MAX-1)
 * are required.
 * Native read/prog/lookahead buffer fields must be NULL: resources supply them.
 */
typedef struct {
    const struct lfs_config *device; /**< Borrowed descriptor copied during init. */
    const esy_fs_littlefs_static_resources_t *static_resources; /**< Copied resource descriptor. */
} esy_fs_littlefs_config_t;

/**
 * @brief Caller-owned backend, initially {0}; never copy, move or clear live state.
 * Fields are private by contract. One serialized non-ISR owner handles all
 * operations, including direct media access. No HAL or media lifecycle is owned.
 */
typedef struct esy_fs_littlefs {
    const struct esy_fs_littlefs *self; /**< Private instance identity. */
    lfs_t native; /**< Private provider state. */
    struct lfs_config device; /**< Private retained configuration. */
    esy_fs_littlefs_static_resources_t resources; /**< Private retained resources. */
    bool mounted; /**< Private lifecycle state. */
} esy_fs_littlefs_t;

/**
 * @brief Validate and bind static resources without media I/O.
 * @return OK, INVALID_ARG for configuration/overlap, NO_MEM for undersized
 * buffers, or STATE for repeated init/copies. Failure leaves zero handle intact.
 * Calls can block only through subsequent filesystem operations. No OS required.
 */
esy_err_t esy_fs_littlefs_init(esy_fs_littlefs_t *backend,
                              const esy_fs_littlefs_config_t *config);
/**
 * @brief Return instance-bound operations, including atomic whole-file replace.
 * Output is unchanged on failure. Bind at most one live esy_fs_t; its complete
 * lifetime precedes backend deinit. Caller-reserved staging paths are exclusively
 * owned by replace and overwritten on retry, including stale partial files.
 * Durability and power-loss guarantees require the media sync/erase contract.
 */
esy_err_t esy_fs_littlefs_get_ops(esy_fs_littlefs_t *backend, esy_fs_ops_t *ops);
/**
 * @brief Release an unmounted backend after facade deinit; zero deinit succeeds.
 * Mounted state or active pool slots return BUSY and retain every resource.
 * Successful deinit returns the handle to zero, allowing a fresh init.
 */
esy_err_t esy_fs_littlefs_deinit(esy_fs_littlefs_t *backend);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FS_LITTLEFS_H */
