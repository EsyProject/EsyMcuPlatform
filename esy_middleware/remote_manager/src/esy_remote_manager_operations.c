/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_remote_manager_internal.h"

#include <string.h>

static bool image_slot_valid(const esy_remote_manager_image_slot_t *slot) {
    const uint32_t known_flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT |
        ESY_REMOTE_MANAGER_IMAGE_RUNNING | ESY_REMOTE_MANAGER_IMAGE_PENDING |
        ESY_REMOTE_MANAGER_IMAGE_CONFIRMED | ESY_REMOTE_MANAGER_IMAGE_BOOTABLE;
    if ((slot->flags & ~known_flags) != 0u) return false;
    if (slot->slot_count == 0u) {
        if (slot->index != 0u || slot->image.slot != 0u || slot->capacity != 0u)
            return false;
    } else if (slot->index >= slot->slot_count || slot->capacity == 0u) {
        return false;
    }
    if (slot->image_size > slot->capacity) return false;
    if ((slot->flags & ESY_REMOTE_MANAGER_IMAGE_PRESENT) != 0u)
        return slot->slot_count != 0u && slot->image_size != 0u;
    if (slot->flags != 0u || slot->image_size != 0u || slot->image.version != 0u)
        return false;
    for (size_t i = 0u; i < sizeof(slot->image.sha256); ++i)
        if (slot->image.sha256[i] != 0u) return false;
    return true;
}

static esy_err_t execute_images(const esy_remote_manager_image_ops_t *ops,
                                const esy_remote_manager_request_t *request,
                                esy_remote_manager_response_t *response) {
    esy_remote_manager_image_slot_t slot = {0};
    esy_err_t result = ESY_ERR_NOT_SUPPORTED;
    switch (request->command) {
    case ESY_REMOTE_MANAGER_OTA_SLOT:
        if (ops->slot != NULL)
            result = ops->slot(ops->context, request->slot_index, &slot);
        break;
    case ESY_REMOTE_MANAGER_OTA_CONFIRM:
        if (ops->confirm != NULL)
            result = ops->confirm(ops->context, &request->image, &slot);
        break;
    case ESY_REMOTE_MANAGER_OTA_ROLLBACK:
        if (ops->rollback != NULL)
            result = ops->rollback(ops->context, &request->image, &slot);
        break;
    default:
        break;
    }
    if (result != ESY_OK) return result;
    if (!image_slot_valid(&slot)) return ESY_ERR_STATE;
    if (request->command == ESY_REMOTE_MANAGER_OTA_SLOT) {
        if (slot.index != request->slot_index) return ESY_ERR_STATE;
    } else {
        if (slot.slot_count == 0u || slot.image.slot != request->image.slot ||
            slot.image.version != request->image.version ||
            memcmp(slot.image.sha256, request->image.sha256,
                   ESY_IMAGE_SHA256_BYTES) != 0)
            return ESY_ERR_STATE;
        const uint32_t required = request->command == ESY_REMOTE_MANAGER_OTA_CONFIRM
            ? ESY_REMOTE_MANAGER_IMAGE_RUNNING | ESY_REMOTE_MANAGER_IMAGE_CONFIRMED
            : ESY_REMOTE_MANAGER_IMAGE_BOOTABLE | ESY_REMOTE_MANAGER_IMAGE_PENDING;
        if ((slot.flags & required) != required ||
            (request->command == ESY_REMOTE_MANAGER_OTA_ROLLBACK &&
             (slot.flags & ESY_REMOTE_MANAGER_IMAGE_RUNNING) != 0u))
            return ESY_ERR_STATE;
    }
    response->image_slot = slot;
    return ESY_OK;
}

