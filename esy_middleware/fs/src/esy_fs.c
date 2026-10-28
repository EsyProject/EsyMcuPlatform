/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/fs/esy_fs.h"
#include <string.h>

static esy_err_t enter(esy_fs_t *fs, bool require_mounted)
{
    if (fs == NULL) return ESY_ERR_INVALID_ARG;
    if (fs->self != fs) return ESY_ERR_STATE;
    if (fs->busy) return ESY_ERR_BUSY;
    if (require_mounted && !fs->mounted) return ESY_ERR_STATE;
    fs->busy = true;
    return ESY_OK;
}

static esy_err_t leave(esy_fs_t *fs, esy_err_t result)
{
    fs->busy = false;
    return result > ESY_OK ? ESY_ERR_IO : result;
}

static bool valid_path(const char *path)
{
    if (path == NULL || path[0] != '/') return false;
    size_t length = 0u;
    while (length <= ESY_FS_PATH_MAX && path[length] != '\0') {
        unsigned char byte = (unsigned char)path[length];
        if (byte < 0x20u || byte == 0x7fu || byte == '\\' || byte == ':') return false;
        ++length;
    }
    if (length > ESY_FS_PATH_MAX) return false;
    if (length == 1u) return true;
    if (path[length - 1u] == '/') return false;
    size_t start = 1u;
    for (size_t i = 1u; i <= length; ++i) {
        if (path[i] != '/' && path[i] != '\0') continue;
        size_t component = i - start;
        if (component == 0u || (component == 1u && path[start] == '.') ||
            (component == 2u && path[start] == '.' && path[start + 1u] == '.')) return false;
        start = i + 1u;
    }
    return true;
}

static bool valid_ops(const esy_fs_ops_t *ops)
{
    return ops != NULL && ops->mount != NULL && ops->unmount != NULL &&
        ops->format != NULL && ops->info != NULL && ops->open != NULL &&
        ops->close != NULL && ops->read != NULL && ops->write != NULL &&
        ops->seek != NULL && ops->sync != NULL && ops->stat != NULL &&
        ops->remove != NULL && ops->rename != NULL && ops->mkdir != NULL &&
        ops->dir_open != NULL && ops->dir_read != NULL && ops->dir_close != NULL;
}

static bool valid_entry_name(const char *name)
{
    if (name[0] == '\0') return false;
    for (size_t i = 0u; i <= ESY_FS_NAME_MAX; ++i) {
        unsigned char byte = (unsigned char)name[i];
        if (byte == '\0') return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
        if (byte < 0x20u || byte == 0x7fu || byte == '/' || byte == '\\' || byte == ':') return false;
    }
    return false;
}

static esy_err_t enter_file(esy_fs_file_t *file)
{
    if (file == NULL) return ESY_ERR_INVALID_ARG;
    if (file->self != file || file->object == NULL) return ESY_ERR_STATE;
    return enter(file->volume, true);
}

static esy_err_t enter_dir(esy_fs_dir_t *dir)
{
    if (dir == NULL) return ESY_ERR_INVALID_ARG;
    if (dir->self != dir || dir->object == NULL) return ESY_ERR_STATE;
    return enter(dir->volume, true);
}

esy_err_t esy_fs_init(esy_fs_t *fs, const esy_fs_ops_t *ops)
{
    if (fs == NULL || !valid_ops(ops)) return ESY_ERR_INVALID_ARG;
    if (fs->self != NULL) return fs->self == fs && fs->busy ? ESY_ERR_BUSY : ESY_ERR_STATE;
    esy_fs_ops_t copied = *ops;
    memset(fs, 0, sizeof(*fs));
    fs->ops = copied;
    fs->self = fs;
    return ESY_OK;
}

esy_err_t esy_fs_deinit(esy_fs_t *fs)
{
    if (fs == NULL) return ESY_ERR_INVALID_ARG;
    if (fs->self == NULL) return ESY_OK;
    esy_err_t result = enter(fs, false);
    if (result != ESY_OK) return result;
    if (fs->mounted || fs->open_files != 0u || fs->open_dirs != 0u || fs->leases != 0u)
        return leave(fs, ESY_ERR_BUSY);
    memset(fs, 0, sizeof(*fs));
    return ESY_OK;
}

esy_err_t esy_fs_mount(esy_fs_t *fs)
{
    esy_err_t result = enter(fs, false);
    if (result != ESY_OK) return result;
    if (fs->mounted) return leave(fs, ESY_ERR_STATE);
    result = fs->ops.mount(fs->ops.context);
    if (result == ESY_OK) fs->mounted = true;
    return leave(fs, result);
}

esy_err_t esy_fs_unmount(esy_fs_t *fs)
{
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    if (fs->open_files != 0u || fs->open_dirs != 0u || fs->leases != 0u)
        return leave(fs, ESY_ERR_BUSY);
    result = fs->ops.unmount(fs->ops.context);
    if (result == ESY_OK) fs->mounted = false;
    return leave(fs, result);
}

