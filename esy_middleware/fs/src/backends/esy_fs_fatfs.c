/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/fs/esy_fs_fatfs.h"
#include "diskio.h"
#include <limits.h>
#include <string.h>

#if FF_DEFINED != 80386 || FF_USE_LFN != 2 || FF_LFN_UNICODE != 2 || \
    FF_FS_READONLY || FF_FS_MINIMIZE || !FF_USE_MKFS || FF_FS_REENTRANT || \
    FF_MULTI_PARTITION || FF_STR_VOLUME_ID || FF_FS_EXFAT || FF_LBA64 || \
    FF_MIN_SS != 512 || FF_MAX_SS != 512 || FF_FS_LOCK < 1 || !FF_FS_NORTC
#error "esy_fs_fatfs requires the supplied allocation-free FatFs R0.16 configuration"
#endif

#define FATFS_DRIVE_PREFIX_SIZE 2u
#define FATFS_PATH_SIZE (ESY_FS_PATH_MAX + FATFS_DRIVE_PREFIX_SIZE + 1u)
#define FATFS_SECTOR_SIZE 512u
#define FATFS_RESOURCE_SPANS 5u

/* One owner serializes every registry and FatFs volume operation. */
typedef struct {
    esy_fs_fatfs_t *drives[FF_VOLUMES];
} fs_fatfs_registry_ctx_t;

static fs_fatfs_registry_ctx_t s_ctx;

typedef struct {
    uintptr_t start;
    size_t size;
} fs_fatfs_span_t;

static bool valid_backend(const esy_fs_fatfs_t *backend)
{
    return backend != NULL && backend->self == backend && backend->drive < FF_VOLUMES &&
           s_ctx.drives[backend->drive] == backend;
}

static bool has_objects(const esy_fs_fatfs_t *backend)
{
    for (size_t i = 0; i < backend->resources.file_count; ++i) {
        if (backend->resources.files[i].used) return true;
    }
    for (size_t i = 0; i < backend->resources.dir_count; ++i) {
        if (backend->resources.dirs[i].used) return true;
    }
    return false;
}

static esy_err_t medium_status(esy_fs_fatfs_t *backend, bool writing)
{
    esy_fs_block_status_t status = {0};
    esy_err_t error = backend->block.status(backend->block.context, &status);
    if (error == ESY_OK && !status.ready) error = ESY_FS_ERR_NO_MEDIA;
    if (error == ESY_OK && writing && status.read_only) error = ESY_FS_ERR_READ_ONLY;
    if (error != ESY_OK) {
        backend->media_error = error;
        if (backend->mounted && error != ESY_FS_ERR_READ_ONLY) backend->fault = error;
    }
    return error;
}

static esy_err_t ready(esy_fs_fatfs_t *backend, bool writing)
{
    if (!valid_backend(backend) || !backend->mounted) return ESY_ERR_STATE;
    if (backend->fault != ESY_OK) return backend->fault;
    backend->media_error = ESY_OK;
    return medium_status(backend, writing);
}

static esy_err_t result(esy_fs_fatfs_t *backend, FRESULT res)
{
    esy_err_t error;
    switch (res) {
    case FR_OK:
        /* R0.16 can ignore an auxiliary FSInfo write failure before CTRL_SYNC. */
        if (backend->media_error == ESY_OK) return ESY_OK;
        error = backend->media_error;
        break;
    case FR_NO_FILE:
    case FR_NO_PATH: return ESY_FS_ERR_NOT_FOUND;
    case FR_EXIST: return ESY_FS_ERR_EXISTS;
    case FR_WRITE_PROTECTED: return ESY_FS_ERR_READ_ONLY;
    case FR_NOT_READY: error = ESY_FS_ERR_NO_MEDIA; break;
    case FR_INT_ERR: error = ESY_FS_ERR_CORRUPT; break;
    case FR_DISK_ERR:
    case FR_INVALID_OBJECT:
        error = backend->media_error != ESY_OK ? backend->media_error : ESY_ERR_IO;
        break;
    case FR_NO_FILESYSTEM: return ESY_FS_ERR_CORRUPT;
    case FR_DENIED: return ESY_ERR_FAIL;
    case FR_LOCKED: return ESY_ERR_BUSY;
    case FR_NOT_ENOUGH_CORE:
    case FR_TOO_MANY_OPEN_FILES: return ESY_ERR_NO_MEM;
    case FR_TIMEOUT: return ESY_ERR_TIMEOUT;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER: return ESY_ERR_INVALID_ARG;
    case FR_MKFS_ABORTED: return ESY_FS_ERR_NO_SPACE;
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED: return ESY_ERR_STATE;
    default: error = ESY_ERR_IO; break;
    }
    if (backend->mounted) backend->fault = error;
    return error;
}

