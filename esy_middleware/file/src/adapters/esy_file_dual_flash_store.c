/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/file/esy_file_dual_flash_store.h"

static esy_err_t valid(const esy_file_dual_flash_store_t *adapter) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    return adapter->initialized ? ESY_OK : ESY_ERR_STATE;
}
static esy_err_t record_info(void *context, esy_file_info_t *info) {
    esy_file_dual_flash_store_t *adapter = context;
    esy_err_t result = valid(adapter);
    if (result != ESY_OK) return result;
    if (info == NULL) return ESY_ERR_INVALID_ARG;
    esy_dual_flash_store_info_t record;
    result = esy_dual_flash_store_get_info(adapter->store, &record);
    if (result == ESY_OK) *info = (esy_file_info_t){.exists = record.has_record,
        .length = record.length, .capacity = record.capacity};
    return result;
}
static esy_err_t record_read(void *context, uint32_t offset, void *buffer, size_t length) {
    esy_file_dual_flash_store_t *adapter = context;
    esy_err_t result = valid(adapter);
    return result == ESY_OK ? esy_dual_flash_store_read_range(adapter->store, offset, buffer, length) : result;
}
static esy_err_t record_create(void *context, const void *data, size_t length) {
    esy_file_dual_flash_store_t *adapter = context;
    esy_err_t result = valid(adapter);
    if (result != ESY_OK) return result;
    esy_dual_flash_store_info_t info;
    result = esy_dual_flash_store_get_info(adapter->store, &info);
    if (result != ESY_OK) return result;
    return info.has_record ? ESY_ERR_BUSY : esy_dual_flash_store_write(adapter->store, data, length);
}
static esy_err_t record_replace(void *context, const void *data, size_t length) {
    esy_file_dual_flash_store_t *adapter = context;
    esy_err_t result = valid(adapter);
    return result == ESY_OK ? esy_dual_flash_store_write(adapter->store, data, length) : result;
}
static esy_err_t record_remove(void *context) {
    esy_file_dual_flash_store_t *adapter = context;
    esy_err_t result = valid(adapter);
    return result == ESY_OK ? esy_dual_flash_store_clear(adapter->store) : result;
}
esy_err_t esy_file_dual_flash_store_init(esy_file_dual_flash_store_t *adapter,
                                    const esy_file_dual_flash_store_config_t *config) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->initialized) return ESY_ERR_STATE;
    if (config == NULL || config->store == NULL) return ESY_ERR_INVALID_ARG;
    esy_dual_flash_store_info_t info;
    esy_err_t result = esy_dual_flash_store_get_info(config->store, &info);
    if (result != ESY_OK) return result;
    adapter->store = config->store;
    adapter->initialized = true;
    return ESY_OK;
}
esy_err_t esy_file_dual_flash_store_get_ops(esy_file_dual_flash_store_t *adapter, esy_file_ops_t *ops) {
    esy_err_t result = valid(adapter);
    if (result != ESY_OK) return result;
    if (ops == NULL) return ESY_ERR_INVALID_ARG;
    *ops = (esy_file_ops_t){.info = record_info, .read = record_read,
        .create = record_create, .replace = record_replace, .remove = record_remove,
        .context = adapter};
    return ESY_OK;
}
esy_err_t esy_file_dual_flash_store_deinit(esy_file_dual_flash_store_t *adapter) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    *adapter = (esy_file_dual_flash_store_t){0};
    return ESY_OK;
}
