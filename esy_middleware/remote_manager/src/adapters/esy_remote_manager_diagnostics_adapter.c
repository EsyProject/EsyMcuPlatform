/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/remote_manager/esy_remote_manager_diagnostics_adapter.h"

static bool overlap(const void *left, size_t left_size, const void *right, size_t right_size) {
    const uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    return left_size > UINTPTR_MAX - a || right_size > UINTPTR_MAX - b ||
        (left_size != 0u && right_size != 0u && a < b + right_size && b < a + left_size);
}

static esy_err_t begin_operation(esy_remote_manager_diagnostics_adapter_t *adapter) {
    if (adapter == NULL || !adapter->initialized) return ESY_ERR_STATE;
    if (adapter->busy) return ESY_ERR_BUSY;
    adapter->busy = true;
    return ESY_OK;
}

static esy_err_t enumerate_source(void *context, uint32_t index,
                                  esy_diagnostics_snapshot_t *snapshot) {
    esy_remote_manager_diagnostics_adapter_t *adapter = context;
    const esy_err_t admission = begin_operation(adapter);
    if (admission != ESY_OK) return admission;
    const esy_err_t result = esy_diagnostics_enumerate(adapter->diagnostics, index, snapshot);
    adapter->busy = false;
    return result;
}

static esy_err_t read_source(void *context, const char *name, size_t length,
                             esy_diagnostics_snapshot_t *snapshot) {
    esy_remote_manager_diagnostics_adapter_t *adapter = context;
    const esy_err_t admission = begin_operation(adapter);
    if (admission != ESY_OK) return admission;
    const esy_err_t result = esy_diagnostics_read(adapter->diagnostics, name, length, snapshot);
    adapter->busy = false;
    return result;
}

static esy_err_t run_source(void *context, const char *name, size_t length,
                            esy_diagnostics_snapshot_t *snapshot) {
    esy_remote_manager_diagnostics_adapter_t *adapter = context;
    const esy_err_t admission = begin_operation(adapter);
    if (admission != ESY_OK) return admission;
    const esy_err_t result = esy_diagnostics_run(adapter->diagnostics, name, length, snapshot);
    adapter->busy = false;
    return result;
}

esy_err_t esy_remote_manager_diagnostics_adapter_init(
    esy_remote_manager_diagnostics_adapter_t *adapter,
    const esy_remote_manager_diagnostics_adapter_config_t *config) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->busy) return ESY_ERR_BUSY;
    if (adapter->initialized) return ESY_ERR_STATE;
    if (config == NULL || config->diagnostics == NULL) return ESY_ERR_INVALID_ARG;
    esy_diagnostics_snapshot_t descriptor;
    const esy_err_t result = esy_diagnostics_enumerate(config->diagnostics, 0u, &descriptor);
    if (result != ESY_OK) return result;
    adapter->diagnostics = config->diagnostics;
    adapter->initialized = true;
    return ESY_OK;
}

esy_err_t esy_remote_manager_diagnostics_adapter_get_ops(
    esy_remote_manager_diagnostics_adapter_t *adapter,
    esy_remote_manager_diagnostics_ops_t *ops) {
    if (adapter == NULL || ops == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->busy) return ESY_ERR_BUSY;
    if (!adapter->initialized) return ESY_ERR_STATE;
    if (overlap(ops, sizeof(*ops), adapter, sizeof(*adapter)) ||
        overlap(ops, sizeof(*ops), adapter->diagnostics, sizeof(*adapter->diagnostics)))
        return ESY_ERR_INVALID_ARG;
    *ops = (esy_remote_manager_diagnostics_ops_t){.enumerate = enumerate_source,
        .read = read_source, .run = run_source, .context = adapter};
    return ESY_OK;
}

esy_err_t esy_remote_manager_diagnostics_adapter_deinit(
    esy_remote_manager_diagnostics_adapter_t *adapter) {
    if (adapter == NULL) return ESY_ERR_INVALID_ARG;
    if (adapter->busy) return ESY_ERR_BUSY;
    *adapter = (esy_remote_manager_diagnostics_adapter_t){0};
    return ESY_OK;
}
