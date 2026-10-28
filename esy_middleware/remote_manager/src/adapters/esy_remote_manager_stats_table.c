/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/remote_manager/esy_remote_manager_stats_table.h"

#include <string.h>

#include "../esy_remote_manager_stats_internal.h"

static esy_err_t copy_entry(const esy_remote_manager_stats_table_t *table,
                             uint32_t index,
                             esy_remote_manager_stats_snapshot_t *snapshot) {
    const esy_remote_manager_stats_entry_t *entry = &table->config.entries[index];
    esy_remote_manager_stats_sample_t sample = *entry->sample;
    if (!sample.available) sample.value = 0u;
    if (sample.available && entry->unit == ESY_REMOTE_MANAGER_STATS_UNIT_PERCENT &&
        sample.value > 100u) return ESY_ERR_STATE;
    *snapshot = (esy_remote_manager_stats_snapshot_t){
        .count = (uint32_t)table->config.count, .index = index,
        .name_length = entry->name_length, .kind = entry->kind,
        .unit = entry->unit, .sample = sample};
    memcpy(snapshot->name, entry->name, entry->name_length);
    return ESY_OK;
}

static esy_err_t enumerate_metric(void *context, uint32_t index,
                                  esy_remote_manager_stats_snapshot_t *snapshot) {
    if (context == NULL || snapshot == NULL) return ESY_ERR_INVALID_ARG;
    const esy_remote_manager_stats_table_t *table = context;
    if (!table->initialized) return ESY_ERR_STATE;
    if (table->config.count == 0u && index == 0u) {
        *snapshot = (esy_remote_manager_stats_snapshot_t){0};
        return ESY_OK;
    }
    if (index >= table->config.count) return ESY_ERR_INVALID_ARG;
    return copy_entry(table, index, snapshot);
}

static esy_err_t read_metric(void *context, const char *name, size_t length,
                             esy_remote_manager_stats_snapshot_t *snapshot) {
    if (context == NULL || snapshot == NULL ||
        !esy_remote_manager_stats_name_valid(name, length)) return ESY_ERR_INVALID_ARG;
    const esy_remote_manager_stats_table_t *table = context;
    if (!table->initialized) return ESY_ERR_STATE;
    for (size_t i = 0u; i < table->config.count; ++i) {
        const esy_remote_manager_stats_entry_t *entry = &table->config.entries[i];
        if (length == entry->name_length && memcmp(name, entry->name, length) == 0)
            return copy_entry(table, (uint32_t)i, snapshot);
    }
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_remote_manager_stats_table_init(
    esy_remote_manager_stats_table_t *table,
    const esy_remote_manager_stats_table_config_t *config) {
    if (table == NULL) return ESY_ERR_INVALID_ARG;
    if (table->initialized) return ESY_ERR_STATE;
    if (config == NULL || (config->entries == NULL && config->count != 0u) ||
        config->count > SIZE_MAX / sizeof(*config->entries))
        return ESY_ERR_INVALID_ARG;
#if SIZE_MAX > UINT32_MAX
    if (config->count > UINT32_MAX) return ESY_ERR_INVALID_ARG;
#endif
    for (size_t i = 0u; i < config->count; ++i) {
        const esy_remote_manager_stats_entry_t *entry = &config->entries[i];
        if (!esy_remote_manager_stats_name_valid(entry->name, entry->name_length) ||
            !esy_remote_manager_stats_type_valid(entry->kind, entry->unit) ||
            entry->sample == NULL)
            return ESY_ERR_INVALID_ARG;
        for (size_t j = 0u; j < i; ++j) {
            if (entry->name_length == config->entries[j].name_length &&
                memcmp(entry->name, config->entries[j].name, entry->name_length) == 0)
                return ESY_ERR_INVALID_ARG;
        }
    }
    table->config = *config;
    table->initialized = true;
    return ESY_OK;
}

esy_err_t esy_remote_manager_stats_table_get_ops(
    esy_remote_manager_stats_table_t *table, esy_remote_manager_stats_ops_t *ops) {
    if (table == NULL || ops == NULL) return ESY_ERR_INVALID_ARG;
    if (!table->initialized) return ESY_ERR_STATE;
    *ops = (esy_remote_manager_stats_ops_t){
        .enumerate = enumerate_metric, .read = read_metric, .context = table};
    return ESY_OK;
}

esy_err_t esy_remote_manager_stats_table_deinit(esy_remote_manager_stats_table_t *table) {
    if (table == NULL) return ESY_ERR_INVALID_ARG;
    *table = (esy_remote_manager_stats_table_t){0};
    return ESY_OK;
}
