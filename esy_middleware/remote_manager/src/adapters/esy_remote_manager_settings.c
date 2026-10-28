/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/remote_manager/esy_remote_manager_settings.h"

#include <string.h>

static esy_err_t prepare_key(esy_remote_manager_settings_t *adapter,
                              const char *key, size_t length) {
    if (adapter == NULL || key == NULL || length == 0u) return ESY_ERR_INVALID_ARG;
    if (!adapter->initialized) return ESY_ERR_STATE;
    if (length >= adapter->key_buffer_size) return ESY_ERR_INVALID_ARG;
    if (memchr(key, '\0', length) != NULL) return ESY_ERR_INVALID_ARG;
    memcpy(adapter->key_buffer, key, length);
    adapter->key_buffer[length] = '\0';
    return ESY_OK;
}

static esy_err_t settings_get(void *context, const char *key, size_t key_length,
                               uint8_t *output, size_t capacity, size_t *length,
                               bool *found) {
    esy_remote_manager_settings_t *adapter = context;
    esy_err_t status = prepare_key(adapter, key, key_length);
    if (status != ESY_OK) return status;
    return esy_settings_get(adapter->settings, adapter->key_buffer, output,
                            capacity, length, found);
}

static esy_err_t settings_set(void *context, const char *key, size_t key_length,
                               const uint8_t *value, size_t length) {
    esy_remote_manager_settings_t *adapter = context;
    esy_err_t status = prepare_key(adapter, key, key_length);
    if (status != ESY_OK) return status;
    return esy_settings_set(adapter->settings, adapter->key_buffer, value, length);
}

static esy_err_t settings_remove(void *context, const char *key,
                                  size_t key_length) {
    esy_remote_manager_settings_t *adapter = context;
    esy_err_t status = prepare_key(adapter, key, key_length);
    if (status != ESY_OK) return status;
    return esy_settings_remove(adapter->settings, adapter->key_buffer);
}

static esy_err_t settings_save(void *context) {
    esy_remote_manager_settings_t *adapter = context;
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (!adapter->initialized) return ESY_ERR_STATE;
    return esy_settings_commit(adapter->settings);
}

esy_err_t esy_remote_manager_settings_init(
    esy_remote_manager_settings_t *adapter,
    const esy_remote_manager_settings_config_t *config) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->initialized) return ESY_ERR_STATE;
    if (config == NULL || config->settings == NULL ||
        config->static_resources == NULL ||
        config->static_resources->key_buffer == NULL ||
        config->static_resources->key_buffer_size <
            ESY_REMOTE_MANAGER_SETTINGS_KEY_BUFFER_MIN_BYTES) return ESY_ERR_INVALID_ARG;
    esy_settings_info_t info;
    esy_err_t status = esy_settings_get_info(config->settings, &info);
    if (status != ESY_OK) return status;
    adapter->settings = config->settings;
    adapter->key_buffer = config->static_resources->key_buffer;
    adapter->key_buffer_size = config->static_resources->key_buffer_size;
    adapter->initialized = true;
    return ESY_OK;
}

esy_err_t esy_remote_manager_settings_deinit(
    esy_remote_manager_settings_t *adapter) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    *adapter = (esy_remote_manager_settings_t){0};
    return ESY_OK;
}

esy_err_t esy_remote_manager_settings_get_ops(
    esy_remote_manager_settings_t *adapter,
    esy_remote_manager_config_ops_t *ops) {
    if (ops == NULL) return ESY_ERR_INVALID_ARG;
    *ops = (esy_remote_manager_config_ops_t){0};
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (!adapter->initialized) return ESY_ERR_STATE;
    *ops = (esy_remote_manager_config_ops_t){
        .get = settings_get, .set = settings_set, .remove = settings_remove,
        .save = settings_save, .context = adapter,
    };
    return ESY_OK;
}
