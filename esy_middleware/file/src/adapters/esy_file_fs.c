/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/file/esy_file_fs.h"
#include <string.h>

static bool valid_binding_path(const char *path)
{
    if (path == NULL || path[0] != '/' || path[1] == '\0') return false;
    size_t start = 1u;
    for (size_t i = 1u; i <= ESY_FS_PATH_MAX; ++i) {
        unsigned char byte = (unsigned char)path[i];
        if (byte != '\0' && (byte < 0x20u || byte == 0x7fu || byte == '\\' || byte == ':'))
            return false;
        if (byte == '/' || byte == '\0') {
            size_t length = i - start;
            if (length == 0u || (length == 1u && path[start] == '.') ||
                (length == 2u && path[start] == '.' && path[start + 1u] == '.')) return false;
            if (byte == '\0') return true;
            start = i + 1u;
        }
    }
    return false;
}

static esy_err_t enter(esy_file_fs_t *adapter)
{
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->self != adapter) return ESY_ERR_STATE;
    if (adapter->busy) return ESY_ERR_BUSY;
    adapter->busy = true;
    esy_err_t result = esy_fs_close(&adapter->active_file);
    if (result != ESY_OK) adapter->busy = false;
    return result;
}

static esy_err_t leave(esy_file_fs_t *adapter, esy_err_t result)
{
    adapter->busy = false;
    return result;
}