static void drive_path(const esy_fs_fatfs_t *backend, char path[3])
{
    path[0] = (char)('0' + backend->drive);
    path[1] = ':';
    path[2] = '\0';
}

static esy_err_t native_path(const esy_fs_fatfs_t *backend, const char *path,
                             char output[FATFS_PATH_SIZE])
{
    if (path == NULL || path[0] != '/') return ESY_ERR_INVALID_ARG;
    size_t length = 0;
    while (path[length] != '\0') {
        if (length >= ESY_FS_PATH_MAX) return ESY_FS_ERR_NAME_TOO_LONG;
        unsigned char ch = (unsigned char)path[length];
        if (ch < 0x20u || strchr("\\:*?\"<>|", ch) != NULL) return ESY_ERR_INVALID_ARG;
        if ((ch == '.' || ch == ' ') && (path[length + 1u] == '/' || path[length + 1u] == '\0'))
            return ESY_ERR_INVALID_ARG;
        ++length;
    }
    drive_path(backend, output);
    memcpy(output + FATFS_DRIVE_PREFIX_SIZE, path, length + 1u);
    return ESY_OK;
}

static esy_fs_fatfs_file_slot_t *file_slot(esy_fs_fatfs_t *backend, void *object)
{
    for (size_t i = 0; i < backend->resources.file_count; ++i) {
        esy_fs_fatfs_file_slot_t *slot = &backend->resources.files[i];
        if (object == slot && slot->used) return slot;
    }
    return NULL;
}

static esy_fs_fatfs_dir_slot_t *dir_slot(esy_fs_fatfs_t *backend, void *object)
{
    for (size_t i = 0; i < backend->resources.dir_count; ++i) {
        esy_fs_fatfs_dir_slot_t *slot = &backend->resources.dirs[i];
        if (object == slot && slot->used) return slot;
    }
    return NULL;
}

static esy_err_t backend_mount(void *context)
{
    esy_fs_fatfs_t *backend = context;
    if (!valid_backend(backend) || backend->mounted) return ESY_ERR_STATE;
    backend->media_error = ESY_OK;
    backend->fault = ESY_OK;
    esy_err_t error = medium_status(backend, false);
    if (error != ESY_OK) return error;
    char path[3];
    drive_path(backend, path);
    error = result(backend, f_mount(backend->resources.volume, path, 1));
    if (error != ESY_OK) {
        (void)f_mount(NULL, path, 0);
        return error;
    }
    backend->mounted = true;
    return ESY_OK;
}

static esy_err_t backend_unmount(void *context)
{
    esy_fs_fatfs_t *backend = context;
    if (!valid_backend(backend) || !backend->mounted) return ESY_ERR_STATE;
    if (has_objects(backend)) return ESY_ERR_BUSY;
    if (backend->fault == ESY_OK) {
        esy_err_t error = medium_status(backend, false);
        if (error == ESY_OK) error = backend->block.sync(backend->block.context);
        if (error != ESY_OK) {
            backend->fault = error;
            return error;
        }
    }
    char path[3];
    drive_path(backend, path);
    /* Unregistering a faulted volume performs no media I/O. */
    backend->media_error = ESY_OK;
    esy_err_t error = result(backend, f_mount(NULL, path, 0));
    if (error != ESY_OK) return error;
    backend->mounted = false;
    backend->fault = ESY_OK;
    memset(backend->resources.volume, 0, sizeof(FATFS));
    for (size_t i = 0; i < backend->resources.file_count; ++i)
        memset(&backend->resources.files[i], 0, sizeof(esy_fs_fatfs_file_slot_t));
    for (size_t i = 0; i < backend->resources.dir_count; ++i)
        memset(&backend->resources.dirs[i], 0, sizeof(esy_fs_fatfs_dir_slot_t));
    return ESY_OK;
}

