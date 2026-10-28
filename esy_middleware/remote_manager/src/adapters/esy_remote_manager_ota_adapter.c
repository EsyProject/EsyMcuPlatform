/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/remote_manager/esy_remote_manager_ota_adapter.h"

static esy_err_t image_info(void *context, esy_remote_manager_ota_info_t *info) {
    if (info == NULL) return ESY_ERR_INVALID_ARG;
    esy_remote_manager_ota_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->initialized) return ESY_ERR_STATE;
    return adapter->config.info(adapter->config.info_context, info);
}

static esy_err_t image_begin(void *context, const esy_image_manifest_t *manifest,
                              esy_image_status_t *status) {
    esy_remote_manager_ota_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->initialized) return ESY_ERR_STATE;
    return esy_ota_begin(adapter->config.ota, manifest, status);
}

static esy_err_t image_status(void *context, uint32_t session_id,
                               esy_image_status_t *status) {
    esy_remote_manager_ota_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->initialized) return ESY_ERR_STATE;
    esy_image_status_t snapshot = {0};
    esy_err_t result = esy_ota_status(adapter->config.ota, &snapshot);
    if (result != ESY_OK) return result;
    if (session_id != 0u && session_id != snapshot.session_id) return ESY_ERR_STATE;
    if (status == NULL) return ESY_ERR_INVALID_ARG;
    *status = snapshot;
    return ESY_OK;
}

static esy_err_t image_write(void *context, uint32_t session_id, uint32_t offset,
                              const uint8_t *data, size_t length,
                              esy_image_status_t *status) {
    esy_remote_manager_ota_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->initialized) return ESY_ERR_STATE;
    return esy_ota_write(adapter->config.ota, session_id, offset, data, length, status);
}

static esy_err_t image_finish(void *context, uint32_t session_id,
                               esy_image_status_t *status) {
    esy_remote_manager_ota_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->initialized) return ESY_ERR_STATE;
    return esy_ota_finish(adapter->config.ota, session_id, status);
}

static esy_err_t image_abort(void *context, uint32_t session_id,
                              esy_image_status_t *status) {
    esy_remote_manager_ota_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->initialized) return ESY_ERR_STATE;
    return esy_ota_abort(adapter->config.ota, session_id, status);
}

static esy_err_t image_activate(void *context, uint32_t session_id,
                                 esy_image_status_t *status) {
    esy_remote_manager_ota_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->initialized) return ESY_ERR_STATE;
    return esy_ota_activate(adapter->config.ota, session_id, status);
}

esy_err_t esy_remote_manager_ota_adapter_init(
    esy_remote_manager_ota_adapter_t *adapter,
    const esy_remote_manager_ota_adapter_config_t *config) {
    if (adapter == NULL || config == NULL || config->ota == NULL ||
        config->info == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->initialized) return ESY_ERR_STATE;
    esy_image_status_t status;
    const esy_err_t result = esy_ota_status(config->ota, &status);
    if (result != ESY_OK) return result;
    adapter->config = *config;
    adapter->initialized = true;
    return ESY_OK;
}

esy_err_t esy_remote_manager_ota_adapter_get_ops(
    esy_remote_manager_ota_adapter_t *adapter, esy_remote_manager_ota_ops_t *ops) {
    if (adapter == NULL || ops == NULL) return ESY_ERR_INVALID_ARG;
    if (!adapter->initialized) return ESY_ERR_STATE;
    *ops = (esy_remote_manager_ota_ops_t){
        .info = image_info, .begin = image_begin, .status = image_status,
        .write = image_write, .finish = image_finish, .abort = image_abort,
        .activate = image_activate, .context = adapter,
        .images = adapter->config.images};
    return ESY_OK;
}

esy_err_t esy_remote_manager_ota_adapter_deinit(
    esy_remote_manager_ota_adapter_t *adapter) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    *adapter = (esy_remote_manager_ota_adapter_t){0};
    return ESY_OK;
}
