/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_logger_backend_uart_internal.h
 * @brief Internal and test-facing interfaces for the UART logger backend.
 *
 * This header is private to the logger module and its tests. It must not be
 * included by unrelated production modules.
 */

#ifndef ESY_LOGGER_BACKEND_UART_INTERNAL_H
#define ESY_LOGGER_BACKEND_UART_INTERNAL_H

#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/logger/esy_logger_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Try to send one queued UART log block.
 *
 * This is an internal state-machine step exposed for module tests that need to
 * verify DMA failure and DMA-busy cleanup paths without running the infinite
 * logger task loop.
 *
 * @param[in,out] backend Initialized UART logger backend.
 *
 * @retval true  A queued descriptor was processed or dropped.
 * @retval false No descriptor was processed, or @p backend is invalid.
 */
ESY_INTERNAL bool esy_logger_backend_uart_internal_try_kick_one(
    const esy_logger_backend_t *backend);

/**
 * @brief Return whether the internal UART backend context is initialized.
 *
 * @param[in] backend UART logger backend.
 *
 * @retval true  The backend has an active initialized context.
 * @retval false The backend is NULL, detached, or not initialized.
 */
ESY_INTERNAL bool esy_logger_backend_uart_internal_is_initialized(
    const esy_logger_backend_t *backend);

/**
 * @brief Return whether deinitialization has been requested internally.
 *
 * @param[in] backend UART logger backend.
 *
 * @retval true  The backend is waiting for deferred cleanup.
 * @retval false No deferred deinitialization is pending, or @p backend is invalid.
 */
ESY_INTERNAL bool esy_logger_backend_uart_internal_is_deinit_requested(
    const esy_logger_backend_t *backend);

/**
 * @brief Return whether the UART backend currently owns an active DMA send.
 *
 * @param[in] backend UART logger backend.
 *
 * @retval true  A DMA transfer is in progress.
 * @retval false No DMA transfer is in progress, or @p backend is invalid.
 */
ESY_INTERNAL bool esy_logger_backend_uart_internal_is_dma_busy(
    const esy_logger_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif /* ESY_LOGGER_BACKEND_UART_INTERNAL_H */
