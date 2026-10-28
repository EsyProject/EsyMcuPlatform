/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FS_FATFS_H
#define ESY_FS_FATFS_H

#include "esy_middleware/fs/esy_fs_block.h"
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Native R0.16 file pool element; fields are private after init. */
typedef struct {
    FIL native; /**< Provider-owned state in caller storage. */
    uint32_t flags; /**< Facade access and append flags. */
    bool used; /**< Borrowed slot. */
} esy_fs_fatfs_file_slot_t;

/** @brief Native R0.16 directory pool element; fields are private after init. */
typedef struct {
    DIR native; /**< Provider-owned iterator in caller storage. */
    bool used; /**< Borrowed slot. */
} esy_fs_fatfs_dir_slot_t;

/**
 * @brief Caller-owned resources; naturally aligned typed objects need not start zero.
 * All spans must be mutually disjoint and outlive the initialized backend.
 */
typedef struct {
    FATFS *volume; /**< One exclusive native filesystem object. */
    esy_fs_fatfs_file_slot_t *files; /**< Exclusive file pool, or NULL for zero count. */
    size_t file_count; /**< File slots; subject to shared FF_FS_LOCK capacity. */
    esy_fs_fatfs_dir_slot_t *dirs; /**< Exclusive iterator pool, or NULL for zero count. */
    size_t dir_count; /**< Directory slots; subject to shared FF_FS_LOCK capacity. */
    void *format_buffer; /**< Explicit f_mkfs scratch space; at least sector_size bytes. */
    size_t format_buffer_size; /**< Scratch bytes, at most UINT_MAX. */
} esy_fs_fatfs_static_resources_t;

/** @brief Immutable backend inputs; copied by init, no hardware access occurs. */
typedef struct {
    esy_fs_block_ops_t block; /**< Initialized caller-owned block medium. */
    const esy_fs_fatfs_static_resources_t *static_resources; /**< Descriptor copied during init. */
} esy_fs_fatfs_config_t;

/**
 * @brief Caller-owned FatFs R0.16 adapter, initialized with {0} before first use.
 *
 * Native types intentionally couple this header to the PUBLIC FatFs provider
 * target and its ffconf.h; all consumers must use the same provider/config ABI.
 * Never copy, move, reset or share native objects while live. Do not call f_*
 * directly while a facade is bound or the backend is mounted.
 * One non-ISR owner serializes ALL FatFs volumes, registry lifecycle and direct
 * medium access; no internal lock, heap, task, retries or timebase is provided.
 * FatFs uses bounded stack LFN scratch and its own global drive/lock tables.
 * Calls can block for multiple sectors and free-space scans; latency belongs to
 * the medium. UTF-8 paths are case insensitive; FAT restrictions still apply.
 * Rename does not replace existing destinations. Seek past EOF is rejected to
 * avoid FatFs extending writable files during seek. No atomic replace promise.
 *
 * A media/I/O or close failure faults the mounted volume. Close then consumes
 * each object without further writes and reports the fault. Once all handles
 * are closed, unmount discards provider caches/locks without accessing media;
 * explicit remount is the recovery boundary. Pending writes may be lost and
 * failed mutations can already be persistent; reconcile contents after recovery.
 * Stop facade borrowers, close all objects, unmount and deinit the facade before
 * backend deinit. Unmount never silently formats an invalid volume.
 */
typedef struct esy_fs_fatfs {
    const struct esy_fs_fatfs *self; /**< Private copy/move detection. */
    esy_fs_block_ops_t block; /**< Private copied device operations. */
    esy_fs_fatfs_static_resources_t resources; /**< Private copied resource descriptor. */
    esy_err_t fault; /**< Private latched mounted-volume fault. */
    esy_err_t media_error; /**< Private last block callback failure. */
    uint8_t drive; /**< Private registered logical/physical drive. */
    bool mounted; /**< Private explicit mount state. */
} esy_fs_fatfs_t;

/**
 * @brief Reserve one drive slot and initialize caller resources without I/O.
 * @return OK; INVALID_ARG for invalid geometry/storage/callbacks; STATE for
 * repeated init/copy; BUSY when resources overlap another registered backend;
 * NO_MEM when the provider drive registry is full. Failure acquires nothing.
 */
esy_err_t esy_fs_fatfs_init(esy_fs_fatfs_t *backend, const esy_fs_fatfs_config_t *config);
/** @brief Release an unmounted backend; repeated zero deinit succeeds.
 * @return OK, INVALID_ARG, STATE for copy, or BUSY while mounted/objects live.
 * The caller must have deinitialized all facades borrowing this backend. */
esy_err_t esy_fs_fatfs_deinit(esy_fs_fatfs_t *backend);
/** @brief Obtain a copied operation binding; output unchanged on errors.
 * @return OK, INVALID_ARG or STATE. Binding outlives the facade; replace is NULL. */
esy_err_t esy_fs_fatfs_get_ops(esy_fs_fatfs_t *backend, esy_fs_ops_t *ops);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FS_FATFS_H */
