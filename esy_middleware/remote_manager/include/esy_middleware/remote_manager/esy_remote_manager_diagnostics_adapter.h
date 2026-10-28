/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESY_REMOTE_MANAGER_DIAGNOSTICS_ADAPTER_H
#define ESY_REMOTE_MANAGER_DIAGNOSTICS_ADAPTER_H

#include "esy_middleware/remote_manager/esy_remote_manager_diagnostics.h"
#include "esy_middleware/diagnostics/esy_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Copied binding borrowing one initialized shared diagnostic inventory. */
typedef struct {
    esy_diagnostics_t *diagnostics; /**< Stable live core, outliving every adapter/ops borrower. */
} esy_remote_manager_diagnostics_adapter_config_t;

/**
 * @brief Caller-owned adapter requiring {0}; fields are private by contract.
 * Do not copy/move/reset while live. Manager, adapter, core and source access
 * share one serialized non-ISR owner. Stop all copied operations before detach.
 * The adapter owns no core/source lifecycle, authorization, timeout or retry.
 */
typedef struct {
    esy_diagnostics_t *diagnostics; /**< Private borrowed core. */
    bool initialized; /**< Private lifecycle flag. */
    bool busy; /**< Private active-operation guard, not a concurrency lock. */
} esy_remote_manager_diagnostics_adapter_t;

/** @brief Bind an initialized core without querying health or executing a test.
 * Validates the binding through metadata-only enumeration. Descriptors may
 * expire after return; the core/entries/names/contexts remain borrowed.
 * @return OK, INVALID_ARG for NULL, STATE for repeated/detached binding or a
 * core error such as BUSY. Active operations reject lifecycle reentry with BUSY.
 * Failure preserves the adapter. */
esy_err_t esy_remote_manager_diagnostics_adapter_init(
    esy_remote_manager_diagnostics_adapter_t *adapter,
    const esy_remote_manager_diagnostics_adapter_config_t *config);
/** @brief Copy ENUMERATE/READ/RUN routes; output unchanged on failure.
 * Global RUN discovery indicates route support; each source's capability mask
 * decides whether its named self-test exists. Output must not overlap adapter,
 * core, inventory or source-owned state; detectable handle overlap is rejected.
 * @return OK, INVALID_ARG, STATE or BUSY during an active operation. */
esy_err_t esy_remote_manager_diagnostics_adapter_get_ops(
    esy_remote_manager_diagnostics_adapter_t *adapter,
    esy_remote_manager_diagnostics_ops_t *ops);
/** @brief Detach after manager delivery stops; never deinitializes the core.
 * @return INVALID_ARG for NULL, BUSY during an active operation, otherwise OK
 * including repeated zero deinit. */
esy_err_t esy_remote_manager_diagnostics_adapter_deinit(
    esy_remote_manager_diagnostics_adapter_t *adapter);

#ifdef __cplusplus
}
#endif
#endif /* ESY_REMOTE_MANAGER_DIAGNOSTICS_ADAPTER_H */
