/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_INTERNAL_H
#define ESY_REMOTE_MANAGER_INTERNAL_H

#include "esy_middleware/remote_manager/esy_remote_manager.h"

/* Module-private configuration dispatch; tested through the public manager. */
esy_err_t esy_remote_manager_config_execute(
    const esy_remote_manager_config_ops_t *ops,
    const esy_remote_manager_request_t *request, uint8_t *output,
    size_t capacity, size_t max_response_value_length,
    esy_remote_manager_response_t *response);

/* Module-private optional operation groups; no provider implementation types. */
esy_err_t esy_remote_manager_operations_execute(
    const esy_remote_manager_config_t *config,
    const esy_remote_manager_request_t *request, uint8_t *output,
    size_t capacity, esy_remote_manager_response_t *response);

/* Module-private typed statistics dispatch, independent of output encoding. */
esy_err_t esy_remote_manager_stats_execute(
    const esy_remote_manager_stats_ops_t *ops,
    const esy_remote_manager_request_t *request,
    esy_remote_manager_response_t *response);

/* Module-private structured diagnostics, independent of the shared core target. */
esy_err_t esy_remote_manager_diagnostics_execute(
    const esy_remote_manager_diagnostics_ops_t *ops,
    const esy_remote_manager_request_t *request,
    esy_remote_manager_response_t *response);

#endif /* ESY_REMOTE_MANAGER_INTERNAL_H */