static esy_err_t backend_format(void *context)
{
    esy_fs_fatfs_t *backend = context;
    if (!valid_backend(backend)) return ESY_ERR_STATE;
    if (backend->mounted || has_objects(backend)) return ESY_ERR_BUSY;
    backend->media_error = ESY_OK;
    esy_err_t error = medium_status(backend, true);
    if (error != ESY_OK) return error;
    char path[3];
    drive_path(backend, path);
    MKFS_PARM options = {0};
    options.fmt = FM_FAT | FM_FAT32;
    return result(backend, f_mkfs(path, &options, backend->resources.format_buffer,
                                 (UINT)backend->resources.format_buffer_size));
}

static esy_err_t backend_info(void *context, esy_fs_info_t *info)
{
    esy_fs_fatfs_t *backend = context;
    if (info == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, false);
    if (error != ESY_OK) return error;
    char path[3];
    drive_path(backend, path);
    DWORD free_clusters;
    FATFS *native;
    error = result(backend, f_getfree(path, &free_clusters, &native));
    if (error != ESY_OK) return error;
    esy_fs_info_t snapshot = {
        .capacity_bytes = ((uint64_t)native->n_fatent - 2u) * native->csize * FATFS_SECTOR_SIZE,
        .free_bytes = (uint64_t)free_clusters * native->csize * FATFS_SECTOR_SIZE,
    };
    *info = snapshot;
    return ESY_OK;
}

static esy_err_t backend_open(void *context, const char *path, uint32_t flags, void **object)
{
    esy_fs_fatfs_t *backend = context;
    if (object == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, (flags & ESY_FS_OPEN_WRITE) != 0u);
    if (error != ESY_OK) return error;
    char native[FATFS_PATH_SIZE];
    error = native_path(backend, path, native);
    if (error != ESY_OK) return error;
    if (path[1] == '\0') return ESY_FS_ERR_IS_DIR;
    esy_fs_fatfs_file_slot_t *slot = NULL;
    for (size_t i = 0; i < backend->resources.file_count; ++i) {
        if (!backend->resources.files[i].used) {
            slot = &backend->resources.files[i];
            break;
        }
    }
    if (slot == NULL) return ESY_ERR_NO_MEM;
    BYTE mode = 0;
    if (flags & ESY_FS_OPEN_READ) mode |= FA_READ;
    if (flags & ESY_FS_OPEN_WRITE) mode |= FA_WRITE;
    if (flags & ESY_FS_OPEN_EXCLUSIVE) mode |= FA_CREATE_NEW;
    else if (flags & ESY_FS_OPEN_CREATE) mode |= FA_OPEN_ALWAYS;
    FILINFO metadata;
    FRESULT res = f_stat(native, &metadata);
    if (res == FR_OK && (metadata.fattrib & AM_DIR)) return ESY_FS_ERR_IS_DIR;
    if (res == FR_OK && (metadata.fattrib & AM_RDO) && (flags & ESY_FS_OPEN_WRITE))
        return ESY_FS_ERR_READ_ONLY;
    if (res != FR_OK && res != FR_NO_FILE && res != FR_NO_PATH) return result(backend, res);
    memset(&slot->native, 0, sizeof(slot->native));
    res = f_open(&slot->native, native, mode);
    error = res == FR_DENIED ? ESY_FS_ERR_NO_SPACE : result(backend, res);
    if (error != ESY_OK) return error;
    /* Truncate separately so WRITE|TRUNCATE without CREATE still requires existence. */
    if (flags & ESY_FS_OPEN_TRUNCATE) error = result(backend, f_truncate(&slot->native));
    if (error == ESY_OK && (flags & ESY_FS_OPEN_APPEND))
        error = result(backend, f_lseek(&slot->native, f_size(&slot->native)));
    if (error != ESY_OK) {
        if (backend->fault == ESY_OK) {
            FRESULT close_result = f_close(&slot->native);
            if (close_result != FR_OK) backend->fault = error;
        }
        return error;
    }
    slot->flags = flags;
    slot->used = true;
    *object = slot;
    return ESY_OK;
}

