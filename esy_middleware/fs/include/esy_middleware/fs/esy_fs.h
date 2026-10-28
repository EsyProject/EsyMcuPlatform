/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FS_H
#define ESY_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum bytes in a canonical volume-relative absolute path. */
#define ESY_FS_PATH_MAX 255u
/** @brief Maximum bytes in a returned directory entry name. */
#define ESY_FS_NAME_MAX 255u

/** @brief Filesystem-specific errors; generic failures use ESY_ERR_* codes. */
typedef enum {
    ESY_FS_ERR_NOT_FOUND = -1001, /**< Path does not exist. */
    ESY_FS_ERR_EXISTS = -1002, /**< Exclusive creation found an existing path. */
    ESY_FS_ERR_NO_SPACE = -1003, /**< Media allocation capacity exhausted. */
    ESY_FS_ERR_READ_ONLY = -1004, /**< Operation requires writable media. */
    ESY_FS_ERR_CORRUPT = -1005, /**< Invalid persistent filesystem structures. */
    ESY_FS_ERR_NOT_DIR = -1006, /**< A path component is not a directory. */
    ESY_FS_ERR_IS_DIR = -1007, /**< Operation requires a regular file. */
    ESY_FS_ERR_NAME_TOO_LONG = -1008, /**< Backend name or path limit exceeded. */
    ESY_FS_ERR_NO_MEDIA = -1009, /**< Removable media is absent or not ready. */
} esy_fs_error_t;

/** @brief File access and creation flags. At least READ or WRITE is required. */
typedef enum {
    ESY_FS_OPEN_READ = 1u << 0, /**< Read access. */
    ESY_FS_OPEN_WRITE = 1u << 1, /**< Write access. */
    ESY_FS_OPEN_CREATE = 1u << 2, /**< Create if absent; requires WRITE. */
    ESY_FS_OPEN_EXCLUSIVE = 1u << 3, /**< Fail if present; requires CREATE. */
    ESY_FS_OPEN_TRUNCATE = 1u << 4, /**< Truncate on open; requires WRITE. */
    ESY_FS_OPEN_APPEND = 1u << 5, /**< Write at end; requires WRITE. */
} esy_fs_open_flags_t;

/** @brief Seek origin. A backend may reject unrepresentable positions. */
typedef enum {
    ESY_FS_SEEK_SET = 0, /**< Relative to file start. */
    ESY_FS_SEEK_CUR = 1, /**< Relative to current position. */
    ESY_FS_SEEK_END = 2, /**< Relative to committed/current file end. */
} esy_fs_whence_t;

/** @brief Type of one filesystem path. */
typedef enum {
    ESY_FS_TYPE_FILE = 1, /**< Regular file. */
    ESY_FS_TYPE_DIR = 2, /**< Directory. */
} esy_fs_type_t;

/** @brief Metadata snapshot; size is zero for directories. */
typedef struct {
    esy_fs_type_t type; /**< Path type. */
    uint64_t size; /**< File length in bytes. */
} esy_fs_stat_t;

/** @brief Media space snapshot, not a reservation or per-file capacity. */
typedef struct {
    uint64_t capacity_bytes; /**< Total allocatable volume capacity. */
    uint64_t free_bytes; /**< Approximate available capacity at query time. */
} esy_fs_info_t;

/** @brief Directory entry; dot entries are omitted by backends. */
typedef struct {
    char name[ESY_FS_NAME_MAX + 1u]; /**< NUL-terminated entry basename. */
    esy_fs_stat_t stat; /**< Entry metadata. */
} esy_fs_entry_t;

