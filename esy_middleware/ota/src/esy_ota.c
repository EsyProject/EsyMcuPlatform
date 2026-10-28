/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/ota/esy_ota.h"

#include <limits.h>
#include <string.h>

static esy_err_t checked_result(esy_err_t result) {
    return result > ESY_OK ? ESY_ERR_STATE : result;
}

static bool manifest_equal(const esy_image_manifest_t *left,
                            const esy_image_manifest_t *right) {
    return left->version == right->version && left->image_size == right->image_size &&
           memcmp(left->image_sha256, right->image_sha256,
                  ESY_IMAGE_SHA256_BYTES) == 0 &&
           memcmp(left->signature, right->signature, ESY_IMAGE_SIGNATURE_BYTES) == 0;
}

static bool range_valid(const void *data, size_t size) {
    return data != NULL && size <= UINTPTR_MAX - (uintptr_t)data;
}

static bool ranges_overlap(const void *left, size_t left_size,
                            const void *right, size_t right_size) {
    return left_size != 0u && right_size != 0u &&
           (uintptr_t)left < (uintptr_t)right + right_size &&
           (uintptr_t)right < (uintptr_t)left + left_size;
}

static esy_err_t cleanup(esy_ota_t *ota) {
    if (ota->provider_owned) {
        const esy_err_t status = checked_result(
            ota->config.provider.abort(ota->config.provider.context));
        if (status != ESY_OK) return status;
        ota->provider_owned = false;
    }
    if (ota->guard_held) {
        const esy_err_t status = checked_result(
            esy_activity_guard_release(ota->config.activity_guard));
        if (status != ESY_OK) return status;
        ota->guard_held = false;
    }
    return ESY_OK;
}

static esy_err_t fail_session(esy_ota_t *ota, esy_err_t cause) {
    ota->deadline_active = false;
    ota->snapshot.phase = ESY_IMAGE_FAILED;
    ota->snapshot.result_valid = true;
    const esy_err_t released = cleanup(ota);
    ota->snapshot.result = released == ESY_OK ? cause : released;
    return ota->snapshot.result;
}

static void mark_progress(esy_ota_t *ota) {
    if (ota->config.progress_timeout_ms != 0u) {
        ota->last_progress_ms = ota->config.clock_ms(ota->config.clock_context);
        ota->deadline_active = true;
    }
}

static esy_err_t expire_if_due(esy_ota_t *ota) {
    if (!ota->deadline_active) return ESY_OK;
    const uint32_t now = ota->config.clock_ms(ota->config.clock_context);
    if ((uint32_t)(now - ota->last_progress_ms) < ota->config.progress_timeout_ms)
        return ESY_OK;
    return fail_session(ota, ESY_ERR_TIMEOUT);
}

static esy_err_t complete_progress(esy_ota_t *ota) {
    if (ota->config.progress_timeout_ms == 0u) return ESY_OK;
    const uint32_t now = ota->config.clock_ms(ota->config.clock_context);
    if (ota->deadline_active &&
        (uint32_t)(now - ota->last_progress_ms) >= ota->config.progress_timeout_ms)
        return fail_session(ota, ESY_ERR_TIMEOUT);
    ota->last_progress_ms = now;
    ota->deadline_active = true;
    return ESY_OK;
}

static esy_err_t enter_operation(esy_ota_t *ota, esy_image_status_t *status) {
    if (status == NULL) return ESY_ERR_INVALID_ARG;
    *status = (esy_image_status_t){0};
    if (ota == NULL) return ESY_ERR_INVALID_ARG;
    if (ota->busy) return ESY_ERR_BUSY;
    if (!ota->initialized) return ESY_ERR_STATE;
    *status = ota->snapshot;
    ota->busy = true;
    return ESY_OK;
}

static esy_err_t leave_operation(esy_ota_t *ota, esy_image_status_t *status,
                                 esy_err_t result) {
    *status = ota->snapshot;
    ota->busy = false;
    return result;
}

static esy_err_t check_session(const esy_ota_t *ota, uint32_t session_id) {
    if (session_id == 0u) return ESY_ERR_INVALID_ARG;
    return session_id == ota->snapshot.session_id ? ESY_OK : ESY_ERR_STATE;
}

esy_err_t esy_ota_init(esy_ota_t *ota, const esy_ota_config_t *config) {
    if (ota == NULL) return ESY_ERR_INVALID_ARG;
    if (ota->busy) return ESY_ERR_BUSY;
    if (ota->initialized) return ESY_ERR_STATE;
    if (config == NULL || config->provider.begin == NULL ||
        config->provider.write == NULL || config->provider.finish == NULL ||
        config->provider.abort == NULL || config->new_session_id == NULL ||
        config->max_image_bytes == 0u || config->max_chunk_bytes == 0u ||
        config->static_resources == NULL || config->progress_timeout_ms > INT32_MAX ||
        (config->progress_timeout_ms != 0u && config->clock_ms == NULL))
        return ESY_ERR_INVALID_ARG;
    const esy_ota_static_resources_t *resources = config->static_resources;
    if (resources->last_chunk_capacity < config->max_chunk_bytes ||
        !range_valid(resources->last_chunk_buffer, resources->last_chunk_capacity) ||
        !range_valid(ota, sizeof(*ota)) ||
        ranges_overlap(resources->last_chunk_buffer, resources->last_chunk_capacity,
                        ota, sizeof(*ota))) return ESY_ERR_INVALID_ARG;
    ota->config = *config;
    ota->resources = *resources;
    ota->config.static_resources = &ota->resources;
    ota->initialized = true;
    return ESY_OK;
}