static esy_err_t backend_close(void *context, void *object, bool *released)
{
    esy_fs_fatfs_t *backend = context;
    if (released == NULL) return ESY_ERR_INVALID_ARG;
    *released = false;
    if (!valid_backend(backend) || !backend->mounted) return ESY_ERR_STATE;
    esy_fs_fatfs_file_slot_t *slot = file_slot(backend, object);
    if (slot == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, false);
    if (error == ESY_OK) error = result(backend, f_close(&slot->native));
    if (error == ESY_OK) error = backend->block.sync(backend->block.context);
    if (error != ESY_OK) backend->fault = error;
    slot->used = false;
    *released = true;
    return error;
}

static esy_err_t backend_read(void *context, void *object, void *data,
                               size_t size, size_t *transferred)
{
    esy_fs_fatfs_t *backend = context;
    if (transferred == NULL || (data == NULL && size != 0u)) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, false);
    if (error != ESY_OK) return error;
    esy_fs_fatfs_file_slot_t *slot = file_slot(backend, object);
    if (slot == NULL || !(slot->flags & ESY_FS_OPEN_READ)) return ESY_ERR_INVALID_ARG;
    if (size > UINT_MAX) return ESY_ERR_INVALID_ARG;
    if (size == 0u) { *transferred = 0; return ESY_OK; }
    UINT count;
    error = result(backend, f_read(&slot->native, data, (UINT)size, &count));
    if (error == ESY_OK) *transferred = count;
    return error;
}

static esy_err_t backend_write(void *context, void *object, const void *data,
                                size_t size, size_t *transferred)
{
    esy_fs_fatfs_t *backend = context;
    if (transferred == NULL || (data == NULL && size != 0u)) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, true);
    if (error != ESY_OK) return error;
    esy_fs_fatfs_file_slot_t *slot = file_slot(backend, object);
    if (slot == NULL || !(slot->flags & ESY_FS_OPEN_WRITE)) return ESY_ERR_INVALID_ARG;
    if (size > UINT_MAX) return ESY_ERR_INVALID_ARG;
    if (slot->flags & ESY_FS_OPEN_APPEND) {
        error = result(backend, f_lseek(&slot->native, f_size(&slot->native)));
        if (error != ESY_OK) return error;
    }
    if (size == 0u) { *transferred = 0; return ESY_OK; }
    UINT count;
    error = result(backend, f_write(&slot->native, data, (UINT)size, &count));
    if (error == ESY_OK) *transferred = count;
    return error;
}

static esy_err_t backend_seek(void *context, void *object, int64_t offset,
                               esy_fs_whence_t whence, uint64_t *position)
{
    esy_fs_fatfs_t *backend = context;
    if (position == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, false);
    if (error != ESY_OK) return error;
    esy_fs_fatfs_file_slot_t *slot = file_slot(backend, object);
    if (slot == NULL) return ESY_ERR_INVALID_ARG;
    uint64_t base;
    if (whence == ESY_FS_SEEK_SET) base = 0;
    else if (whence == ESY_FS_SEEK_CUR) base = f_tell(&slot->native);
    else if (whence == ESY_FS_SEEK_END) base = f_size(&slot->native);
    else return ESY_ERR_INVALID_ARG;
    uint64_t target;
    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1u;
        if (magnitude > base) return ESY_ERR_INVALID_ARG;
        target = base - magnitude;
    } else {
        if ((uint64_t)offset > UINT64_MAX - base) return ESY_ERR_INVALID_ARG;
        target = base + (uint64_t)offset;
    }
    if (target > f_size(&slot->native)) return ESY_ERR_INVALID_ARG;
    error = result(backend, f_lseek(&slot->native, (FSIZE_t)target));
    if (error == ESY_OK) *position = f_tell(&slot->native);
    return error;
}

