/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/remote_manager/esy_remote_manager.h"

#include <string.h>

#include "esy_remote_manager_internal.h"
#include "esy_remote_manager_stats_internal.h"
#include "esy_remote_manager_diagnostics_internal.h"

static bool manifest_empty(const esy_image_manifest_t *manifest) {
    if (manifest->version != 0u || manifest->image_size != 0u) return false;
    for (size_t i = 0u; i < sizeof(manifest->image_sha256); ++i)
        if (manifest->image_sha256[i] != 0u) return false;
    for (size_t i = 0u; i < sizeof(manifest->signature); ++i)
        if (manifest->signature[i] != 0u) return false;
    return true;
}

static bool image_id_empty(const esy_remote_manager_image_id_t *image) {
    if (image->slot != 0u || image->version != 0u) return false;
    for (size_t i = 0u; i < sizeof(image->sha256); ++i)
        if (image->sha256[i] != 0u) return false;
    return true;
}

static bool record_id_empty(const esy_coredump_record_id_t *record_id) {
    for (size_t i = 0u; i < sizeof(record_id->bytes); ++i)
        if (record_id->bytes[i] != 0u) return false;
    return true;
}

static esy_err_t validate_request(const esy_remote_manager_config_t *config,
                                  const esy_remote_manager_request_t *request) {
    if ((request->key == NULL && request->key_length != 0u) ||
        (request->value == NULL && request->value_length != 0u) ||
        request->key_length > config->max_key_length ||
        request->value_length > config->max_request_value_length ||
        (request->key_length != 0u &&
         memchr(request->key, '\0', request->key_length) != NULL))
        return ESY_ERR_INVALID_ARG;
    const bool ota = request->group == ESY_REMOTE_MANAGER_GROUP_OTA;
    const bool begin = ota && request->command == ESY_REMOTE_MANAGER_OTA_BEGIN;
    const bool write = ota && request->command == ESY_REMOTE_MANAGER_OTA_WRITE;
    const bool slot = ota && request->command == ESY_REMOTE_MANAGER_OTA_SLOT;
    const bool image_mutation = ota &&
        (request->command == ESY_REMOTE_MANAGER_OTA_CONFIRM ||
         request->command == ESY_REMOTE_MANAGER_OTA_ROLLBACK);
    const bool dump_read = request->group == ESY_REMOTE_MANAGER_GROUP_COREDUMP &&
                          request->command == ESY_REMOTE_MANAGER_COREDUMP_READ;
    const bool dump_clear = request->group == ESY_REMOTE_MANAGER_GROUP_COREDUMP &&
                           request->command == ESY_REMOTE_MANAGER_COREDUMP_CLEAR;
    const bool has_record_id = !record_id_empty(&request->record_id);
    const bool stats = request->group == ESY_REMOTE_MANAGER_GROUP_STATS;
    const bool stats_enumerate = stats && request->command == ESY_REMOTE_MANAGER_STATS_ENUMERATE;
    const bool diagnostics = request->group == ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS;
    const bool diagnostic_enumerate = diagnostics &&
        request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE;
    if ((!begin && !manifest_empty(&request->manifest)) ||
        (!ota && request->session_id != 0u) ||
        (!slot && request->slot_index != 0u) ||
        (!image_mutation && !image_id_empty(&request->image)) ||
        (!stats_enumerate && request->stat_index != 0u) ||
        (!diagnostic_enumerate && request->diagnostic_index != 0u) ||
        (has_record_id != (dump_read || dump_clear)) ||
        (!write && !dump_read && request->offset != 0u) ||
        (!dump_read && request->length != 0u)) return ESY_ERR_INVALID_ARG;
    if (diagnostics) {
        if (request->value_length != 0u) return ESY_ERR_INVALID_ARG;
        if (diagnostic_enumerate)
            return request->key_length == 0u &&
                   config->max_request_value_length >= ESY_REMOTE_MANAGER_DIAGNOSTICS_INDEX_BYTES
                ? ESY_OK : ESY_ERR_INVALID_ARG;
        if (request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_READ ||
            request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN)
            return esy_remote_manager_diagnostics_name_valid(request->key, request->key_length)
                ? ESY_OK : ESY_ERR_INVALID_ARG;
        return request->key_length == 0u ? ESY_OK : ESY_ERR_INVALID_ARG;
    }
    if (stats) {
        if (request->value_length != 0u) return ESY_ERR_INVALID_ARG;
        if (stats_enumerate)
            return request->key_length == 0u &&
                   config->max_request_value_length >= ESY_REMOTE_MANAGER_STATS_INDEX_BYTES
                ? ESY_OK : ESY_ERR_INVALID_ARG;
        if (request->command == ESY_REMOTE_MANAGER_STATS_READ)
            return esy_remote_manager_stats_name_valid(request->key, request->key_length)
                ? ESY_OK : ESY_ERR_INVALID_ARG;
        return ESY_OK;
    }
    if (ota) {
        if (request->key_length != 0u || (!write && request->value_length != 0u))
            return ESY_ERR_INVALID_ARG;
        size_t argument_bytes = 0u;
        switch (request->command) {
        case ESY_REMOTE_MANAGER_OTA_SLOT:
            if (request->session_id != 0u) return ESY_ERR_INVALID_ARG;
            argument_bytes = ESY_REMOTE_MANAGER_OTA_SLOT_INDEX_BYTES;
            break;
        case ESY_REMOTE_MANAGER_OTA_CONFIRM:
        case ESY_REMOTE_MANAGER_OTA_ROLLBACK:
            if (request->session_id != 0u) return ESY_ERR_INVALID_ARG;
            argument_bytes = ESY_REMOTE_MANAGER_OTA_IMAGE_ID_BYTES;
            break;
        case ESY_REMOTE_MANAGER_OTA_INFO:
            if (request->session_id != 0u) return ESY_ERR_INVALID_ARG;
            break;
        case ESY_REMOTE_MANAGER_OTA_BEGIN:
            if (request->session_id != 0u || request->manifest.image_size == 0u)
                return ESY_ERR_INVALID_ARG;
            argument_bytes = ESY_REMOTE_MANAGER_OTA_MANIFEST_BYTES;
            break;
        case ESY_REMOTE_MANAGER_OTA_WRITE:
            if (request->session_id == 0u || request->value_length == 0u ||
                request->value_length > UINT32_MAX - request->offset ||
                request->value_length > SIZE_MAX - ESY_REMOTE_MANAGER_OTA_WRITE_PREFIX_BYTES)
                return ESY_ERR_INVALID_ARG;
            argument_bytes = ESY_REMOTE_MANAGER_OTA_WRITE_PREFIX_BYTES + request->value_length;
            break;
        case ESY_REMOTE_MANAGER_OTA_FINISH:
        case ESY_REMOTE_MANAGER_OTA_ABORT:
        case ESY_REMOTE_MANAGER_OTA_ACTIVATE:
            if (request->session_id == 0u) return ESY_ERR_INVALID_ARG;
            /* fall through */
        case ESY_REMOTE_MANAGER_OTA_STATUS:
            argument_bytes = ESY_REMOTE_MANAGER_OTA_SESSION_BYTES;
            break;
        default:
            break;
        }
        return argument_bytes <= config->max_request_value_length
                   ? ESY_OK : ESY_ERR_INVALID_ARG;
    }
    if (dump_read && (request->length > UINT32_MAX - request->offset ||
                     config->max_request_value_length < ESY_REMOTE_MANAGER_COREDUMP_READ_BYTES))
        return ESY_ERR_INVALID_ARG;
    if (dump_clear && config->max_request_value_length < ESY_COREDUMP_RECORD_ID_BYTES)
        return ESY_ERR_INVALID_ARG;
    if (request->group == ESY_REMOTE_MANAGER_GROUP_SYSTEM &&
        request->command == ESY_REMOTE_MANAGER_SYSTEM_DISCOVER)
        return request->key_length == 0u && request->value_length == 0u
                   ? ESY_OK : ESY_ERR_INVALID_ARG;
    if (request->group == ESY_REMOTE_MANAGER_GROUP_COREDUMP)
        return request->key_length == 0u && request->value_length == 0u
                   ? ESY_OK : ESY_ERR_INVALID_ARG;
    if (request->group != ESY_REMOTE_MANAGER_GROUP_CONFIG) return ESY_OK;
    switch (request->command) {
    case ESY_REMOTE_MANAGER_CONFIG_GET:
    case ESY_REMOTE_MANAGER_CONFIG_REMOVE:
        return request->key_length != 0u && request->value_length == 0u
                   ? ESY_OK : ESY_ERR_INVALID_ARG;
    case ESY_REMOTE_MANAGER_CONFIG_SET:
        return request->key_length != 0u ? ESY_OK : ESY_ERR_INVALID_ARG;
    case ESY_REMOTE_MANAGER_CONFIG_APPLY:
    case ESY_REMOTE_MANAGER_CONFIG_SAVE:
        return request->key_length == 0u && request->value_length == 0u
                   ? ESY_OK : ESY_ERR_INVALID_ARG;
    default:
        return ESY_OK;
    }
}