/**
 * @brief Synchronous backend contract, copied by esy_fs_init().
 *
 * A single non-ISR task/main-loop owner serializes every facade, backend,
 * lifecycle and direct-media operation. A busy guard detects reentry only; it
 * is not a lock. No automatic task, timer, retry, format or heap is supplied.
 * One backend context is exclusively bound to one live facade; its typed native
 * resources and context outlive that facade. All callbacks except replace are
 * required. Backend init/deinit and media lifecycle remain with their owner.
 *
 * Paths are canonical absolute paths within this volume, at most PATH_MAX bytes,
 * with no empty, dot, dot-dot, backslash, colon or control-character component.
 * No global namespace, working directory or cross-volume rename is provided.
 * Bytes >= 0x80 are passed through; backend encoding restrictions still apply.
 *
 * Successful open/dir_open lends one native object until close. Failed opens
 * must release acquired resources. close/dir_close always set released, even on
 * failure: true means the native object was consumed and cannot be retried;
 * false retains it for a retry. Native resources must never reference a moved
 * facade file/directory handle. unmount failure retains the mounted volume.
 *
 * read/write return status and set transferred only on success. Short reads,
 * including zero-byte EOF, are successful. Short writes are permitted and must
 * be inspected by the caller. A failure can follow partial I/O or durable
 * mutation; output/buffer contents are not authoritative on failure. sync/close
 * errors must be observed. Durability is limited by the actual media sync
 * contract. No general multi-file transaction or secure deletion is implied.
 *
 * replace, when present, publishes the complete data at path using an exclusive
 * caller-reserved temporary path on the same volume. After interruption a
 * surviving volume exposes the old or new complete target. The temporary path
 * can be left behind after failure and is exclusively owned by this operation;
 * it must differ from path and must not name somebody else's data. Replacement
 * requires enough free space for staging. Errors may follow publication.
 *
 * Output snapshots remain unchanged on errors. Caller buffers must be accessible
 * for their declared spans and disjoint from handles and borrowed resources.
 */
typedef struct {
    void *context; /**< Borrowed initialized backend. */
    esy_err_t (*mount)(void *context); /**< Mount without formatting. */
    esy_err_t (*unmount)(void *context); /**< Release a mounted volume. */
    esy_err_t (*format)(void *context); /**< Destructive explicit unmounted operation. */
    esy_err_t (*info)(void *context, esy_fs_info_t *info); /**< Query space. */
    esy_err_t (*open)(void *context, const char *path, uint32_t flags, void **object); /**< Open file. */
    esy_err_t (*close)(void *context, void *object, bool *released); /**< Close file. */
    esy_err_t (*read)(void *context, void *object, void *data, size_t size, size_t *transferred); /**< Read span. */
    esy_err_t (*write)(void *context, void *object, const void *data, size_t size, size_t *transferred); /**< Write span. */
    esy_err_t (*seek)(void *context, void *object, int64_t offset, esy_fs_whence_t whence, uint64_t *position); /**< Seek. */
    esy_err_t (*sync)(void *context, void *object); /**< Commit pending file writes. */
    esy_err_t (*stat)(void *context, const char *path, esy_fs_stat_t *stat); /**< Query path. */
    esy_err_t (*remove)(void *context, const char *path); /**< Remove file or empty directory. */
    esy_err_t (*rename)(void *context, const char *from, const char *to); /**< Backend rename semantics apply. */
    esy_err_t (*mkdir)(void *context, const char *path); /**< Create one directory. */
    esy_err_t (*dir_open)(void *context, const char *path, void **object); /**< Open iterator. */
    esy_err_t (*dir_read)(void *context, void *object, esy_fs_entry_t *entry, bool *end); /**< Read next entry. */
    esy_err_t (*dir_close)(void *context, void *object, bool *released); /**< Close iterator. */
    esy_err_t (*replace)(void *context, const char *path, const char *temporary_path,
                         const void *data, size_t size); /**< Optional durable atomic replacement. */
} esy_fs_ops_t;

/**
 * @brief Caller-owned volume; initialize with {0} before first use.
 * Fields are private by contract. Never copy, move or clear a live handle.
 * Stop borrowers, release leases, close objects and unmount before deinit.
 */
typedef struct esy_fs {
    esy_fs_ops_t ops; /**< Private copied operations. */
    const struct esy_fs *self; /**< Private ownership identity. */
    size_t open_files; /**< Private open file count. */
    size_t open_dirs; /**< Private directory count. */
    size_t leases; /**< Private long-lived borrower count. */
    bool mounted; /**< Private mount state. */
    bool busy; /**< Private reentry guard. */
} esy_fs_t;

/** @brief Caller-owned file, initially {0}; do not copy/move while open. */
typedef struct esy_fs_file {
    esy_fs_t *volume; /**< Private owner volume. */
    void *object; /**< Private borrowed native file. */
    const struct esy_fs_file *self; /**< Private ownership identity. */
    uint32_t flags; /**< Private access flags. */
} esy_fs_file_t;

