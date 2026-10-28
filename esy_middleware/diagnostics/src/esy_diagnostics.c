/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/diagnostics/esy_diagnostics.h"

#include <string.h>

static bool range_valid(const void *data, size_t length) {
    return length <= UINTPTR_MAX - (uintptr_t)data;
}

static bool overlaps(const void *a, size_t an, const void *b, size_t bn) {
    if (an == 0u || bn == 0u) return false;
    const uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
    return aa < bb + bn && bb < aa + an;
}

static bool name_character(unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           character == '_' || character == '.' || character == '-' || character == '/';
}

static bool lookup_valid(const char *name, size_t length) {
    if (name == NULL || length == 0u || length > ESY_DIAGNOSTICS_NAME_MAX_BYTES ||
        !range_valid(name, length)) return false;
    for (size_t i = 0u; i < length; ++i)
        if (!name_character((unsigned char)name[i])) return false;
    return true;
}

static bool registered_length(const char *name, size_t *length) {
    if (name == NULL || !range_valid(name, ESY_DIAGNOSTICS_NAME_MAX_BYTES + 1u))
        return false;
    for (size_t i = 0u; i < ESY_DIAGNOSTICS_NAME_MAX_BYTES; ++i) {
        const unsigned char character = (unsigned char)name[i];
        if (character == '\0') {
            *length = i;
            return i != 0u;
        }
        if (!name_character(character)) return false;
    }
    if (name[ESY_DIAGNOSTICS_NAME_MAX_BYTES] != '\0') return false;
    *length = ESY_DIAGNOSTICS_NAME_MAX_BYTES;
    return true;
}

static esy_err_t ready(esy_diagnostics_t *diagnostics) {
    if (diagnostics == NULL || !range_valid(diagnostics, sizeof(*diagnostics)))
        return ESY_ERR_INVALID_ARG;
    if (diagnostics->busy) return ESY_ERR_BUSY;
    return diagnostics->initialized ? ESY_OK : ESY_ERR_STATE;
}

static esy_err_t output_valid(const esy_diagnostics_t *diagnostics,
                              const char *lookup, size_t lookup_length,
                              const esy_diagnostics_snapshot_t *snapshot) {
    if (snapshot == NULL || !range_valid(snapshot, sizeof(*snapshot)) ||
        overlaps(snapshot, sizeof(*snapshot), diagnostics, sizeof(*diagnostics)) ||
        overlaps(snapshot, sizeof(*snapshot), diagnostics->resources.entries,
                 diagnostics->resources.count * sizeof(*diagnostics->resources.entries)) ||
        overlaps(snapshot, sizeof(*snapshot), lookup, lookup_length))
        return ESY_ERR_INVALID_ARG;
    for (size_t i = 0u; i < diagnostics->resources.count; ++i) {
        const char *name = diagnostics->resources.entries[i].name;
        size_t length;
        if (!registered_length(name, &length)) return ESY_ERR_STATE;
        if (overlaps(snapshot, sizeof(*snapshot), name, length + 1u))
            return ESY_ERR_INVALID_ARG;
    }
    return ESY_OK;
}

static void descriptor(const esy_diagnostics_t *diagnostics, uint32_t index,
                        size_t length, esy_diagnostics_snapshot_t *snapshot) {
    const esy_diagnostics_entry_t *entry = &diagnostics->resources.entries[index];
    *snapshot = (esy_diagnostics_snapshot_t){
        .count = (uint32_t)diagnostics->resources.count, .index = index,
        .capabilities = ESY_DIAGNOSTICS_CAP_READ |
            (entry->run != NULL ? ESY_DIAGNOSTICS_CAP_RUN : 0u),
        .name_length = length};
    memcpy(snapshot->name, entry->name, length);
}

static bool result_valid(const esy_diagnostics_result_t *result, bool run) {
    if ((unsigned)result->health > ESY_DIAGNOSTICS_HEALTH_ERROR ||
        (run && result->health == ESY_DIAGNOSTICS_HEALTH_UNKNOWN) ||
        result->detail_length > ESY_DIAGNOSTICS_DETAIL_MAX_BYTES)
        return false;
    if (result->detail[result->detail_length] != '\0') return false;
    for (size_t i = 0u; i < result->detail_length; ++i) {
        const unsigned char character = (unsigned char)result->detail[i];
        if (character < 0x20u || character > 0x7eu) return false;
    }
    return true;
}