static esy_err_t backend_sync(void *context, void *object)
{
    esy_fs_fatfs_t *backend = context;
    esy_err_t error = ready(backend, false);
    if (error != ESY_OK) return error;
    esy_fs_fatfs_file_slot_t *slot = file_slot(backend, object);
    if (slot == NULL) return ESY_ERR_INVALID_ARG;
    error = result(backend, f_sync(&slot->native));
    if (error == ESY_OK) error = backend->block.sync(backend->block.context);
    if (error != ESY_OK) backend->fault = error;
    return error;
}

static esy_err_t backend_stat(void *context, const char *path, esy_fs_stat_t *stat)
{
    esy_fs_fatfs_t *backend = context;
    if (stat == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, false);
    if (error != ESY_OK) return error;
    char native[FATFS_PATH_SIZE];
    error = native_path(backend, path, native);
    if (error != ESY_OK) return error;
    if (path[1] == '\0') {
        *stat = (esy_fs_stat_t){ .type = ESY_FS_TYPE_DIR, .size = 0 };
        return ESY_OK;
    }
    FILINFO metadata;
    error = result(backend, f_stat(native, &metadata));
    if (error == ESY_OK) {
        esy_fs_stat_t snapshot = { .type = (metadata.fattrib & AM_DIR) ? ESY_FS_TYPE_DIR : ESY_FS_TYPE_FILE,
                                   .size = (metadata.fattrib & AM_DIR) ? 0u : metadata.fsize };
        *stat = snapshot;
    }
    return error;
}

static esy_err_t backend_remove(void *context, const char *path)
{
    esy_fs_fatfs_t *backend = context;
    esy_err_t error = ready(backend, true);
    if (error != ESY_OK) return error;
    char native[FATFS_PATH_SIZE];
    error = native_path(backend, path, native);
    if (error != ESY_OK) return error;
    if (path[1] == '\0') return ESY_ERR_BUSY;
    FILINFO metadata;
    error = result(backend, f_stat(native, &metadata));
    if (error != ESY_OK) return error;
    if (metadata.fattrib & AM_RDO) return ESY_FS_ERR_READ_ONLY;
    FRESULT res = f_unlink(native);
    if (res == FR_DENIED && (metadata.fattrib & AM_DIR)) return ESY_ERR_BUSY;
    return result(backend, res);
}

static esy_err_t backend_rename(void *context, const char *from, const char *to)
{
    esy_fs_fatfs_t *backend = context;
    esy_err_t error = ready(backend, true);
    if (error != ESY_OK) return error;
    char source[FATFS_PATH_SIZE];
    char destination[FATFS_PATH_SIZE];
    error = native_path(backend, from, source);
    if (error == ESY_OK) error = native_path(backend, to, destination);
    return error == ESY_OK ? result(backend, f_rename(source, destination)) : error;
}

static esy_err_t backend_mkdir(void *context, const char *path)
{
    esy_fs_fatfs_t *backend = context;
    esy_err_t error = ready(backend, true);
    if (error != ESY_OK) return error;
    char native[FATFS_PATH_SIZE];
    error = native_path(backend, path, native);
    if (error != ESY_OK) return error;
    if (path[1] == '\0') return ESY_FS_ERR_EXISTS;
    FRESULT res = f_mkdir(native);
    return res == FR_DENIED ? ESY_FS_ERR_NO_SPACE : result(backend, res);
}

