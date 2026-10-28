/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_BACKEND_CORE_INTERNAL_H
#define ESY_LOGGER_BACKEND_CORE_INTERNAL_H
#include "esy_logger_backend_runtime_internal.h"
#include "esy_middleware/logger/esy_logger_backend_uart_core.h"
#include "esy_middleware/logger/esy_logger_backend_printf_core.h"
esy_err_t esy_logger_backend_uart_init_with_runtime(esy_logger_backend_t *backend,
    const esy_logger_backend_uart_core_config_t *config,
    const esy_logger_backend_runtime_t *runtime, const void *worker_config);
bool esy_logger_backend_uart_run_one(void *ctx);
esy_err_t esy_logger_backend_printf_init_with_runtime(esy_logger_backend_t *backend,
    const esy_logger_backend_printf_core_config_t *config,
    const esy_logger_backend_runtime_t *runtime, const void *worker_config);
bool esy_logger_backend_printf_run_one(void *ctx);
bool esy_logger_backend_uart_retry_pending(void *ctx);
bool esy_logger_backend_uart_stop_requested(void *ctx);
bool esy_logger_backend_printf_stop_requested(void *ctx);
#endif /* ESY_LOGGER_BACKEND_CORE_INTERNAL_H */
