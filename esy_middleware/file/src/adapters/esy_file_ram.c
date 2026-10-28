/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/file/esy_file_ram.h"
#include <string.h>

static bool overlap(const void *a, size_t an, const void *b, size_t bn) {
    uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
    return an > UINTPTR_MAX - aa || bn > UINTPTR_MAX - bb ||
        (an != 0u && bn != 0u && aa < bb + bn && bb < aa + an);
}
static esy_err_t valid(const esy_file_ram_t *ram) {
    if (ram == NULL) return ESY_ERR_INVALID_ARG;
    return ram->initialized ? ESY_OK : ESY_ERR_STATE;
}
static bool aliases(const esy_file_ram_t *ram, const void *data, size_t size) {
    return overlap(data, size, ram, sizeof(*ram)) ||
        overlap(data, size, ram->resources.data_buffer, ram->resources.capacity);
}
static esy_err_t ram_info(void *context, esy_file_info_t *info) {
    esy_file_ram_t *ram = context;
    esy_err_t result = valid(ram);
    if (result != ESY_OK) return result;
    if (info == NULL || aliases(ram, info, sizeof(*info))) return ESY_ERR_INVALID_ARG;
    *info = (esy_file_info_t){.exists = ram->exists, .length = ram->length,
        .capacity = (uint32_t)ram->resources.capacity};
    return ESY_OK;
}
static esy_err_t ram_read(void *context, uint32_t offset, void *buffer, size_t length) {
    esy_file_ram_t *ram = context;
    esy_err_t result = valid(ram);
    if (result != ESY_OK) return result;
    if ((buffer == NULL && length != 0u) || aliases(ram, buffer, length)) return ESY_ERR_INVALID_ARG;
    if (!ram->exists) return ESY_ERR_STATE;
    if (offset > ram->length || length > ram->length - offset) return ESY_ERR_INVALID_ARG;
    if (length != 0u) memcpy(buffer, ram->resources.data_buffer + offset, length);
    return ESY_OK;
}
static esy_err_t ram_write(esy_file_ram_t *ram, const void *data, size_t length, bool create) {
    esy_err_t result = valid(ram);
    if (result != ESY_OK) return result;
    if ((data == NULL && length != 0u) || aliases(ram, data, length)) return ESY_ERR_INVALID_ARG;
    if (create && ram->exists) return ESY_ERR_BUSY;
    if (length > ram->resources.capacity) return ESY_ERR_NO_MEM;
    if (length != 0u) memcpy(ram->resources.data_buffer, data, length);
    ram->length = (uint32_t)length;
    ram->exists = true;
    return ESY_OK;
}
static esy_err_t ram_create(void *context, const void *data, size_t length) {
    return ram_write(context, data, length, true);
}
static esy_err_t ram_replace(void *context, const void *data, size_t length) {
    return ram_write(context, data, length, false);
}
static esy_err_t ram_remove(void *context) {
    esy_file_ram_t *ram = context;
    esy_err_t result = valid(ram);
    if (result != ESY_OK) return result;
    ram->length = 0u;
    ram->exists = false;
    return ESY_OK;
}
esy_err_t esy_file_ram_init(esy_file_ram_t *ram, const esy_file_ram_config_t *config) {
    if (ram == NULL) return ESY_ERR_INVALID_ARG;
    if (ram->initialized) return ESY_ERR_STATE;
    if (config == NULL || config->static_resources == NULL ||
        config->static_resources->data_buffer == NULL ||
        config->static_resources->capacity == 0u || config->static_resources->capacity > UINT32_MAX ||
        (!config->exists && config->initial_length != 0u)) return ESY_ERR_INVALID_ARG;
    const esy_file_ram_static_resources_t resources = *config->static_resources;
    if (overlap(resources.data_buffer, resources.capacity, ram, sizeof(*ram))) return ESY_ERR_INVALID_ARG;
    if (config->initial_length > resources.capacity) return ESY_ERR_NO_MEM;
    ram->resources = resources;
    ram->length = (uint32_t)config->initial_length;
    ram->exists = config->exists;
    ram->initialized = true;
    return ESY_OK;
}
esy_err_t esy_file_ram_get_ops(esy_file_ram_t *ram, esy_file_ops_t *ops) {
    esy_err_t result = valid(ram);
    if (result != ESY_OK) return result;
    if (ops == NULL || aliases(ram, ops, sizeof(*ops))) return ESY_ERR_INVALID_ARG;
    *ops = (esy_file_ops_t){.info = ram_info, .read = ram_read, .create = ram_create,
        .replace = ram_replace, .remove = ram_remove, .context = ram};
    return ESY_OK;
}
esy_err_t esy_file_ram_deinit(esy_file_ram_t *ram) {
    if (ram == NULL) return ESY_ERR_INVALID_ARG;
    *ram = (esy_file_ram_t){0};
    return ESY_OK;
}
