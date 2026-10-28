/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_BACKEND_RTT_INTERNAL_H
#define ESY_LOGGER_BACKEND_RTT_INTERNAL_H
#include "esy_middleware/logger/esy_logger_backend_rtt.h"
#include "esy_logger_backend_runtime_internal.h"
esy_err_t esy_logger_backend_rtt_init_with_runtime(esy_logger_backend_t *backend,
    const esy_logger_backend_rtt_config_t *config, const esy_logger_backend_runtime_t *runtime);
#endif /* ESY_LOGGER_BACKEND_RTT_INTERNAL_H */
