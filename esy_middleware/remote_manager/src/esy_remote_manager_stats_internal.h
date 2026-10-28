/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_STATS_INTERNAL_H
#define ESY_REMOTE_MANAGER_STATS_INTERNAL_H

#include "esy_middleware/remote_manager/esy_remote_manager_stats.h"

/* Shared pure validation inside this module; no public or test seam is needed. */
static inline bool esy_remote_manager_stats_name_valid(const char *name, size_t length) {
    if (name == NULL || length == 0u || length > ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES)
        return false;
    for (size_t i = 0u; i < length; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' || c == '/'))
            return false;
    }
    return true;
}

static inline bool esy_remote_manager_stats_type_valid(
    esy_remote_manager_stats_kind_t kind, esy_remote_manager_stats_unit_t unit) {
    return (kind == ESY_REMOTE_MANAGER_STATS_COUNTER ||
            kind == ESY_REMOTE_MANAGER_STATS_GAUGE) &&
           (unsigned)unit <= ESY_REMOTE_MANAGER_STATS_UNIT_PERCENT;
}

#endif /* ESY_REMOTE_MANAGER_STATS_INTERNAL_H */
