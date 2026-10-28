/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/power_manager/esy_power_hal_backend.h"

#include <string.h>

#include "esy_hal/esy_hal_system.h"

static esy_err_t platform_backend_get_capabilities(
    void *backend_ctx, esy_power_capabilities_t *out_capabilities) {
  esy_power_hal_backend_t *backend = backend_ctx;
  if (backend == NULL) return ESY_ERR_INVALID_ARG;
  if (!backend->initialized) return ESY_ERR_STATE;
  return esy_hal_power_get_capabilities(out_capabilities);
}

static esy_err_t platform_backend_begin(void *backend_ctx,
                                        esy_power_state_t state) {
  esy_power_hal_backend_t *backend = backend_ctx;
  if (backend == NULL) return ESY_ERR_INVALID_ARG;
  if (!backend->initialized) return ESY_ERR_STATE;
  if (backend->active) return ESY_ERR_BUSY;
  esy_err_t ret = esy_hal_power_transaction_begin(&backend->transaction, state);
  if (ret == ESY_OK) backend->active = true;
  return ret;
}

static esy_err_t platform_backend_enter_atomic(
    void *backend_ctx, esy_power_backend_result_t *out_result) {
  esy_power_hal_backend_t *backend = backend_ctx;
  esy_err_t ret;
  if ((backend == NULL) || (out_result == NULL)) return ESY_ERR_INVALID_ARG;
  if (!backend->initialized || !backend->active) return ESY_ERR_STATE;
  ret = esy_hal_power_transaction_enter_atomic(&backend->transaction);
  out_result->entered = backend->transaction.result.entered;
  out_result->entry_status = backend->transaction.result.entry_status;
  out_result->fatal_error = backend->transaction.result.fatal_error;
  return ret;
}

static esy_err_t platform_backend_complete(void *backend_ctx) {
  esy_power_hal_backend_t *backend = backend_ctx;
  if (backend == NULL) return ESY_ERR_INVALID_ARG;
  if (!backend->initialized) return ESY_ERR_STATE;
  if (!backend->active) return ESY_ERR_STATE;
  esy_err_t ret = esy_hal_power_transaction_complete(&backend->transaction,
                                                        &backend->result);
  if (ret == ESY_OK) backend->active = false;
  return ret;
}

static esy_err_t platform_backend_abort(void *backend_ctx) {
  esy_power_hal_backend_t *backend = backend_ctx;
  if (backend == NULL) return ESY_ERR_INVALID_ARG;
  if (!backend->initialized) return ESY_ERR_STATE;
  if (!backend->active) return ESY_ERR_STATE;
  esy_err_t ret = esy_hal_power_transaction_abort(&backend->transaction);
  if (ret == ESY_OK) backend->active = false;
  return ret;
}

static const esy_power_backend_ops_t k_platform_backend_ops = {
    .get_capabilities = platform_backend_get_capabilities,
    .begin = platform_backend_begin,
    .enter_atomic = platform_backend_enter_atomic,
    .complete = platform_backend_complete,
    .abort = platform_backend_abort,
};

static esy_err_t platform_clocksource_get_supported_state_mask(
    void *ctx, uint32_t *out_state_mask) {
  (void)ctx;
  if (out_state_mask == NULL) return ESY_ERR_INVALID_ARG;
  *out_state_mask = esy_hal_system_always_on_state_mask();
  return ESY_OK;
}

static esy_err_t platform_clocksource_read_atomic(void *ctx,
                                                  uint64_t *out_time_us) {
  (void)ctx;
  return esy_hal_system_always_on_time_us(out_time_us);
}

static const esy_power_clocksource_ops_t k_platform_clocksource_ops = {
    .get_supported_state_mask = platform_clocksource_get_supported_state_mask,
};

static const esy_monotonic_clock_ops_t k_platform_monotonic_clock_ops = {
    .read_atomic = platform_clocksource_read_atomic,
};

esy_err_t esy_power_hal_backend_init(esy_power_hal_backend_t *backend) {
  if (backend == NULL) return ESY_ERR_INVALID_ARG;
  if (backend->initialized) return ESY_ERR_STATE;
  memset(backend, 0, sizeof(*backend));
  esy_hal_power_transaction_init(&backend->transaction);
  backend->initialized = true;
  return ESY_OK;
}

esy_err_t esy_power_hal_backend_deinit(esy_power_hal_backend_t *backend) {
  if (backend == NULL) return ESY_ERR_INVALID_ARG;
  if (!backend->initialized) return ESY_ERR_STATE;
  if (backend->active) return ESY_ERR_BUSY;
  memset(backend, 0, sizeof(*backend));
  return ESY_OK;
}

const esy_power_backend_ops_t *esy_power_hal_backend_ops(void) {
  return &k_platform_backend_ops;
}

const esy_power_clocksource_ops_t *esy_power_platform_clocksource_ops(void) {
  return &k_platform_clocksource_ops;
}

const esy_monotonic_clock_ops_t *esy_power_platform_monotonic_clock_ops(void) {
  return &k_platform_monotonic_clock_ops;
}