esy_err_t esy_ota_deinit(esy_ota_t *ota) {
    if (ota == NULL) return ESY_ERR_INVALID_ARG;
    if (ota->busy) return ESY_ERR_BUSY;
    if (!ota->initialized) return ESY_OK;
    ota->busy = true;
    ota->deadline_active = false;
    const esy_err_t status = cleanup(ota);
    if (status != ESY_OK) {
        if (ota->snapshot.phase != ESY_IMAGE_ACTIVATED)
            ota->snapshot.phase = ESY_IMAGE_FAILED;
        ota->snapshot.result = status;
        ota->snapshot.result_valid = true;
        ota->busy = false;
        return status;
    }
    *ota = (esy_ota_t){0};
    return ESY_OK;
}

esy_err_t esy_ota_begin(esy_ota_t *ota, const esy_image_manifest_t *manifest,
                        esy_image_status_t *status) {
    esy_err_t result = enter_operation(ota, status);
    if (result != ESY_OK) return result;
    if (manifest == NULL || manifest->version == 0u || manifest->image_size == 0u ||
        manifest->image_size > ota->config.max_image_bytes)
        return leave_operation(ota, status, ESY_ERR_INVALID_ARG);
    result = expire_if_due(ota);
    if (result != ESY_OK) return leave_operation(ota, status, result);
    if (ota->snapshot.phase == ESY_IMAGE_RECEIVING ||
        ota->snapshot.phase == ESY_IMAGE_READY) {
        result = manifest_equal(manifest, &ota->manifest) ? ESY_OK : ESY_ERR_BUSY;
        return leave_operation(ota, status, result);
    }
    if (ota->provider_owned || ota->guard_held)
        return leave_operation(ota, status, ESY_ERR_STATE);
    uint32_t session_id = 0u;
    result = checked_result(ota->config.new_session_id(
        ota->config.session_id_context, &session_id));
    if (result != ESY_OK) return leave_operation(ota, status, result);
    if (session_id == 0u || session_id == ota->snapshot.session_id)
        return leave_operation(ota, status, ESY_ERR_STATE);
    ota->manifest = *manifest;
    ota->snapshot = (esy_image_status_t){
        .phase = ESY_IMAGE_RECEIVING, .session_id = session_id,
        .total_bytes = manifest->image_size};
    ota->last_chunk_length = 0u;
    ota->last_chunk_offset = 0u;
    mark_progress(ota);
    result = checked_result(esy_activity_guard_acquire(ota->config.activity_guard));
    if (result != ESY_OK)
        return leave_operation(ota, status, fail_session(ota, result));
    ota->guard_held = ota->config.activity_guard != NULL;
    ota->provider_owned = true;
    result = checked_result(ota->config.provider.begin(
        ota->config.provider.context, &ota->manifest));
    if (result != ESY_OK)
        return leave_operation(ota, status, fail_session(ota, result));
    result = complete_progress(ota);
    return leave_operation(ota, status, result);
}

esy_err_t esy_ota_write(esy_ota_t *ota, uint32_t session_id, uint32_t offset,
                        const uint8_t *data, size_t length,
                        esy_image_status_t *status) {
    esy_err_t result = enter_operation(ota, status);
    if (result != ESY_OK) return result;
    result = check_session(ota, session_id);
    if (result != ESY_OK) return leave_operation(ota, status, result);
    if (length == 0u || (uintmax_t)length > ota->config.max_chunk_bytes ||
        !range_valid(data, length) ||
        ranges_overlap(data, length, ota->resources.last_chunk_buffer,
                        ota->resources.last_chunk_capacity))
        return leave_operation(ota, status, ESY_ERR_INVALID_ARG);
    result = expire_if_due(ota);
    if (result != ESY_OK) return leave_operation(ota, status, result);
    const esy_image_phase_t phase = ota->snapshot.phase;
    if ((phase == ESY_IMAGE_RECEIVING || phase == ESY_IMAGE_READY ||
         phase == ESY_IMAGE_ACTIVATED) && ota->last_chunk_length != 0u &&
        offset == ota->last_chunk_offset && length == ota->last_chunk_length &&
        memcmp(data, ota->resources.last_chunk_buffer, length) == 0)
        return leave_operation(ota, status, ESY_OK);
    if (phase != ESY_IMAGE_RECEIVING || offset != ota->snapshot.next_offset)
        return leave_operation(ota, status, ESY_ERR_STATE);
    if ((uintmax_t)length > ota->snapshot.total_bytes - ota->snapshot.next_offset)
        return leave_operation(ota, status, ESY_ERR_INVALID_ARG);
    result = checked_result(ota->config.provider.write(
        ota->config.provider.context, offset, data, length));
    if (result != ESY_OK)
        return leave_operation(ota, status, fail_session(ota, result));
    memcpy(ota->resources.last_chunk_buffer, data, length);
    ota->last_chunk_offset = offset;
    ota->last_chunk_length = length;
    ota->snapshot.next_offset += (uint32_t)length;
    result = complete_progress(ota);
    return leave_operation(ota, status, result);
}