esy_err_t esy_fs_format(esy_fs_t *fs)
{
    esy_err_t result = enter(fs, false);
    if (result != ESY_OK) return result;
    if (fs->mounted || fs->leases != 0u || fs->open_files != 0u || fs->open_dirs != 0u)
        return leave(fs, ESY_ERR_BUSY);
    return leave(fs, fs->ops.format(fs->ops.context));
}

esy_err_t esy_fs_get_info(esy_fs_t *fs, esy_fs_info_t *info)
{
    if (info == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    esy_fs_info_t output = {0};
    result = fs->ops.info(fs->ops.context, &output);
    if (result == ESY_OK && output.free_bytes > output.capacity_bytes) result = ESY_ERR_IO;
    if (result == ESY_OK) *info = output;
    return leave(fs, result);
}

esy_err_t esy_fs_retain(esy_fs_t *fs)
{
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    if (fs->leases == SIZE_MAX) return leave(fs, ESY_ERR_NO_MEM);
    ++fs->leases;
    return leave(fs, ESY_OK);
}

esy_err_t esy_fs_release(esy_fs_t *fs)
{
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    if (fs->leases == 0u) return leave(fs, ESY_ERR_STATE);
    --fs->leases;
    return leave(fs, ESY_OK);
}

esy_err_t esy_fs_supports_atomic_replace(esy_fs_t *fs, bool *supported)
{
    if (supported == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(fs, false);
    if (result != ESY_OK) return result;
    *supported = fs->ops.replace != NULL;
    return leave(fs, ESY_OK);
}

esy_err_t esy_fs_open(esy_fs_t *fs, esy_fs_file_t *file, const char *path, uint32_t flags)
{
    const uint32_t known = ESY_FS_OPEN_READ | ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE |
        ESY_FS_OPEN_EXCLUSIVE | ESY_FS_OPEN_TRUNCATE | ESY_FS_OPEN_APPEND;
    if (file == NULL || !valid_path(path) || (flags & ~known) != 0u ||
        (flags & (ESY_FS_OPEN_READ | ESY_FS_OPEN_WRITE)) == 0u ||
        ((flags & (ESY_FS_OPEN_CREATE | ESY_FS_OPEN_TRUNCATE | ESY_FS_OPEN_APPEND)) != 0u &&
         (flags & ESY_FS_OPEN_WRITE) == 0u) ||
        ((flags & ESY_FS_OPEN_EXCLUSIVE) != 0u && (flags & ESY_FS_OPEN_CREATE) == 0u))
        return ESY_ERR_INVALID_ARG;
    if (file->self != NULL || file->volume != NULL || file->object != NULL) return ESY_ERR_STATE;
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    if (fs->open_files == SIZE_MAX) return leave(fs, ESY_ERR_NO_MEM);
    void *object = NULL;
    result = fs->ops.open(fs->ops.context, path, flags, &object);
    if (result == ESY_OK && object == NULL) result = ESY_ERR_IO;
    if (result == ESY_OK) {
        file->volume = fs;
        file->object = object;
        file->self = file;
        file->flags = flags;
        ++fs->open_files;
    }
    return leave(fs, result);
}

esy_err_t esy_fs_close(esy_fs_file_t *file)
{
    if (file == NULL) return ESY_ERR_INVALID_ARG;
    if (file->self == NULL && file->volume == NULL && file->object == NULL) return ESY_OK;
    esy_err_t result = enter_file(file);
    if (result != ESY_OK) return result;
    esy_fs_t *fs = file->volume;
    bool released = false;
    result = fs->ops.close(fs->ops.context, file->object, &released);
    if (released) {
        --fs->open_files;
        memset(file, 0, sizeof(*file));
    } else if (result == ESY_OK) result = ESY_ERR_IO;
    return leave(fs, result);
}

esy_err_t esy_fs_read(esy_fs_file_t *file, void *data, size_t size, size_t *transferred)
{
    if (transferred == NULL || (size != 0u && data == NULL)) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter_file(file);
    if (result != ESY_OK) return result;
    esy_fs_t *fs = file->volume;
    if ((file->flags & ESY_FS_OPEN_READ) == 0u) return leave(fs, ESY_ERR_STATE);
    size_t count = 0u;
    result = size == 0u ? ESY_OK : fs->ops.read(fs->ops.context, file->object, data, size, &count);
    if (result == ESY_OK && count > size) result = ESY_ERR_IO;
    if (result == ESY_OK) *transferred = count;
    return leave(fs, result);
}

esy_err_t esy_fs_write(esy_fs_file_t *file, const void *data, size_t size, size_t *transferred)
{
    if (transferred == NULL || (size != 0u && data == NULL)) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter_file(file);
    if (result != ESY_OK) return result;
    esy_fs_t *fs = file->volume;
    if ((file->flags & ESY_FS_OPEN_WRITE) == 0u) return leave(fs, ESY_FS_ERR_READ_ONLY);
    size_t count = 0u;
    result = size == 0u ? ESY_OK : fs->ops.write(fs->ops.context, file->object, data, size, &count);
    if (result == ESY_OK && count > size) result = ESY_ERR_IO;
    if (result == ESY_OK) *transferred = count;
    return leave(fs, result);
}

esy_err_t esy_fs_seek(esy_fs_file_t *file, int64_t offset, esy_fs_whence_t whence, uint64_t *position)
{
    if (position == NULL || (whence != ESY_FS_SEEK_SET && whence != ESY_FS_SEEK_CUR &&
                             whence != ESY_FS_SEEK_END)) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter_file(file);
    if (result != ESY_OK) return result;
    esy_fs_t *fs = file->volume;
    uint64_t output = 0u;
    result = fs->ops.seek(fs->ops.context, file->object, offset, whence, &output);
    if (result == ESY_OK) *position = output;
    return leave(fs, result);
}

esy_err_t esy_fs_sync(esy_fs_file_t *file)
{
    esy_err_t result = enter_file(file);
    if (result != ESY_OK) return result;
    esy_fs_t *fs = file->volume;
    return leave(fs, fs->ops.sync(fs->ops.context, file->object));
}

esy_err_t esy_fs_stat(esy_fs_t *fs, const char *path, esy_fs_stat_t *stat)
{
    if (stat == NULL || !valid_path(path)) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    esy_fs_stat_t output = {0};
    result = fs->ops.stat(fs->ops.context, path, &output);
    if (result == ESY_OK && output.type != ESY_FS_TYPE_FILE && output.type != ESY_FS_TYPE_DIR)
        result = ESY_ERR_IO;
    if (result == ESY_OK) *stat = output;
    return leave(fs, result);
}

esy_err_t esy_fs_remove(esy_fs_t *fs, const char *path)
{
    if (!valid_path(path)) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    return leave(fs, fs->ops.remove(fs->ops.context, path));
}

esy_err_t esy_fs_rename(esy_fs_t *fs, const char *from, const char *to)
{
    if (!valid_path(from) || !valid_path(to)) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    return leave(fs, fs->ops.rename(fs->ops.context, from, to));
}

esy_err_t esy_fs_mkdir(esy_fs_t *fs, const char *path)
{
    if (!valid_path(path)) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    return leave(fs, fs->ops.mkdir(fs->ops.context, path));
}

esy_err_t esy_fs_dir_open(esy_fs_t *fs, esy_fs_dir_t *dir, const char *path)
{
    if (dir == NULL || !valid_path(path)) return ESY_ERR_INVALID_ARG;
    if (dir->self != NULL || dir->volume != NULL || dir->object != NULL) return ESY_ERR_STATE;
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    if (fs->open_dirs == SIZE_MAX) return leave(fs, ESY_ERR_NO_MEM);
    void *object = NULL;
    result = fs->ops.dir_open(fs->ops.context, path, &object);
    if (result == ESY_OK && object == NULL) result = ESY_ERR_IO;
    if (result == ESY_OK) {
        dir->volume = fs;
        dir->object = object;
        dir->self = dir;
        ++fs->open_dirs;
    }
    return leave(fs, result);
}

esy_err_t esy_fs_dir_read(esy_fs_dir_t *dir, esy_fs_entry_t *entry, bool *end)
{
    if (entry == NULL || end == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter_dir(dir);
    if (result != ESY_OK) return result;
    esy_fs_t *fs = dir->volume;
    esy_fs_entry_t output = {0};
    bool at_end = false;
    result = fs->ops.dir_read(fs->ops.context, dir->object, &output, &at_end);
    if (result == ESY_OK && !at_end &&
        (!valid_entry_name(output.name) ||
         (output.stat.type != ESY_FS_TYPE_FILE && output.stat.type != ESY_FS_TYPE_DIR)))
        result = ESY_ERR_IO;
    if (result == ESY_OK) {
        *end = at_end;
        if (!at_end) *entry = output;
    }
    return leave(fs, result);
}

esy_err_t esy_fs_dir_close(esy_fs_dir_t *dir)
{
    if (dir == NULL) return ESY_ERR_INVALID_ARG;
    if (dir->self == NULL && dir->volume == NULL && dir->object == NULL) return ESY_OK;
    esy_err_t result = enter_dir(dir);
    if (result != ESY_OK) return result;
    esy_fs_t *fs = dir->volume;
    bool released = false;
    result = fs->ops.dir_close(fs->ops.context, dir->object, &released);
    if (released) {
        --fs->open_dirs;
        memset(dir, 0, sizeof(*dir));
    } else if (result == ESY_OK) result = ESY_ERR_IO;
    return leave(fs, result);
}

esy_err_t esy_fs_replace(esy_fs_t *fs, const char *path, const char *temporary_path,
                         const void *data, size_t size)
{
    if (!valid_path(path) || !valid_path(temporary_path) || strcmp(path, temporary_path) == 0 ||
        (size != 0u && data == NULL)) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(fs, true);
    if (result != ESY_OK) return result;
    if (fs->ops.replace == NULL) return leave(fs, ESY_ERR_NOT_SUPPORTED);
    return leave(fs, fs->ops.replace(fs->ops.context, path, temporary_path, data, size));
}