static esy_err_t backend_dir_open(void *context, const char *path, void **object)
{
    esy_fs_fatfs_t *backend = context;
    if (object == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, false);
    if (error != ESY_OK) return error;
    char native[FATFS_PATH_SIZE];
    error = native_path(backend, path, native);
    if (error != ESY_OK) return error;
    if (path[1] != '\0') {
        FILINFO metadata;
        error = result(backend, f_stat(native, &metadata));
        if (error != ESY_OK) return error;
        if (!(metadata.fattrib & AM_DIR)) return ESY_FS_ERR_NOT_DIR;
    }
    esy_fs_fatfs_dir_slot_t *slot = NULL;
    for (size_t i = 0; i < backend->resources.dir_count; ++i) {
        if (!backend->resources.dirs[i].used) {
            slot = &backend->resources.dirs[i];
            break;
        }
    }
    if (slot == NULL) return ESY_ERR_NO_MEM;
    memset(&slot->native, 0, sizeof(slot->native));
    error = result(backend, f_opendir(&slot->native, native));
    if (error != ESY_OK) return error;
    slot->used = true;
    *object = slot;
    return ESY_OK;
}

static esy_err_t backend_dir_read(void *context, void *object, esy_fs_entry_t *entry, bool *end)
{
    esy_fs_fatfs_t *backend = context;
    if (entry == NULL || end == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, false);
    if (error != ESY_OK) return error;
    esy_fs_fatfs_dir_slot_t *slot = dir_slot(backend, object);
    if (slot == NULL) return ESY_ERR_INVALID_ARG;
    FILINFO metadata;
    do {
        error = result(backend, f_readdir(&slot->native, &metadata));
        if (error != ESY_OK) return error;
    } while (strcmp(metadata.fname, ".") == 0 || strcmp(metadata.fname, "..") == 0);
    if (metadata.fname[0] == '\0') { *end = true; return ESY_OK; }
    size_t length = strlen(metadata.fname);
    if (length > ESY_FS_NAME_MAX) return ESY_FS_ERR_NAME_TOO_LONG;
    esy_fs_entry_t snapshot = {0};
    memcpy(snapshot.name, metadata.fname, length + 1u);
    snapshot.stat.type = (metadata.fattrib & AM_DIR) ? ESY_FS_TYPE_DIR : ESY_FS_TYPE_FILE;
    snapshot.stat.size = (metadata.fattrib & AM_DIR) ? 0u : metadata.fsize;
    *entry = snapshot;
    *end = false;
    return ESY_OK;
}

