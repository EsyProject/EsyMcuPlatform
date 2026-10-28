/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/fs/esy_fs_littlefs.h"
#include <limits.h>
#include <string.h>

#if !defined(LFS_NO_MALLOC) || defined(LFS_READONLY) || defined(LFS_THREADSAFE) || defined(LFS_MULTIVERSION)
#error "esy_fs_littlefs requires LFS_NO_MALLOC and the default writable, owner-serialized littlefs ABI"
#endif
#if LFS_VERSION_MAJOR != 2 || LFS_VERSION_MINOR < 11
#error "esy_fs_littlefs requires littlefs 2.11 or later in major version 2"
#endif

#define FS_LITTLEFS_MIN_BLOCK_SIZE 128u
#define FS_LITTLEFS_SPAN_COUNT 7u

typedef struct {
    const void *pointer;
    size_t size;
} littlefs_span_t;

static bool overlap(const void *a, size_t an, const void *b, size_t bn) {
    if (an == 0u || bn == 0u) return false;
    const uintptr_t ap = (uintptr_t)a;
    const uintptr_t bp = (uintptr_t)b;
    if (ap > UINTPTR_MAX - (an - 1u) || bp > UINTPTR_MAX - (bn - 1u)) return true;
    return ap <= bp + bn - 1u && bp <= ap + an - 1u;
}

static esy_err_t map_error(int error) {
    switch (error) {
    case 0: return ESY_OK;
    case LFS_ERR_IO: return ESY_ERR_IO;
    case LFS_ERR_CORRUPT: return ESY_FS_ERR_CORRUPT;
    case LFS_ERR_NOENT: return ESY_FS_ERR_NOT_FOUND;
    case LFS_ERR_EXIST: return ESY_FS_ERR_EXISTS;
    case LFS_ERR_NOTDIR: return ESY_FS_ERR_NOT_DIR;
    case LFS_ERR_ISDIR: return ESY_FS_ERR_IS_DIR;
    case LFS_ERR_NOTEMPTY: return ESY_ERR_BUSY;
    case LFS_ERR_BADF: return ESY_ERR_STATE;
    case LFS_ERR_FBIG: return ESY_FS_ERR_NO_SPACE;
    case LFS_ERR_INVAL: return ESY_ERR_INVALID_ARG;
    case LFS_ERR_NOSPC: return ESY_FS_ERR_NO_SPACE;
    case LFS_ERR_NOMEM: return ESY_ERR_NO_MEM;
    case LFS_ERR_NOATTR: return ESY_FS_ERR_NOT_FOUND;
    case LFS_ERR_NAMETOOLONG: return ESY_FS_ERR_NAME_TOO_LONG;
    default: return ESY_ERR_IO;
    }
}

static bool valid(const esy_fs_littlefs_t *backend) {
    return backend != NULL && backend->self == backend;
}

static bool ready(const esy_fs_littlefs_t *backend) {
    return valid(backend) && backend->mounted;
}

static void resource_spans(const esy_fs_littlefs_t *backend, littlefs_span_t *spans) {
    const esy_fs_littlefs_static_resources_t *r = &backend->resources;
    spans[0] = (littlefs_span_t){backend, sizeof(*backend)};
    spans[1] = (littlefs_span_t){r->files, r->file_count * sizeof(*r->files)};
    spans[2] = (littlefs_span_t){r->directories, r->directory_count * sizeof(*r->directories)};
    spans[3] = (littlefs_span_t){r->read_buffer, r->read_capacity};
    spans[4] = (littlefs_span_t){r->program_buffer, r->program_capacity};
    spans[5] = (littlefs_span_t){r->lookahead_buffer, r->lookahead_capacity};
    spans[6] = (littlefs_span_t){r->file_buffers, r->file_buffers_capacity};
}

static bool aliases(esy_fs_littlefs_t *backend, const void *pointer, size_t size) {
    littlefs_span_t spans[FS_LITTLEFS_SPAN_COUNT];
    resource_spans(backend, spans);
    for (size_t i = 0u; i < FS_LITTLEFS_SPAN_COUNT; ++i)
        if (overlap(pointer, size, spans[i].pointer, spans[i].size)) return true;
    return false;
}

static bool active_objects(const esy_fs_littlefs_t *backend) {
    for (size_t i = 0u; i < backend->resources.file_count; ++i)
        if (backend->resources.files[i].in_use) return true;
    for (size_t i = 0u; i < backend->resources.directory_count; ++i)
        if (backend->resources.directories[i].in_use) return true;
    return false;
}