static esy_err_t assess(esy_diagnostics_t *diagnostics,
                         const char *name, size_t name_length,
                         esy_diagnostics_snapshot_t *snapshot, bool run) {
    esy_err_t status = ready(diagnostics);
    if (status != ESY_OK) return status;
    if (!lookup_valid(name, name_length)) return ESY_ERR_INVALID_ARG;
    status = output_valid(diagnostics, name, name_length, snapshot);
    if (status != ESY_OK) return status;
    for (size_t i = 0u; i < diagnostics->resources.count; ++i) {
        const esy_diagnostics_entry_t *entry = &diagnostics->resources.entries[i];
        size_t length;
        if (!registered_length(entry->name, &length)) return ESY_ERR_STATE;
        if (length != name_length || memcmp(entry->name, name, length) != 0) continue;
        esy_err_t (*callback)(void *, esy_diagnostics_result_t *) = run ? entry->run : entry->read;
        if (callback == NULL) return ESY_ERR_NOT_SUPPORTED;
        esy_diagnostics_result_t result = {0};
        diagnostics->busy = true;
        status = callback(entry->context, &result);
        if (status > ESY_OK || (status == ESY_OK && !result_valid(&result, run)))
            status = ESY_ERR_STATE;
        if (status == ESY_OK) {
            esy_diagnostics_snapshot_t output;
            descriptor(diagnostics, (uint32_t)i, length, &output);
            output.result = result;
            *snapshot = output;
        }
        diagnostics->busy = false;
        return status;
    }
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_diagnostics_init(esy_diagnostics_t *diagnostics,
                              const esy_diagnostics_config_t *config) {
    if (diagnostics == NULL || !range_valid(diagnostics, sizeof(*diagnostics)))
        return ESY_ERR_INVALID_ARG;
    if (diagnostics->busy) return ESY_ERR_BUSY;
    if (diagnostics->initialized) return ESY_ERR_STATE;
    if (config == NULL || !range_valid(config, sizeof(*config)) ||
        config->static_resources == NULL ||
        !range_valid(config->static_resources, sizeof(*config->static_resources)))
        return ESY_ERR_INVALID_ARG;
    const esy_diagnostics_static_resources_t resources = *config->static_resources;
    if ((resources.entries == NULL && resources.count != 0u) ||
        resources.count > SIZE_MAX / sizeof(*resources.entries))
        return ESY_ERR_INVALID_ARG;
#if SIZE_MAX > UINT32_MAX
    if (resources.count > UINT32_MAX) return ESY_ERR_INVALID_ARG;
#endif
    const size_t inventory_bytes = resources.count * sizeof(*resources.entries);
    if (!range_valid(resources.entries, inventory_bytes) ||
        overlaps(diagnostics, sizeof(*diagnostics), resources.entries, inventory_bytes))
        return ESY_ERR_INVALID_ARG;
    for (size_t i = 0u; i < resources.count; ++i) {
        const esy_diagnostics_entry_t *entry = &resources.entries[i];
        size_t length;
        if (entry->read == NULL || !registered_length(entry->name, &length) ||
            overlaps(diagnostics, sizeof(*diagnostics), entry->name, length + 1u))
            return ESY_ERR_INVALID_ARG;
        for (size_t j = 0u; j < i; ++j) {
            size_t previous_length;
            if (!registered_length(resources.entries[j].name, &previous_length))
                return ESY_ERR_INVALID_ARG;
            if (length == previous_length &&
                memcmp(entry->name, resources.entries[j].name, length) == 0)
                return ESY_ERR_INVALID_ARG;
        }
    }
    diagnostics->resources = resources;
    diagnostics->initialized = true;
    return ESY_OK;
}

esy_err_t esy_diagnostics_deinit(esy_diagnostics_t *diagnostics) {
    if (diagnostics == NULL || !range_valid(diagnostics, sizeof(*diagnostics)))
        return ESY_ERR_INVALID_ARG;
    if (diagnostics->busy) return ESY_ERR_BUSY;
    *diagnostics = (esy_diagnostics_t){0};
    return ESY_OK;
}

esy_err_t esy_diagnostics_enumerate(esy_diagnostics_t *diagnostics, uint32_t index,
                                   esy_diagnostics_snapshot_t *snapshot) {
    esy_err_t status = ready(diagnostics);
    if (status != ESY_OK) return status;
    status = output_valid(diagnostics, NULL, 0u, snapshot);
    if (status != ESY_OK) return status;
    if (diagnostics->resources.count == 0u && index == 0u) {
        *snapshot = (esy_diagnostics_snapshot_t){0};
        return ESY_OK;
    }
    if (index >= diagnostics->resources.count) return ESY_ERR_INVALID_ARG;
    size_t length;
    if (!registered_length(diagnostics->resources.entries[index].name, &length))
        return ESY_ERR_STATE;
    esy_diagnostics_snapshot_t output;
    descriptor(diagnostics, index, length, &output);
    *snapshot = output;
    return ESY_OK;
}

esy_err_t esy_diagnostics_read(esy_diagnostics_t *diagnostics,
                              const char *name, size_t name_length,
                              esy_diagnostics_snapshot_t *snapshot) {
    return assess(diagnostics, name, name_length, snapshot, false);
}

esy_err_t esy_diagnostics_run(esy_diagnostics_t *diagnostics,
                             const char *name, size_t name_length,
                             esy_diagnostics_snapshot_t *snapshot) {
    return assess(diagnostics, name, name_length, snapshot, true);
}
