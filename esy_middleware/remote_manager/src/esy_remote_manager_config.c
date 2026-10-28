/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_remote_manager_internal.h"

static esy_err_t config_get(const esy_remote_manager_config_ops_t *ops,
                            const esy_remote_manager_request_t *request,
                            uint8_t *output, size_t capacity,
                            size_t max_response_value_length,
                            esy_remote_manager_response_t *response) {
    if (ops->get == NULL) return ESY_ERR_NOT_SUPPORTED;
    size_t length = 0u;
    bool found = false;
    esy_err_t status = ops->get(ops->context, request->key, request->key_length,
                               output, capacity, &length, &found);
    if (status == ESY_OK || status == ESY_ERR_NO_MEM) {
        if (length > max_response_value_length || (!found && length != 0u) ||
            (status == ESY_OK && length > capacity) ||
            (status == ESY_ERR_NO_MEM && (!found || length <= capacity))) {
            return ESY_ERR_STATE;
        }
        response->value_length = length;
        response->found = found;
    }
    return status;
}

esy_err_t esy_remote_manager_config_execute(
    const esy_remote_manager_config_ops_t *ops,
    const esy_remote_manager_request_t *request, uint8_t *output,
    size_t capacity, size_t max_response_value_length,
    esy_remote_manager_response_t *response) {
    switch (request->command) {
    case ESY_REMOTE_MANAGER_CONFIG_GET:
        return config_get(ops, request, output, capacity, max_response_value_length,
                          response);
    case ESY_REMOTE_MANAGER_CONFIG_SET:
        return ops->set != NULL
                   ? ops->set(ops->context, request->key, request->key_length,
                              request->value, request->value_length)
                   : ESY_ERR_NOT_SUPPORTED;
    case ESY_REMOTE_MANAGER_CONFIG_REMOVE:
        return ops->remove != NULL
                   ? ops->remove(ops->context, request->key, request->key_length)
                   : ESY_ERR_NOT_SUPPORTED;
    case ESY_REMOTE_MANAGER_CONFIG_APPLY:
        return ops->apply != NULL ? ops->apply(ops->context)
                                  : ESY_ERR_NOT_SUPPORTED;
    case ESY_REMOTE_MANAGER_CONFIG_SAVE:
        return ops->save != NULL ? ops->save(ops->context)
                                 : ESY_ERR_NOT_SUPPORTED;
    default:
        return ESY_ERR_NOT_SUPPORTED;
    }
}