static esy_fs_littlefs_file_t *file_slot(esy_fs_littlefs_t *backend, void *object) {
    if (!ready(backend) || object == NULL) return NULL;
    for (size_t i = 0u; i < backend->resources.file_count; ++i)
        if (object == &backend->resources.files[i] && backend->resources.files[i].in_use)
            return &backend->resources.files[i];
    return NULL;
}

static esy_fs_littlefs_dir_t *dir_slot(esy_fs_littlefs_t *backend, void *object) {
    if (!ready(backend) || object == NULL) return NULL;
    for (size_t i = 0u; i < backend->resources.directory_count; ++i)
        if (object == &backend->resources.directories[i] && backend->resources.directories[i].in_use)
            return &backend->resources.directories[i];
    return NULL;
}

static esy_err_t backend_mount(void *context) {
    esy_fs_littlefs_t *backend = context;
    if (!valid(backend)) return ESY_ERR_STATE;
    if (backend->mounted) return ESY_ERR_BUSY;
    const int result = lfs_mount(&backend->native, &backend->device);
    if (result == 0) backend->mounted = true;
    return map_error(result);
}

static esy_err_t backend_unmount(void *context) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    if (active_objects(backend)) return ESY_ERR_BUSY;
    const int result = lfs_unmount(&backend->native);
    if (result == 0) backend->mounted = false;
    return map_error(result);
}

static esy_err_t backend_format(void *context) {
    esy_fs_littlefs_t *backend = context;
    if (!valid(backend)) return ESY_ERR_STATE;
    if (backend->mounted || active_objects(backend)) return ESY_ERR_BUSY;
    return map_error(lfs_format(&backend->native, &backend->device));
}

static esy_err_t backend_info(void *context, esy_fs_info_t *info) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    if (info == NULL) return ESY_ERR_INVALID_ARG;
    struct lfs_fsinfo native;
    int result = lfs_fs_stat(&backend->native, &native);
    if (result < 0) return map_error(result);
    const lfs_ssize_t used = lfs_fs_size(&backend->native);
    if (used < 0) return map_error((int)used);
    if ((lfs_size_t)used > native.block_count) return ESY_FS_ERR_CORRUPT;
    *info = (esy_fs_info_t){.capacity_bytes = (uint64_t)native.block_size * native.block_count,
        .free_bytes = (uint64_t)native.block_size * (native.block_count - (lfs_size_t)used)};
    return ESY_OK;
}

static esy_err_t backend_open(void *context, const char *path, uint32_t flags, void **object) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    if (path == NULL || object == NULL) return ESY_ERR_INVALID_ARG;
    int native_flags = (flags & ESY_FS_OPEN_READ) != 0u ?
        ((flags & ESY_FS_OPEN_WRITE) != 0u ? LFS_O_RDWR : LFS_O_RDONLY) : LFS_O_WRONLY;
    if ((flags & ESY_FS_OPEN_CREATE) != 0u) native_flags |= LFS_O_CREAT;
    if ((flags & ESY_FS_OPEN_EXCLUSIVE) != 0u) native_flags |= LFS_O_EXCL;
    if ((flags & ESY_FS_OPEN_TRUNCATE) != 0u) native_flags |= LFS_O_TRUNC;
    if ((flags & ESY_FS_OPEN_APPEND) != 0u) native_flags |= LFS_O_APPEND;
    for (size_t i = 0u; i < backend->resources.file_count; ++i) {
        esy_fs_littlefs_file_t *slot = &backend->resources.files[i];
        if (slot->in_use) continue;
        memset(&slot->native, 0, sizeof(slot->native));
        const int result = lfs_file_opencfg(&backend->native, &slot->native, path,
                                          native_flags, &slot->config);
        if (result != 0) return map_error(result);
        slot->in_use = true;
        *object = slot;
        return ESY_OK;
    }
    return ESY_ERR_NO_MEM;
}

static esy_err_t backend_close(void *context, void *object, bool *released) {
    if (released == NULL) return ESY_ERR_INVALID_ARG;
    *released = false;
    esy_fs_littlefs_t *backend = context;
    esy_fs_littlefs_file_t *slot = file_slot(backend, object);
    if (slot == NULL) return ESY_ERR_STATE;
    const int result = lfs_file_close(&backend->native, &slot->native);
    slot->in_use = false;
    *released = true;
    return map_error(result);
}