/** @brief Caller-owned directory iterator, initially {0}; do not copy/move. */
typedef struct esy_fs_dir {
    esy_fs_t *volume; /**< Private owner volume. */
    void *object; /**< Private borrowed native iterator. */
    const struct esy_fs_dir *self; /**< Private ownership identity. */
} esy_fs_dir_t;

/** @brief Bind operations without media I/O. Repeated init returns STATE. */
esy_err_t esy_fs_init(esy_fs_t *fs, const esy_fs_ops_t *ops);
/** @brief Detach an unmounted, unborrowed volume; repeated zero deinit succeeds. */
esy_err_t esy_fs_deinit(esy_fs_t *fs);
/** @brief Mount explicitly; failure preserves unmounted state and never formats. */
esy_err_t esy_fs_mount(esy_fs_t *fs);
/** @brief Unmount; open objects/leases return BUSY, failures retain state. */
esy_err_t esy_fs_unmount(esy_fs_t *fs);
/** @brief Explicit destructive format; mounted/borrowed volumes return BUSY. */
esy_err_t esy_fs_format(esy_fs_t *fs);
/** @brief Query mounted volume space; output unchanged on failure. */
esy_err_t esy_fs_get_info(esy_fs_t *fs, esy_fs_info_t *info);
/** @brief Acquire a mounted-volume lease for a long-lived file adapter. */
esy_err_t esy_fs_retain(esy_fs_t *fs);
/** @brief Release one previously acquired lease; unmatched release returns STATE. */
esy_err_t esy_fs_release(esy_fs_t *fs);
/** @brief Query optional atomic replacement without storage I/O. */
esy_err_t esy_fs_supports_atomic_replace(esy_fs_t *fs, bool *supported);
/** @brief Open using backend-owned caller-supplied pools; failures leave file zero. */
esy_err_t esy_fs_open(esy_fs_t *fs, esy_fs_file_t *file, const char *path, uint32_t flags);
/** @brief Close; a failed backend close may release the handle, see backend contract. */
esy_err_t esy_fs_close(esy_fs_file_t *file);
/** @brief Read up to size bytes, reporting short reads/EOF; NULL data only for size=0. */
esy_err_t esy_fs_read(esy_fs_file_t *file, void *data, size_t size, size_t *transferred);
/** @brief Write up to size bytes; success need not mean durable or complete. */
esy_err_t esy_fs_write(esy_fs_file_t *file, const void *data, size_t size, size_t *transferred);
/** @brief Seek using signed offset; output unchanged on failure. */
esy_err_t esy_fs_seek(esy_fs_file_t *file, int64_t offset, esy_fs_whence_t whence, uint64_t *position);
/** @brief Synchronize a file with its media provider. */
esy_err_t esy_fs_sync(esy_fs_file_t *file);
/** @brief Query an existing path; absence is NOT_FOUND, not an empty file. */
esy_err_t esy_fs_stat(esy_fs_t *fs, const char *path, esy_fs_stat_t *stat);
/** @brief Remove a file/empty directory; absence is NOT_FOUND. */
esy_err_t esy_fs_remove(esy_fs_t *fs, const char *path);
/** @brief Rename within one volume; replacement behavior is backend-specific. */
esy_err_t esy_fs_rename(esy_fs_t *fs, const char *from, const char *to);
/** @brief Create one directory; parents must already exist. */
esy_err_t esy_fs_mkdir(esy_fs_t *fs, const char *path);
/** @brief Open a directory iterator from the backend pool. */
esy_err_t esy_fs_dir_open(esy_fs_t *fs, esy_fs_dir_t *dir, const char *path);
/** @brief Read next entry; end=true leaves entry unchanged. Errors preserve outputs. */
esy_err_t esy_fs_dir_read(esy_fs_dir_t *dir, esy_fs_entry_t *entry, bool *end);
/** @brief Close an iterator; failed close can retain the handle for retry. */
esy_err_t esy_fs_dir_close(esy_fs_dir_t *dir);
/** @brief Atomically publish one complete file if the backend supports it. */
esy_err_t esy_fs_replace(esy_fs_t *fs, const char *path, const char *temporary_path,
                         const void *data, size_t size);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FS_H */
