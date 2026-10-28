/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/file/esy_file.h"

static bool overlap(const void *a, size_t an, const void *b, size_t bn) {
    const uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
    return an > UINTPTR_MAX - aa || bn > UINTPTR_MAX - bb ||
        (an != 0u && bn != 0u && aa < bb + bn && bb < aa + an);
}
static esy_err_t check_file(const esy_file_t *file) {
    if (file == NULL) return ESY_ERR_INVALID_ARG;
    if (file->busy) return ESY_ERR_BUSY;
    return file->initialized ? ESY_OK : ESY_ERR_STATE;
}
static uint32_t capabilities(const esy_file_ops_t *ops) {
    uint32_t value = ESY_FILE_CAP_READ;
    if (ops->create != NULL) value |= ESY_FILE_CAP_CREATE;
    if (ops->replace != NULL) value |= ESY_FILE_CAP_ATOMIC_REPLACE;
    if (ops->remove != NULL) value |= ESY_FILE_CAP_REMOVE;
    return value;
}
static esy_err_t read_info(esy_file_t *file, esy_file_info_t *info) {
    esy_file_info_t snapshot = {0};
    esy_err_t result = file->ops.info(file->ops.context, &snapshot);
    if (result != ESY_OK) return result > ESY_OK ? ESY_ERR_STATE : result;
    if (snapshot.length > snapshot.capacity || (!snapshot.exists && snapshot.length != 0u))
        return ESY_ERR_STATE;
    snapshot.capabilities = capabilities(&file->ops);
    *info = snapshot;
    return ESY_OK;
}
esy_err_t esy_file_init(esy_file_t *file, const esy_file_ops_t *ops) {
    if (file == NULL) return ESY_ERR_INVALID_ARG;
    if (file->busy) return ESY_ERR_BUSY;
    if (file->initialized) return ESY_ERR_STATE;
    if (ops == NULL || ops->info == NULL || ops->read == NULL) return ESY_ERR_INVALID_ARG;
    file->ops = *ops;
    file->initialized = true;
    return ESY_OK;
}
esy_err_t esy_file_deinit(esy_file_t *file) {
    if (file == NULL) return ESY_ERR_INVALID_ARG;
    if (file->busy) return ESY_ERR_BUSY;
    *file = (esy_file_t){0};
    return ESY_OK;
}
esy_err_t esy_file_get_capabilities(esy_file_t *file, uint32_t *output) {
    esy_err_t result = check_file(file);
    if (result != ESY_OK) return result;
    if (output == NULL || overlap(output, sizeof(*output), file, sizeof(*file)))
        return ESY_ERR_INVALID_ARG;
    *output = capabilities(&file->ops);
    return ESY_OK;
}
esy_err_t esy_file_get_info(esy_file_t *file, esy_file_info_t *info) {
    esy_err_t result = check_file(file);
    if (result != ESY_OK) return result;
    if (info == NULL || overlap(info, sizeof(*info), file, sizeof(*file)))
        return ESY_ERR_INVALID_ARG;
    file->busy = true;
    result = read_info(file, info);
    file->busy = false;
    return result;
}
esy_err_t esy_file_read(esy_file_t *file, uint32_t offset, void *buffer, size_t length) {
    esy_err_t result = check_file(file);
    if (result != ESY_OK) return result;
    if ((buffer == NULL && length != 0u) || length > UINT32_MAX - offset ||
        overlap(buffer, length, file, sizeof(*file))) return ESY_ERR_INVALID_ARG;
    file->busy = true;
    esy_file_info_t info;
    result = read_info(file, &info);
    if (result == ESY_OK) {
        if (!info.exists) result = ESY_ERR_STATE;
        else if (offset > info.length || length > info.length - offset) result = ESY_ERR_INVALID_ARG;
        else result = file->ops.read(file->ops.context, offset, buffer, length);
    }
    file->busy = false;
    return result > ESY_OK ? ESY_ERR_STATE : result;
}
static esy_err_t write_file(esy_file_t *file, const void *data, size_t length, bool create) {
    esy_err_t result = check_file(file);
    if (result != ESY_OK) return result;
    if ((data == NULL && length != 0u) || length > UINT32_MAX ||
        overlap(data, length, file, sizeof(*file))) return ESY_ERR_INVALID_ARG;
    esy_err_t (*operation)(void *, const void *, size_t) = create ? file->ops.create : file->ops.replace;
    if (operation == NULL) return ESY_ERR_NOT_SUPPORTED;
    file->busy = true;
    esy_file_info_t info;
    result = read_info(file, &info);
    if (result == ESY_OK) {
        if (create && info.exists) result = ESY_ERR_BUSY;
        else if (length > info.capacity) result = ESY_ERR_NO_MEM;
        else result = operation(file->ops.context, data, length);
    }
    file->busy = false;
    return result > ESY_OK ? ESY_ERR_STATE : result;
}
esy_err_t esy_file_create(esy_file_t *file, const void *data, size_t length) {
    return write_file(file, data, length, true);
}
esy_err_t esy_file_replace(esy_file_t *file, const void *data, size_t length) {
    return write_file(file, data, length, false);
}
esy_err_t esy_file_remove(esy_file_t *file) {
    esy_err_t result = check_file(file);
    if (result != ESY_OK) return result;
    if (file->ops.remove == NULL) return ESY_ERR_NOT_SUPPORTED;
    file->busy = true;
    esy_file_info_t info;
    result = read_info(file, &info);
    if (result == ESY_OK) result = file->ops.remove(file->ops.context);
    file->busy = false;
    return result > ESY_OK ? ESY_ERR_STATE : result;
}