static uint32_t get_capabilities(const esy_remote_manager_config_t *config) {
    const esy_remote_manager_config_ops_t *ops = &config->config_ops;
    uint32_t capabilities = ESY_REMOTE_MANAGER_CAP_DISCOVER;
    if (ops->get != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_CONFIG_GET;
    if (ops->set != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_CONFIG_SET;
    if (ops->remove != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_CONFIG_REMOVE;
    if (ops->apply != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_CONFIG_APPLY;
    if (ops->save != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_CONFIG_SAVE;
    if (config->ota_ops.info != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_INFO;
    if (config->ota_ops.begin != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_BEGIN;
    if (config->ota_ops.write != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_WRITE;
    if (config->ota_ops.finish != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_FINISH;
    if (config->ota_ops.abort != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_ABORT;
    if (config->ota_ops.activate != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_ACTIVATE;
    if (config->ota_ops.status != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_STATUS;
    if (config->ota_ops.images.slot != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_SLOT;
    if (config->ota_ops.images.confirm != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_CONFIRM;
    if (config->ota_ops.images.rollback != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_OTA_ROLLBACK;
    if (config->coredump_ops.info != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_COREDUMP_INFO;
    if (config->coredump_ops.read != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_COREDUMP_READ;
    if (config->coredump_ops.clear != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_COREDUMP_CLEAR;
    if (config->stats_ops.enumerate != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_STATS_ENUMERATE;
    if (config->stats_ops.read != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_STATS_READ;
    if (config->diagnostics_ops.enumerate != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_ENUMERATE;
    if (config->diagnostics_ops.read != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_READ;
    if (config->diagnostics_ops.run != NULL) capabilities |= ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_RUN;
    return capabilities;
}

esy_err_t esy_remote_manager_init(esy_remote_manager_t *manager,
                                  const esy_remote_manager_config_t *config) {
    if (manager == NULL) return ESY_ERR_INVALID_ARG;
    if (manager->busy) return ESY_ERR_BUSY;
    if (manager->initialized) return ESY_ERR_STATE;
    if (config == NULL || config->authorize == NULL ||
        config->max_key_length == 0u) {
        return ESY_ERR_INVALID_ARG;
    }
    manager->config = *config;
    manager->initialized = true;
    return ESY_OK;
}

esy_err_t esy_remote_manager_deinit(esy_remote_manager_t *manager) {
    if (manager == NULL) return ESY_ERR_INVALID_ARG;
    if (manager->busy) return ESY_ERR_BUSY;
    *manager = (esy_remote_manager_t){0};
    return ESY_OK;
}

esy_err_t esy_remote_manager_dispatch(
    esy_remote_manager_t *manager, const esy_remote_manager_request_t *request,
    uint8_t *output, size_t capacity, esy_remote_manager_response_t *response) {
    if (response == NULL) return ESY_ERR_INVALID_ARG;
    *response = (esy_remote_manager_response_t){.status = ESY_ERR_INVALID_ARG};
    if (manager == NULL || request == NULL ||
        (output == NULL && capacity != 0u)) {
        return response->status;
    }
    if (manager->busy) {
        response->status = ESY_ERR_BUSY;
        return response->status;
    }
    if (!manager->initialized) {
        response->status = ESY_ERR_STATE;
        return response->status;
    }
    esy_err_t status = validate_request(&manager->config, request);
    if (status != ESY_OK) return response->status;

    manager->busy = true;
    status = manager->config.authorize(manager->config.authorize_context, request);
    if (status == ESY_OK) {
        if (request->group == ESY_REMOTE_MANAGER_GROUP_SYSTEM &&
            request->command == ESY_REMOTE_MANAGER_SYSTEM_DISCOVER) {
            response->capabilities = get_capabilities(&manager->config);
        } else if (request->group == ESY_REMOTE_MANAGER_GROUP_CONFIG) {
            size_t available = capacity;
            if (available > manager->config.max_response_value_length) {
                available = manager->config.max_response_value_length;
            }
            status = esy_remote_manager_config_execute(
                &manager->config.config_ops, request, output, available,
                manager->config.max_response_value_length, response);
        } else {
            size_t available = capacity;
            if (available > manager->config.max_response_value_length)
                available = manager->config.max_response_value_length;
            status = esy_remote_manager_operations_execute(
                &manager->config, request, output, available, response);
        }
    }
    manager->busy = false;
    response->status = status > ESY_OK ? ESY_ERR_STATE : status;
    return response->status;
}
