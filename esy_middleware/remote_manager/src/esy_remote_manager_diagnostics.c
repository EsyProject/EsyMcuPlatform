/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_remote_manager_internal.h"
#include "esy_remote_manager_diagnostics_internal.h"

#include <string.h>

static bool result_empty(const esy_diagnostics_result_t *result) {
    if (result->health != ESY_DIAGNOSTICS_HEALTH_UNKNOWN || result->code != 0u ||
        result->detail_length != 0u) return false;
    for (size_t i = 0u; i < sizeof(result->detail); ++i)
        if (result->detail[i] != '\0') return false;
    return true;
}

static bool result_valid(const esy_diagnostics_result_t *result) {
    if ((unsigned)result->health > ESY_DIAGNOSTICS_HEALTH_ERROR ||
        result->detail_length > ESY_DIAGNOSTICS_DETAIL_MAX_BYTES ||
        result->detail[result->detail_length] != '\0') return false;
    for (size_t i = 0u; i < result->detail_length; ++i) {
        const unsigned char c = (unsigned char)result->detail[i];
        if (c < 0x20u || c > 0x7eu) return false;
    }
    return true;
}

static bool snapshot_valid(const esy_diagnostics_snapshot_t *snapshot) {
    if (snapshot->count == 0u) {
        if (snapshot->index != 0u || snapshot->capabilities != 0u ||
            snapshot->name_length != 0u || !result_empty(&snapshot->result)) return false;
        for (size_t i = 0u; i < sizeof(snapshot->name); ++i)
            if (snapshot->name[i] != '\0') return false;
        return true;
    }
    const uint32_t known = ESY_DIAGNOSTICS_CAP_READ | ESY_DIAGNOSTICS_CAP_RUN;
    return snapshot->index < snapshot->count &&
        (snapshot->capabilities & ESY_DIAGNOSTICS_CAP_READ) != 0u &&
        (snapshot->capabilities & ~known) == 0u &&
        esy_remote_manager_diagnostics_name_valid(snapshot->name, snapshot->name_length) &&
        snapshot->name[snapshot->name_length] == '\0' && result_valid(&snapshot->result);
}

esy_err_t esy_remote_manager_diagnostics_execute(
    const esy_remote_manager_diagnostics_ops_t *ops,
    const esy_remote_manager_request_t *request,
    esy_remote_manager_response_t *response) {
    esy_diagnostics_snapshot_t snapshot = {0};
    esy_err_t result = ESY_ERR_NOT_SUPPORTED;
    switch (request->command) {
    case ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE:
        if (ops->enumerate != NULL)
            result = ops->enumerate(ops->context, request->diagnostic_index, &snapshot);
        break;
    case ESY_REMOTE_MANAGER_DIAGNOSTICS_READ:
        if (ops->read != NULL)
            result = ops->read(ops->context, request->key, request->key_length, &snapshot);
        break;
    case ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN:
        if (ops->run != NULL)
            result = ops->run(ops->context, request->key, request->key_length, &snapshot);
        break;
    default:
        break;
    }
    if (result != ESY_OK) return result > ESY_OK ? ESY_ERR_STATE : result;
    if (!snapshot_valid(&snapshot)) return ESY_ERR_STATE;
    if (request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE) {
        if (snapshot.index != request->diagnostic_index || !result_empty(&snapshot.result))
            return ESY_ERR_STATE;
    } else {
        if (snapshot.count == 0u || snapshot.name_length != request->key_length ||
            memcmp(snapshot.name, request->key, request->key_length) != 0)
            return ESY_ERR_STATE;
        if (request->command == ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN &&
            ((snapshot.capabilities & ESY_DIAGNOSTICS_CAP_RUN) == 0u ||
             snapshot.result.health == ESY_DIAGNOSTICS_HEALTH_UNKNOWN)) return ESY_ERR_STATE;
    }
    response->diagnostic = snapshot;
    return ESY_OK;
}