static esy_err_t backend_dir_close(void *context, void *object, bool *released)
{
    esy_fs_fatfs_t *backend = context;
    if (released == NULL) return ESY_ERR_INVALID_ARG;
    *released = false;
    if (!valid_backend(backend) || !backend->mounted) return ESY_ERR_STATE;
    esy_fs_fatfs_dir_slot_t *slot = dir_slot(backend, object);
    if (slot == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t error = ready(backend, false);
    if (error == ESY_OK) error = result(backend, f_closedir(&slot->native));
    if (error != ESY_OK) backend->fault = error;
    slot->used = false;
    *released = true;
    return error;
}

static void resource_spans(const esy_fs_fatfs_t *backend, const esy_fs_fatfs_static_resources_t *storage,
                            fs_fatfs_span_t spans[FATFS_RESOURCE_SPANS])
{
    spans[0] = (fs_fatfs_span_t){ (uintptr_t)backend, sizeof(*backend) };
    spans[1] = (fs_fatfs_span_t){ (uintptr_t)storage->volume, sizeof(FATFS) };
    spans[2] = (fs_fatfs_span_t){ (uintptr_t)storage->files, storage->file_count * sizeof(*storage->files) };
    spans[3] = (fs_fatfs_span_t){ (uintptr_t)storage->dirs, storage->dir_count * sizeof(*storage->dirs) };
    spans[4] = (fs_fatfs_span_t){ (uintptr_t)storage->format_buffer, storage->format_buffer_size };
}

static bool overlaps(fs_fatfs_span_t a, fs_fatfs_span_t b)
{
    return a.size != 0u && b.size != 0u && a.start < b.start + b.size && b.start < a.start + a.size;
}

esy_err_t esy_fs_fatfs_init(esy_fs_fatfs_t *backend, const esy_fs_fatfs_config_t *config)
{
    if (backend == NULL || config == NULL || config->static_resources == NULL) return ESY_ERR_INVALID_ARG;
    if (backend->self != NULL) return ESY_ERR_STATE;
    const esy_fs_block_ops_t *block = &config->block;
    const esy_fs_fatfs_static_resources_t *storage = config->static_resources;
    if (block->status == NULL || block->read == NULL || block->write == NULL || block->sync == NULL ||
        block->sector_size != FATFS_SECTOR_SIZE || block->sector_count == 0u ||
        block->sector_count > UINT32_MAX || block->erase_block_sectors == 0u ||
        (block->erase_block_sectors & (block->erase_block_sectors - 1u)) != 0u ||
        block->erase_block_sectors > block->sector_count ||
        storage->volume == NULL || storage->format_buffer == NULL ||
        storage->format_buffer_size < block->sector_size || storage->format_buffer_size > UINT_MAX ||
        (storage->file_count != 0u && storage->files == NULL) ||
        (storage->dir_count != 0u && storage->dirs == NULL) ||
        storage->file_count > SIZE_MAX / sizeof(*storage->files) ||
        storage->dir_count > SIZE_MAX / sizeof(*storage->dirs) ||
        (uintptr_t)storage->volume % _Alignof(FATFS) != 0u ||
        (uintptr_t)storage->files % _Alignof(esy_fs_fatfs_file_slot_t) != 0u ||
        (uintptr_t)storage->dirs % _Alignof(esy_fs_fatfs_dir_slot_t) != 0u)
        return ESY_ERR_INVALID_ARG;
    fs_fatfs_span_t spans[FATFS_RESOURCE_SPANS];
    resource_spans(backend, storage, spans);
    for (size_t i = 0; i < FATFS_RESOURCE_SPANS; ++i) {
        if (spans[i].size > UINTPTR_MAX - spans[i].start) return ESY_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; ++j)
            if (overlaps(spans[i], spans[j])) return ESY_ERR_INVALID_ARG;
    }
    size_t drive = FF_VOLUMES;
    for (size_t i = 0; i < FF_VOLUMES; ++i) {
        if (s_ctx.drives[i] == NULL) { if (drive == FF_VOLUMES) drive = i; continue; }
        fs_fatfs_span_t occupied[FATFS_RESOURCE_SPANS];
        resource_spans(s_ctx.drives[i], &s_ctx.drives[i]->resources, occupied);
        for (size_t a = 0; a < FATFS_RESOURCE_SPANS; ++a)
            for (size_t b = 0; b < FATFS_RESOURCE_SPANS; ++b)
                if (overlaps(spans[a], occupied[b])) return ESY_ERR_BUSY;
    }
    if (drive == FF_VOLUMES) return ESY_ERR_NO_MEM;
    esy_fs_block_ops_t copied_block = *block;
    esy_fs_fatfs_static_resources_t copied = *storage;
    memset(copied.volume, 0, sizeof(FATFS));
    for (size_t i = 0; i < copied.file_count; ++i)
        memset(&copied.files[i], 0, sizeof(esy_fs_fatfs_file_slot_t));
    for (size_t i = 0; i < copied.dir_count; ++i)
        memset(&copied.dirs[i], 0, sizeof(esy_fs_fatfs_dir_slot_t));
    *backend = (esy_fs_fatfs_t){ .self = backend, .block = copied_block, .resources = copied,
                                  .drive = (uint8_t)drive };
    s_ctx.drives[drive] = backend;
    return ESY_OK;
}

esy_err_t esy_fs_fatfs_deinit(esy_fs_fatfs_t *backend)
{
    if (backend == NULL) return ESY_ERR_INVALID_ARG;
    if (backend->self == NULL) return ESY_OK;
    if (!valid_backend(backend)) return ESY_ERR_STATE;
    if (backend->mounted || has_objects(backend)) return ESY_ERR_BUSY;
    s_ctx.drives[backend->drive] = NULL;
    memset(backend, 0, sizeof(*backend));
    return ESY_OK;
}

