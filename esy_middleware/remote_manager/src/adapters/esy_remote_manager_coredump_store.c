/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/remote_manager/esy_remote_manager_coredump_store.h"

#include <string.h>

static esy_err_t store_info(void *context,
                             esy_remote_manager_coredump_info_t *info) {
    esy_remote_manager_coredump_store_t *adapter = context;
    if (adapter == NULL || info == NULL) return ESY_ERR_INVALID_ARG;
    if (!adapter->initialized) return ESY_ERR_STATE;
    esy_coredump_info_t stored;
    esy_err_t status = esy_coredump_get_info(adapter->config.store, &stored);
    if (status == ESY_ERR_STATE) {
        *info = (esy_remote_manager_coredump_info_t){0};
        return ESY_OK;
    }
    if (status != ESY_OK) return status;
    *info = (esy_remote_manager_coredump_info_t){
        .record_size = stored.record_size,
        .record_crc = stored.record_crc,
        .available = true,
        .record_id = stored.record_id,
    };
    return ESY_OK;
}

static esy_err_t store_read(void *context, const esy_coredump_record_id_t *record_id,
                             uint32_t offset, uint8_t *output,
                             size_t length) {
    esy_remote_manager_coredump_store_t *adapter = context;
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (!adapter->initialized) return ESY_ERR_STATE;
    return esy_coredump_read(adapter->config.store, record_id, offset, output, length);
}

static esy_err_t store_clear(void *context, const esy_coredump_record_id_t *record_id) {
    esy_remote_manager_coredump_store_t *adapter = context;
    if (adapter == NULL || record_id == NULL) return ESY_ERR_INVALID_ARG;
    if (!adapter->initialized) return ESY_ERR_STATE;
    if (adapter->config.clear == NULL) return ESY_ERR_NOT_SUPPORTED;
    bool nonzero = false;
    for (size_t i = 0u; i < sizeof(record_id->bytes); ++i)
        nonzero = nonzero || record_id->bytes[i] != 0u;
    if (!nonzero) return ESY_ERR_INVALID_ARG;
    esy_coredump_info_t info;
    const esy_err_t result = esy_coredump_get_info(adapter->config.store, &info);
    if (result != ESY_OK) return result;
    if (memcmp(record_id->bytes, info.record_id.bytes, ESY_COREDUMP_RECORD_ID_BYTES) != 0)
        return ESY_ERR_STATE;
    return adapter->config.clear(adapter->config.clear_context, record_id);
}

esy_err_t esy_remote_manager_coredump_store_init(
    esy_remote_manager_coredump_store_t *adapter,
    const esy_remote_manager_coredump_store_config_t *config) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->initialized) return ESY_ERR_STATE;
    if (config == NULL || config->store == NULL) return ESY_ERR_INVALID_ARG;
    adapter->config = *config;
    adapter->initialized = true;
    return ESY_OK;
}

esy_err_t esy_remote_manager_coredump_store_deinit(
    esy_remote_manager_coredump_store_t *adapter) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    *adapter = (esy_remote_manager_coredump_store_t){0};
    return ESY_OK;
}

esy_err_t esy_remote_manager_coredump_store_get_ops(
    esy_remote_manager_coredump_store_t *adapter,
    esy_remote_manager_coredump_ops_t *ops) {
    if (ops == NULL) return ESY_ERR_INVALID_ARG;
    *ops = (esy_remote_manager_coredump_ops_t){0};
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (!adapter->initialized) return ESY_ERR_STATE;
    *ops = (esy_remote_manager_coredump_ops_t){
        .info = store_info, .read = store_read,
        .clear = adapter->config.clear != NULL ? store_clear : NULL,
        .context = adapter,
    };
    return ESY_OK;
}