static esy_err_t execute_ota(const esy_remote_manager_ota_ops_t *ops,
                             const esy_remote_manager_request_t *request,
                             esy_remote_manager_response_t *response) {
    if (request->command == ESY_REMOTE_MANAGER_OTA_SLOT ||
        request->command == ESY_REMOTE_MANAGER_OTA_CONFIRM ||
        request->command == ESY_REMOTE_MANAGER_OTA_ROLLBACK)
        return execute_images(&ops->images, request, response);
    if (request->command == ESY_REMOTE_MANAGER_OTA_INFO) {
        if (ops->info == NULL) return ESY_ERR_NOT_SUPPORTED;
        esy_remote_manager_ota_info_t info = {0};
        const esy_err_t result = ops->info(ops->context, &info);
        if (result == ESY_OK) {
            if (info.running_slot < -1) return ESY_ERR_STATE;
            response->ota_info = info;
        }
        return result;
    }
    esy_image_status_t snapshot = {0};
    esy_err_t result = ESY_ERR_NOT_SUPPORTED;
    switch (request->command) {
    case ESY_REMOTE_MANAGER_OTA_BEGIN:
        if (ops->begin != NULL)
            result = ops->begin(ops->context, &request->manifest, &snapshot);
        break;
    case ESY_REMOTE_MANAGER_OTA_WRITE:
        if (ops->write != NULL)
            result = ops->write(ops->context, request->session_id, request->offset,
                                request->value, request->value_length, &snapshot);
        break;
    case ESY_REMOTE_MANAGER_OTA_FINISH:
        if (ops->finish != NULL)
            result = ops->finish(ops->context, request->session_id, &snapshot);
        break;
    case ESY_REMOTE_MANAGER_OTA_ABORT:
        if (ops->abort != NULL)
            result = ops->abort(ops->context, request->session_id, &snapshot);
        break;
    case ESY_REMOTE_MANAGER_OTA_ACTIVATE:
        if (ops->activate != NULL)
            result = ops->activate(ops->context, request->session_id, &snapshot);
        break;
    case ESY_REMOTE_MANAGER_OTA_STATUS:
        if (ops->status != NULL)
            result = ops->status(ops->context, request->session_id, &snapshot);
        break;
    default:
        break;
    }
    if (result == ESY_OK) {
        if ((unsigned)snapshot.phase > ESY_IMAGE_FAILED ||
            snapshot.next_offset > snapshot.total_bytes ||
            (snapshot.result_valid && snapshot.phase != ESY_IMAGE_ACTIVATED &&
             snapshot.phase != ESY_IMAGE_ABORTED && snapshot.phase != ESY_IMAGE_FAILED) ||
            (snapshot.phase != ESY_IMAGE_IDLE && snapshot.session_id == 0u) ||
            (request->session_id != 0u && request->session_id != snapshot.session_id))
            return ESY_ERR_STATE;
        response->ota_status = snapshot;
    }
    return result;
}

static esy_err_t execute_coredump(const esy_remote_manager_config_t *config,
                                  const esy_remote_manager_request_t *request,
                                  uint8_t *output, size_t capacity,
                                  esy_remote_manager_response_t *response) {
    const esy_remote_manager_coredump_ops_t *ops = &config->coredump_ops;
    if (request->command == ESY_REMOTE_MANAGER_COREDUMP_INFO && ops->info != NULL) {
        esy_remote_manager_coredump_info_t info = {0};
        esy_err_t status = ops->info(ops->context, &info);
        if (status == ESY_OK) {
            bool nonzero_id = false;
            for (size_t i = 0u; i < sizeof(info.record_id.bytes); ++i)
                nonzero_id = nonzero_id || info.record_id.bytes[i] != 0u;
            if ((info.available && (info.record_size == 0u || !nonzero_id)) ||
                (!info.available && (info.record_size != 0u || info.record_crc != 0u || nonzero_id)))
                return ESY_ERR_STATE;
            response->coredump_info = info;
        }
        return status;
    }
    if (request->command == ESY_REMOTE_MANAGER_COREDUMP_READ && ops->read != NULL) {
        if (request->length > config->max_response_value_length) return ESY_ERR_INVALID_ARG;
        if (request->length > capacity) {
            response->value_length = request->length;
            return ESY_ERR_NO_MEM;
        }
        esy_err_t status = ops->read(ops->context, &request->record_id, request->offset, output,
                                     request->length);
        if (status == ESY_OK) response->value_length = request->length;
        return status;
    }
    if (request->command == ESY_REMOTE_MANAGER_COREDUMP_CLEAR && ops->clear != NULL)
        return ops->clear(ops->context, &request->record_id);
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_remote_manager_operations_execute(
    const esy_remote_manager_config_t *config,
    const esy_remote_manager_request_t *request, uint8_t *output,
    size_t capacity, esy_remote_manager_response_t *response) {
    switch (request->group) {
    case ESY_REMOTE_MANAGER_GROUP_OTA:
        return execute_ota(&config->ota_ops, request, response);
    case ESY_REMOTE_MANAGER_GROUP_COREDUMP:
        return execute_coredump(config, request, output, capacity, response);
    case ESY_REMOTE_MANAGER_GROUP_STATS:
        return esy_remote_manager_stats_execute(&config->stats_ops, request, response);
    case ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS:
        return esy_remote_manager_diagnostics_execute(&config->diagnostics_ops, request, response);
    default:
        return ESY_ERR_NOT_SUPPORTED;
    }
}
