/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_remote_manager_internal.h"
#include "esy_remote_manager_stats_internal.h"

#include <string.h>

static bool empty_snapshot(const esy_remote_manager_stats_snapshot_t *snapshot) {
    if (snapshot->index != 0u || snapshot->name_length != 0u ||
        snapshot->kind != 0 || snapshot->unit != 0 || snapshot->sample.available ||
        snapshot->sample.epoch != 0u || snapshot->sample.value != 0u)
        return false;
    for (size_t i = 0u; i < sizeof(snapshot->name); ++i)
        if (snapshot->name[i] != '\0') return false;
    return true;
}

static bool snapshot_valid(const esy_remote_manager_stats_snapshot_t *snapshot) {
    if (snapshot->count == 0u) return empty_snapshot(snapshot);
    return snapshot->index < snapshot->count &&
        esy_remote_manager_stats_name_valid(snapshot->name, snapshot->name_length) &&
        snapshot->name[snapshot->name_length] == '\0' &&
        esy_remote_manager_stats_type_valid(snapshot->kind, snapshot->unit) &&
        (snapshot->sample.available || snapshot->sample.value == 0u) &&
        (!snapshot->sample.available ||
         snapshot->unit != ESY_REMOTE_MANAGER_STATS_UNIT_PERCENT ||
         snapshot->sample.value <= 100u);
}

esy_err_t esy_remote_manager_stats_execute(
    const esy_remote_manager_stats_ops_t *ops,
    const esy_remote_manager_request_t *request,
    esy_remote_manager_response_t *response) {
    esy_remote_manager_stats_snapshot_t snapshot = {0};
    esy_err_t result = ESY_ERR_NOT_SUPPORTED;
    if (request->command == ESY_REMOTE_MANAGER_STATS_ENUMERATE && ops->enumerate != NULL)
        result = ops->enumerate(ops->context, request->stat_index, &snapshot);
    else if (request->command == ESY_REMOTE_MANAGER_STATS_READ && ops->read != NULL)
        result = ops->read(ops->context, request->key, request->key_length, &snapshot);
    if (result != ESY_OK) return result;
    if (!snapshot_valid(&snapshot)) return ESY_ERR_STATE;
    if (request->command == ESY_REMOTE_MANAGER_STATS_ENUMERATE) {
        if (snapshot.index != request->stat_index) return ESY_ERR_STATE;
    } else if (snapshot.count == 0u || snapshot.name_length != request->key_length ||
               memcmp(snapshot.name, request->key, request->key_length) != 0) {
        return ESY_ERR_STATE;
    }
    response->stats = snapshot;
    return ESY_OK;
}