static esy_err_t backend_read(void *context, void *object, void *data, size_t size,
                             size_t *transferred) {
    esy_fs_littlefs_t *backend = context;
    esy_fs_littlefs_file_t *slot = file_slot(backend, object);
    if (slot == NULL) return ESY_ERR_STATE;
    if ((data == NULL && size != 0u) || transferred == NULL) return ESY_ERR_INVALID_ARG;
    if (size == 0u) { *transferred = 0u; return ESY_OK; }
    const lfs_size_t amount = size > INT32_MAX ? INT32_MAX : (lfs_size_t)size;
    const lfs_ssize_t result = lfs_file_read(&backend->native, &slot->native, data, amount);
    if (result < 0) return map_error((int)result);
    *transferred = (size_t)result;
    return ESY_OK;
}

static esy_err_t backend_write(void *context, void *object, const void *data, size_t size,
                              size_t *transferred) {
    esy_fs_littlefs_t *backend = context;
    esy_fs_littlefs_file_t *slot = file_slot(backend, object);
    if (slot == NULL) return ESY_ERR_STATE;
    if ((data == NULL && size != 0u) || transferred == NULL) return ESY_ERR_INVALID_ARG;
    if (size == 0u) { *transferred = 0u; return ESY_OK; }
    const lfs_size_t amount = size > INT32_MAX ? INT32_MAX : (lfs_size_t)size;
    const lfs_ssize_t result = lfs_file_write(&backend->native, &slot->native, data, amount);
    if (result < 0) return map_error((int)result);
    *transferred = (size_t)result;
    return ESY_OK;
}

static esy_err_t backend_seek(void *context, void *object, int64_t offset,
                             esy_fs_whence_t whence, uint64_t *position) {
    esy_fs_littlefs_t *backend = context;
    esy_fs_littlefs_file_t *slot = file_slot(backend, object);
    if (slot == NULL) return ESY_ERR_STATE;
    if (position == NULL || offset < INT32_MIN || offset > INT32_MAX ||
        (whence != ESY_FS_SEEK_SET && whence != ESY_FS_SEEK_CUR && whence != ESY_FS_SEEK_END))
        return ESY_ERR_INVALID_ARG;
    const int native_whence = whence == ESY_FS_SEEK_SET ? LFS_SEEK_SET :
        (whence == ESY_FS_SEEK_CUR ? LFS_SEEK_CUR : LFS_SEEK_END);
    const lfs_soff_t result = lfs_file_seek(&backend->native, &slot->native,
                                         (lfs_soff_t)offset, native_whence);
    if (result < 0) return map_error((int)result);
    *position = (uint64_t)result;
    return ESY_OK;
}

static esy_err_t backend_sync(void *context, void *object) {
    esy_fs_littlefs_t *backend = context;
    esy_fs_littlefs_file_t *slot = file_slot(backend, object);
    return slot == NULL ? ESY_ERR_STATE : map_error(lfs_file_sync(&backend->native, &slot->native));
}

static esy_fs_stat_t convert_stat(const struct lfs_info *info) {
    return (esy_fs_stat_t){.type = info->type == LFS_TYPE_DIR ? ESY_FS_TYPE_DIR : ESY_FS_TYPE_FILE,
        .size = info->type == LFS_TYPE_DIR ? 0u : info->size};
}

static esy_err_t backend_stat(void *context, const char *path, esy_fs_stat_t *stat) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    if (path == NULL || stat == NULL) return ESY_ERR_INVALID_ARG;
    struct lfs_info info;
    const int result = lfs_stat(&backend->native, path, &info);
    if (result != 0) return map_error(result);
    *stat = convert_stat(&info);
    return ESY_OK;
}

static esy_err_t backend_remove(void *context, const char *path) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    return path == NULL ? ESY_ERR_INVALID_ARG : map_error(lfs_remove(&backend->native, path));
}

static esy_err_t backend_rename(void *context, const char *from, const char *to) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    return from == NULL || to == NULL ? ESY_ERR_INVALID_ARG :
        map_error(lfs_rename(&backend->native, from, to));
}

static esy_err_t backend_mkdir(void *context, const char *path) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    return path == NULL ? ESY_ERR_INVALID_ARG : map_error(lfs_mkdir(&backend->native, path));
}

static esy_err_t backend_dir_open(void *context, const char *path, void **object) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    if (path == NULL || object == NULL) return ESY_ERR_INVALID_ARG;
    for (size_t i = 0u; i < backend->resources.directory_count; ++i) {
        esy_fs_littlefs_dir_t *slot = &backend->resources.directories[i];
        if (slot->in_use) continue;
        memset(&slot->native, 0, sizeof(slot->native));
        const int result = lfs_dir_open(&backend->native, &slot->native, path);
        if (result != 0) return map_error(result);
        slot->in_use = true;
        *object = slot;
        return ESY_OK;
    }
    return ESY_ERR_NO_MEM;
}