static esy_err_t binding_info(void *context, esy_file_info_t *info)
{
    esy_file_fs_t *adapter = context;
    if (info == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(adapter);
    if (result != ESY_OK) return result;
    esy_fs_stat_t stat = {0};
    result = esy_fs_stat(adapter->config.volume, adapter->config.path, &stat);
    esy_file_info_t output = {0};
    output.capacity = adapter->config.capacity;
    if (result == ESY_FS_ERR_NOT_FOUND) result = ESY_OK;
    else if (result == ESY_OK) {
        if (stat.type != ESY_FS_TYPE_FILE) result = ESY_FS_ERR_IS_DIR;
        else if (stat.size > adapter->config.capacity) result = ESY_ERR_STATE;
        else {
            output.exists = true;
            output.length = (uint32_t)stat.size;
        }
    }
    if (result == ESY_OK) *info = output;
    return leave(adapter, result);
}

static esy_err_t binding_read(void *context, uint32_t offset, void *buffer, size_t length)
{
    esy_file_fs_t *adapter = context;
    if (length != 0u && buffer == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(adapter);
    if (result != ESY_OK) return result;
    if (offset > adapter->config.capacity || length > adapter->config.capacity - offset)
        return leave(adapter, ESY_ERR_INVALID_ARG);
    esy_fs_stat_t stat = {0};
    result = esy_fs_stat(adapter->config.volume, adapter->config.path, &stat);
    if (result == ESY_FS_ERR_NOT_FOUND) result = ESY_ERR_STATE;
    if (result != ESY_OK) return leave(adapter, result);
    if (stat.type != ESY_FS_TYPE_FILE) return leave(adapter, ESY_FS_ERR_IS_DIR);
    if (offset > stat.size || length > stat.size - offset)
        return leave(adapter, ESY_ERR_INVALID_ARG);
    result = esy_fs_open(adapter->config.volume, &adapter->active_file,
                         adapter->config.path, ESY_FS_OPEN_READ);
    if (result != ESY_OK) return leave(adapter, result);
    uint64_t position = 0u;
    result = esy_fs_seek(&adapter->active_file, offset, ESY_FS_SEEK_SET, &position);
    if (result == ESY_OK && position != offset) result = ESY_ERR_IO;
    size_t remaining = length;
    unsigned char *destination = buffer;
    while (result == ESY_OK && remaining != 0u) {
        size_t count = 0u;
        result = esy_fs_read(&adapter->active_file, destination, remaining, &count);
        if (result != ESY_OK) break;
        if (count == 0u) { result = ESY_ERR_IO; break; }
        destination += count;
        remaining -= count;
    }
    esy_err_t closed = esy_fs_close(&adapter->active_file);
    if (result == ESY_OK) result = closed;
    return leave(adapter, result);
}

static esy_err_t replace_content(esy_file_fs_t *adapter, const void *data, size_t length,
                                 bool create_only)
{
    if (length != 0u && data == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t result = enter(adapter);
    if (result != ESY_OK) return result;
    if (!adapter->atomic_replace) return leave(adapter, ESY_ERR_NOT_SUPPORTED);
    if (length > adapter->config.capacity) return leave(adapter, ESY_ERR_NO_MEM);
    esy_fs_stat_t stat = {0};
    result = esy_fs_stat(adapter->config.volume, adapter->config.path, &stat);
    if (result == ESY_OK) {
        if (stat.type != ESY_FS_TYPE_FILE) return leave(adapter, ESY_FS_ERR_IS_DIR);
        if (create_only) return leave(adapter, ESY_ERR_BUSY);
    } else if (result != ESY_FS_ERR_NOT_FOUND) return leave(adapter, result);
    result = esy_fs_replace(adapter->config.volume, adapter->config.path,
                            adapter->config.temporary_path, data, length);
    return leave(adapter, result);
}

static esy_err_t binding_create(void *context, const void *data, size_t length)
{
    return replace_content(context, data, length, true);
}

static esy_err_t binding_replace(void *context, const void *data, size_t length)
{
    return replace_content(context, data, length, false);
}

static esy_err_t binding_remove(void *context)
{
    esy_file_fs_t *adapter = context;
    esy_err_t result = enter(adapter);
    if (result != ESY_OK) return result;
    esy_fs_stat_t stat = {0};
    result = esy_fs_stat(adapter->config.volume, adapter->config.path, &stat);
    if (result == ESY_FS_ERR_NOT_FOUND) return leave(adapter, ESY_OK);
    if (result != ESY_OK) return leave(adapter, result);
    if (stat.type != ESY_FS_TYPE_FILE) return leave(adapter, ESY_FS_ERR_IS_DIR);
    result = esy_fs_remove(adapter->config.volume, adapter->config.path);
    return leave(adapter, result);
}

esy_err_t esy_file_fs_init(esy_file_fs_t *adapter, const esy_file_fs_config_t *config)
{
    if (adapter == NULL || config == NULL || !valid_binding_path(config->path) ||
        config->volume == NULL || (config->temporary_path != NULL &&
        (!valid_binding_path(config->temporary_path) || strcmp(config->path, config->temporary_path) == 0)))
        return ESY_ERR_INVALID_ARG;
    if (adapter->self != NULL) return ESY_ERR_STATE;
    bool supported = false;
    esy_err_t result = esy_fs_supports_atomic_replace(config->volume, &supported);
    if (result != ESY_OK) return result;
    esy_file_fs_config_t copied = *config;
    result = esy_fs_retain(config->volume);
    if (result != ESY_OK) return result;
    memset(adapter, 0, sizeof(*adapter));
    adapter->config = copied;
    adapter->atomic_replace = supported && copied.temporary_path != NULL;
    adapter->self = adapter;
    return ESY_OK;
}

esy_err_t esy_file_fs_deinit(esy_file_fs_t *adapter)
{
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->self == NULL) return ESY_OK;
    esy_err_t result = enter(adapter);
    if (result != ESY_OK) return result;
    result = esy_fs_release(adapter->config.volume);
    if (result != ESY_OK) return leave(adapter, result);
    memset(adapter, 0, sizeof(*adapter));
    return ESY_OK;
}

esy_err_t esy_file_fs_get_ops(esy_file_fs_t *adapter, esy_file_ops_t *ops)
{
    if (adapter == NULL || ops == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->self != adapter) return ESY_ERR_STATE;
    if (adapter->busy) return ESY_ERR_BUSY;
    esy_file_ops_t output = {0};
    output.info = binding_info;
    output.read = binding_read;
    output.remove = binding_remove;
    output.context = adapter;
    if (adapter->atomic_replace) {
        output.create = binding_create;
        output.replace = binding_replace;
    }
    *ops = output;
    return ESY_OK;
}