esy_err_t esy_ota_finish(esy_ota_t *ota, uint32_t session_id,
                         esy_image_status_t *status) {
    esy_err_t result = enter_operation(ota, status);
    if (result != ESY_OK) return result;
    result = check_session(ota, session_id);
    if (result != ESY_OK) return leave_operation(ota, status, result);
    result = expire_if_due(ota);
    if (result != ESY_OK) return leave_operation(ota, status, result);
    if (ota->snapshot.phase == ESY_IMAGE_READY ||
        ota->snapshot.phase == ESY_IMAGE_ACTIVATED)
        return leave_operation(ota, status, ESY_OK);
    if (ota->snapshot.phase == ESY_IMAGE_FAILED)
        return leave_operation(ota, status, ota->snapshot.result);
    if (ota->snapshot.phase != ESY_IMAGE_RECEIVING ||
        ota->snapshot.next_offset != ota->snapshot.total_bytes)
        return leave_operation(ota, status, ESY_ERR_STATE);
    result = checked_result(ota->config.provider.finish(ota->config.provider.context));
    if (result != ESY_OK)
        return leave_operation(ota, status, fail_session(ota, result));
    result = complete_progress(ota);
    if (result == ESY_OK) ota->snapshot.phase = ESY_IMAGE_READY;
    return leave_operation(ota, status, result);
}

esy_err_t esy_ota_activate(esy_ota_t *ota, uint32_t session_id,
                           esy_image_status_t *status) {
    esy_err_t result = enter_operation(ota, status);
    if (result != ESY_OK) return result;
    result = check_session(ota, session_id);
    if (result != ESY_OK) return leave_operation(ota, status, result);
    result = expire_if_due(ota);
    if (result != ESY_OK) return leave_operation(ota, status, result);
    if (ota->snapshot.phase == ESY_IMAGE_ACTIVATED)
        return leave_operation(ota, status, ota->snapshot.result);
    if (ota->snapshot.phase != ESY_IMAGE_READY)
        return leave_operation(ota, status, ESY_ERR_STATE);
    if (ota->config.provider.activate == NULL)
        return leave_operation(ota, status, ESY_ERR_NOT_SUPPORTED);
    result = checked_result(ota->config.provider.activate(ota->config.provider.context));
    if (result != ESY_OK)
        return leave_operation(ota, status, fail_session(ota, result));
    ota->provider_owned = false;
    ota->deadline_active = false;
    ota->snapshot.phase = ESY_IMAGE_ACTIVATED;
    ota->snapshot.result_valid = true;
    ota->snapshot.result = cleanup(ota);
    return leave_operation(ota, status, ota->snapshot.result);
}

esy_err_t esy_ota_abort(esy_ota_t *ota, uint32_t session_id,
                        esy_image_status_t *status) {
    esy_err_t result = enter_operation(ota, status);
    if (result != ESY_OK) return result;
    result = check_session(ota, session_id);
    if (result != ESY_OK) return leave_operation(ota, status, result);
    if (ota->snapshot.phase == ESY_IMAGE_ACTIVATED)
        return leave_operation(ota, status, ESY_ERR_STATE);
    ota->deadline_active = false;
    result = cleanup(ota);
    ota->snapshot.phase = result == ESY_OK ? ESY_IMAGE_ABORTED : ESY_IMAGE_FAILED;
    ota->snapshot.result = result;
    ota->snapshot.result_valid = true;
    return leave_operation(ota, status, result);
}

esy_err_t esy_ota_status(esy_ota_t *ota, esy_image_status_t *status) {
    const esy_err_t result = enter_operation(ota, status);
    if (result != ESY_OK) return result;
    return leave_operation(ota, status, ESY_OK);
}

esy_err_t esy_ota_poll(esy_ota_t *ota) {
    if (ota == NULL) return ESY_ERR_INVALID_ARG;
    if (ota->busy) return ESY_ERR_BUSY;
    if (!ota->initialized) return ESY_ERR_STATE;
    ota->busy = true;
    const esy_err_t result = expire_if_due(ota);
    ota->busy = false;
    return result;
}