esy_err_t esy_fs_fatfs_get_ops(esy_fs_fatfs_t *backend, esy_fs_ops_t *ops)
{
    if (backend == NULL || ops == NULL) return ESY_ERR_INVALID_ARG;
    if (!valid_backend(backend)) return ESY_ERR_STATE;
    *ops = (esy_fs_ops_t){
        .context = backend, .mount = backend_mount, .unmount = backend_unmount,
        .format = backend_format, .info = backend_info, .open = backend_open,
        .close = backend_close, .read = backend_read, .write = backend_write,
        .seek = backend_seek, .sync = backend_sync, .stat = backend_stat,
        .remove = backend_remove, .rename = backend_rename, .mkdir = backend_mkdir,
        .dir_open = backend_dir_open, .dir_read = backend_dir_read, .dir_close = backend_dir_close,
        .replace = NULL,
    };
    return ESY_OK;
}

/* FatFs diskio ABI belongs exclusively to this provider adapter. */
DSTATUS disk_status(BYTE drive)
{
    if (drive >= FF_VOLUMES || s_ctx.drives[drive] == NULL) return STA_NOINIT | STA_NODISK;
    esy_fs_fatfs_t *backend = s_ctx.drives[drive];
    esy_fs_block_status_t status = {0};
    esy_err_t error = backend->block.status(backend->block.context, &status);
    if (error != ESY_OK || !status.ready) {
        backend->media_error = error != ESY_OK ? error : ESY_FS_ERR_NO_MEDIA;
        return STA_NOINIT | STA_NODISK;
    }
    return status.read_only ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE drive)
{
    /* Hardware initialization remains with the caller's HAL owner. */
    return disk_status(drive);
}

static DRESULT disk_result(esy_fs_fatfs_t *backend, esy_err_t error)
{
    if (error == ESY_OK) return RES_OK;
    backend->media_error = error;
    if (error == ESY_FS_ERR_NO_MEDIA || error == ESY_ERR_STATE) return RES_NOTRDY;
    if (error == ESY_FS_ERR_READ_ONLY) return RES_WRPRT;
    if (error == ESY_ERR_INVALID_ARG) return RES_PARERR;
    return RES_ERROR;
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count)
{
    if (drive >= FF_VOLUMES || s_ctx.drives[drive] == NULL) return RES_NOTRDY;
    esy_fs_fatfs_t *backend = s_ctx.drives[drive];
    if (buffer == NULL || count == 0u || (uint64_t)sector >= backend->block.sector_count ||
        (uint64_t)count > backend->block.sector_count - sector) return RES_PARERR;
    esy_err_t error = medium_status(backend, false);
    if (error == ESY_OK) error = backend->block.read(backend->block.context, sector, count, buffer);
    return disk_result(backend, error);
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count)
{
    if (drive >= FF_VOLUMES || s_ctx.drives[drive] == NULL) return RES_NOTRDY;
    esy_fs_fatfs_t *backend = s_ctx.drives[drive];
    if (buffer == NULL || count == 0u || (uint64_t)sector >= backend->block.sector_count ||
        (uint64_t)count > backend->block.sector_count - sector) return RES_PARERR;
    esy_err_t error = medium_status(backend, true);
    if (error == ESY_OK) error = backend->block.write(backend->block.context, sector, count, buffer);
    return disk_result(backend, error);
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
    if (drive >= FF_VOLUMES || s_ctx.drives[drive] == NULL) return RES_NOTRDY;
    esy_fs_fatfs_t *backend = s_ctx.drives[drive];
    esy_err_t error = medium_status(backend, false);
    if (error != ESY_OK) return disk_result(backend, error);
    switch (command) {
    case CTRL_SYNC: return disk_result(backend, backend->block.sync(backend->block.context));
    case GET_SECTOR_COUNT:
        if (buffer == NULL) return RES_PARERR;
        *(LBA_t *)buffer = (LBA_t)backend->block.sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (buffer == NULL) return RES_PARERR;
        *(WORD *)buffer = (WORD)backend->block.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (buffer == NULL) return RES_PARERR;
        *(DWORD *)buffer = backend->block.erase_block_sectors;
        return RES_OK;
    default: return RES_PARERR;
    }
}