static esy_err_t backend_dir_read(void *context, void *object, esy_fs_entry_t *entry, bool *end) {
    esy_fs_littlefs_t *backend = context;
    esy_fs_littlefs_dir_t *slot = dir_slot(backend, object);
    if (slot == NULL) return ESY_ERR_STATE;
    if (entry == NULL || end == NULL) return ESY_ERR_INVALID_ARG;
    struct lfs_info info;
    int result;
    do {
        result = lfs_dir_read(&backend->native, &slot->native, &info);
        if (result < 0) return map_error(result);
        if (result == 0) { *end = true; return ESY_OK; }
    } while (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0);
    const size_t length = strlen(info.name);
    if (length > ESY_FS_NAME_MAX) return ESY_FS_ERR_NAME_TOO_LONG;
    esy_fs_entry_t output = {.stat = convert_stat(&info)};
    memcpy(output.name, info.name, length + 1u);
    *entry = output;
    *end = false;
    return ESY_OK;
}

static esy_err_t backend_dir_close(void *context, void *object, bool *released) {
    if (released == NULL) return ESY_ERR_INVALID_ARG;
    *released = false;
    esy_fs_littlefs_t *backend = context;
    esy_fs_littlefs_dir_t *slot = dir_slot(backend, object);
    if (slot == NULL) return ESY_ERR_STATE;
    const int result = lfs_dir_close(&backend->native, &slot->native);
    slot->in_use = false;
    *released = true;
    return map_error(result);
}

static esy_err_t backend_replace(void *context, const char *path, const char *temporary_path,
                                const void *data, size_t size) {
    esy_fs_littlefs_t *backend = context;
    if (!ready(backend)) return ESY_ERR_STATE;
    if (path == NULL || temporary_path == NULL || strcmp(path, temporary_path) == 0 ||
        (data == NULL && size != 0u)) return ESY_ERR_INVALID_ARG;
    if (size > backend->native.file_max) return ESY_FS_ERR_NO_SPACE;
    void *object;
    esy_err_t result = backend_open(context, temporary_path,
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE | ESY_FS_OPEN_TRUNCATE, &object);
    if (result != ESY_OK) return result;
    size_t transferred = 0u;
    result = backend_write(context, object, data, size, &transferred);
    if (result == ESY_OK && transferred != size) result = ESY_FS_ERR_NO_SPACE;
    if (result == ESY_OK) result = backend_sync(context, object);
    bool released;
    const esy_err_t closed = backend_close(context, object, &released);
    if (result == ESY_OK) result = closed;
    if (result == ESY_OK) result = backend_rename(context, temporary_path, path);
    return result;
}

static bool valid_device(const struct lfs_config *d) {
    if (d->read == NULL || d->prog == NULL || d->erase == NULL || d->sync == NULL ||
        d->read_size == 0u || d->prog_size == 0u || d->cache_size == 0u ||
        d->block_size < FS_LITTLEFS_MIN_BLOCK_SIZE || d->block_count < 2u ||
        d->block_cycles == 0 || d->block_cycles < -1 || d->block_cycles == INT32_MAX ||
        d->lookahead_size == 0u ||
        d->lookahead_size > UINT32_MAX / 8u ||
        d->cache_size % d->read_size != 0u || d->cache_size % d->prog_size != 0u ||
        d->block_size % d->cache_size != 0u || d->name_max > LFS_NAME_MAX ||
        d->file_max > LFS_FILE_MAX || d->attr_max > LFS_ATTR_MAX ||
        d->read_buffer != NULL || d->prog_buffer != NULL || d->lookahead_buffer != NULL)
        return false;
    if (d->compact_thresh != 0u && d->compact_thresh != (lfs_size_t)-1 &&
        (d->compact_thresh < d->block_size / 2u || d->compact_thresh > d->block_size)) return false;
    if (d->metadata_max != 0u && (d->metadata_max < FS_LITTLEFS_MIN_BLOCK_SIZE ||
        d->metadata_max > d->block_size || d->metadata_max % d->read_size != 0u ||
        d->metadata_max % d->prog_size != 0u || d->block_size % d->metadata_max != 0u)) return false;
    const lfs_size_t attrs = d->attr_max != 0u ? d->attr_max : LFS_ATTR_MAX;
    const lfs_size_t metadata = d->metadata_max != 0u ? d->metadata_max : d->block_size;
    return d->inline_max == (lfs_size_t)-1 ||
        (d->inline_max <= d->cache_size && d->inline_max <= attrs && d->inline_max <= metadata / 8u);
}

