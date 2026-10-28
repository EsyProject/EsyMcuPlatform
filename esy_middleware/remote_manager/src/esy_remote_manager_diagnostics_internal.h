/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESY_REMOTE_MANAGER_DIAGNOSTICS_INTERNAL_H
#define ESY_REMOTE_MANAGER_DIAGNOSTICS_INTERNAL_H

#include "esy_middleware/remote_manager/esy_remote_manager_diagnostics.h"

/* Shared pure validation within this module; no public/test seam is needed. */
static inline bool esy_remote_manager_diagnostics_name_valid(const char *name, size_t length) {
    if (name == NULL || length == 0u || length > ESY_DIAGNOSTICS_NAME_MAX_BYTES)
        return false;
    for (size_t i = 0u; i < length; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' || c == '/'))
            return false;
    }
    return true;
}

#endif /* ESY_REMOTE_MANAGER_DIAGNOSTICS_INTERNAL_H */