esy_err_t esy_fs_littlefs_init(esy_fs_littlefs_t *backend, const esy_fs_littlefs_config_t *config) {
    if (backend == NULL || config == NULL || config->device == NULL || config->static_resources == NULL)
        return ESY_ERR_INVALID_ARG;
    if (backend->self != NULL) return ESY_ERR_STATE;
    const struct lfs_config device = *config->device;
    const esy_fs_littlefs_static_resources_t r = *config->static_resources;
    if (!valid_device(&device) || r.file_count == 0u || r.files == NULL ||
        (r.directory_count != 0u && r.directories == NULL) || r.read_buffer == NULL ||
        r.program_buffer == NULL || r.lookahead_buffer == NULL || r.file_buffers == NULL ||
        (uintptr_t)r.files % _Alignof(esy_fs_littlefs_file_t) != 0u ||
        (r.directory_count != 0u && (uintptr_t)r.directories % _Alignof(esy_fs_littlefs_dir_t) != 0u) ||
        r.file_count > SIZE_MAX / sizeof(*r.files) ||
        r.directory_count > SIZE_MAX / sizeof(*r.directories) ||
        r.file_count > SIZE_MAX / device.cache_size) return ESY_ERR_INVALID_ARG;
    if (r.read_capacity < device.cache_size || r.program_capacity < device.cache_size ||
        r.lookahead_capacity < device.lookahead_size ||
        r.file_buffers_capacity < r.file_count * device.cache_size) return ESY_ERR_NO_MEM;
    const littlefs_span_t spans[FS_LITTLEFS_SPAN_COUNT] = {
        {backend, sizeof(*backend)}, {r.files, r.file_count * sizeof(*r.files)},
        {r.directories, r.directory_count * sizeof(*r.directories)},
        {r.read_buffer, r.read_capacity}, {r.program_buffer, r.program_capacity},
        {r.lookahead_buffer, r.lookahead_capacity}, {r.file_buffers, r.file_buffers_capacity}};
    for (size_t i = 0u; i < FS_LITTLEFS_SPAN_COUNT; ++i) {
        if (spans[i].size != 0u &&
            (uintptr_t)spans[i].pointer > UINTPTR_MAX - (spans[i].size - 1u)) return ESY_ERR_INVALID_ARG;
        for (size_t j = 0u; j < i; ++j)
            if (overlap(spans[i].pointer, spans[i].size, spans[j].pointer, spans[j].size))
                return ESY_ERR_INVALID_ARG;
    }
    *backend = (esy_fs_littlefs_t){.self = backend, .device = device, .resources = r};
    backend->device.read_buffer = r.read_buffer;
    backend->device.prog_buffer = r.program_buffer;
    backend->device.lookahead_buffer = r.lookahead_buffer;
    for (size_t i = 0u; i < r.file_count; ++i) {
        r.files[i] = (esy_fs_littlefs_file_t){.config = {
            .buffer = (uint8_t *)r.file_buffers + i * device.cache_size}};
    }
    for (size_t i = 0u; i < r.directory_count; ++i) r.directories[i] = (esy_fs_littlefs_dir_t){0};
    return ESY_OK;
}

esy_err_t esy_fs_littlefs_get_ops(esy_fs_littlefs_t *backend, esy_fs_ops_t *ops) {
    if (!valid(backend)) return backend == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_STATE;
    if (ops == NULL || aliases(backend, ops, sizeof(*ops))) return ESY_ERR_INVALID_ARG;
    *ops = (esy_fs_ops_t){.context = backend, .mount = backend_mount, .unmount = backend_unmount,
        .format = backend_format, .info = backend_info, .open = backend_open, .close = backend_close,
        .read = backend_read, .write = backend_write, .seek = backend_seek, .sync = backend_sync,
        .stat = backend_stat, .remove = backend_remove, .rename = backend_rename, .mkdir = backend_mkdir,
        .dir_open = backend_dir_open, .dir_read = backend_dir_read, .dir_close = backend_dir_close,
        .replace = backend_replace};
    return ESY_OK;
}

esy_err_t esy_fs_littlefs_deinit(esy_fs_littlefs_t *backend) {
    if (backend == NULL) return ESY_ERR_INVALID_ARG;
    if (backend->self == NULL) return ESY_OK;
    if (!valid(backend)) return ESY_ERR_STATE;
    if (backend->mounted || active_objects(backend)) return ESY_ERR_BUSY;
    memset(backend, 0, sizeof(*backend));
    return ESY_OK;
}
