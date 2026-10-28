/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/power_manager/esy_power_tickless_adapter.h"

#include <string.h>

#define ESY_POWER_TICKLESS_ADAPTER_MAGIC 0x45535441u

static bool adapter_is_valid(const esy_power_tickless_adapter_t *adapter) {
  return (adapter != NULL) &&
         (adapter->magic == ESY_POWER_TICKLESS_ADAPTER_MAGIC) &&
         (adapter->manager != NULL);
}

static esy_err_t tickless_prepare(void *arg, uint64_t maximum_sleep_us) {
  esy_power_tickless_adapter_t *adapter = arg;
  esy_power_request_t request;
  uint64_t now_us;
  esy_err_t ret;
  if (!adapter_is_valid(adapter)) return ESY_ERR_INVALID_ARG;
  if (adapter->prepared) return ESY_ERR_STATE;
  ret = esy_power_manager_now_atomic(adapter->manager, &now_us);
  if (ret != ESY_OK) return ret;
  esy_power_request_default(&request);
  request.latest_resume_time_us = now_us > (UINT64_MAX - maximum_sleep_us)
                                      ? UINT64_MAX
                                      : now_us + maximum_sleep_us;
  request.maximum_exit_latency_us = adapter->maximum_exit_latency_us;
  request.late_resume_guard_us = adapter->late_resume_guard_us;
  request.allowed_state_mask = adapter->allowed_state_mask;
  request.required_wake_source_mask = adapter->required_wake_source_mask;
  ret = esy_power_transaction_prepare(adapter->manager, &adapter->transaction,
                                      &request);
  if ((ret == ESY_OK) ||
      (adapter->transaction.result.recovery_error != ESY_OK))
    adapter->prepared = true;
  return ret;
}

static esy_err_t tickless_check_prepare_recovery(void *arg) {
  esy_power_tickless_adapter_t *adapter = arg;
  if (!adapter_is_valid(adapter) || adapter->prepared ||
      adapter->transaction.result.fatal)
    return ESY_ERR_STATE;
  return adapter->transaction.result.recovery_error;
}

static esy_err_t tickless_read_clock_atomic(void *arg, uint64_t *out_time_us) {
  esy_power_tickless_adapter_t *adapter = arg;
  if (!adapter_is_valid(adapter)) return ESY_ERR_INVALID_ARG;
  return esy_power_manager_now_atomic(adapter->manager, out_time_us);
}

static esy_err_t tickless_enter_atomic(void *arg,
                                       esy_os_tickless_result_t *out_result) {
  esy_power_tickless_adapter_t *adapter = arg;
  esy_err_t ret;
  if (!adapter_is_valid(adapter) || (out_result == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!adapter->prepared) return ESY_ERR_STATE;
  ret = esy_power_transaction_enter_atomic(&adapter->transaction);
  out_result->committed = adapter->transaction.device_graph_committed;
  out_result->entered = adapter->transaction.result.entered;
  out_result->elapsed_valid = adapter->transaction.result.elapsed_valid;
  out_result->elapsed_us = adapter->transaction.result.elapsed_us;
  out_result->fatal = adapter->transaction.result.fatal;
  out_result->fatal_error = adapter->transaction.result.fatal_error;
  return ret;
}

static esy_err_t tickless_recover_atomic(void *arg) {
  esy_power_tickless_adapter_t *adapter = arg;
  if (!adapter_is_valid(adapter) || !adapter->prepared) {
    return ESY_ERR_INVALID_ARG;
  }
  return esy_power_transaction_recover_atomic(&adapter->transaction);
}

static esy_err_t tickless_complete(void *arg) {
  esy_power_tickless_adapter_t *adapter = arg;
  esy_err_t ret;
  if (!adapter_is_valid(adapter) || !adapter->prepared) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = esy_power_transaction_complete(&adapter->transaction, &adapter->result);
  if (ret == ESY_OK) adapter->prepared = false;
  return ret;
}

static esy_err_t tickless_abort(void *arg) {
  esy_power_tickless_adapter_t *adapter = arg;
  esy_err_t ret;
  if (!adapter_is_valid(adapter) || !adapter->prepared) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = esy_power_transaction_abort(&adapter->transaction, &adapter->result);
  if (ret == ESY_OK) adapter->prepared = false;
  return ret;
}

static const esy_os_tickless_ops_t k_tickless_ops = {
    .prepare = tickless_prepare,
    .enter_atomic = tickless_enter_atomic,
    .recover_atomic = tickless_recover_atomic,
    .complete = tickless_complete,
    .abort = tickless_abort,
    .check_prepare_recovery = tickless_check_prepare_recovery,
    .read_clock_atomic = tickless_read_clock_atomic,
};

esy_err_t esy_power_tickless_adapter_init(
    esy_power_tickless_adapter_t *adapter,
    const esy_power_tickless_adapter_config_t *config) {
  if ((adapter == NULL) || (config == NULL) || (config->manager == NULL) ||
      ((config->allowed_state_mask &
        ~(ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE) |
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION))) != 0u)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (adapter->magic != 0u) return ESY_ERR_STATE;
  memset(adapter, 0, sizeof(*adapter));
  adapter->manager = config->manager;
  adapter->maximum_exit_latency_us = config->maximum_exit_latency_us;
  adapter->late_resume_guard_us = config->late_resume_guard_us;
  adapter->allowed_state_mask = config->allowed_state_mask;
  adapter->required_wake_source_mask = config->required_wake_source_mask;
  esy_power_transaction_init(&adapter->transaction);
  adapter->magic = ESY_POWER_TICKLESS_ADAPTER_MAGIC;
  return ESY_OK;
}

esy_err_t esy_power_tickless_adapter_deinit(
    esy_power_tickless_adapter_t *adapter) {
  if (!adapter_is_valid(adapter)) return ESY_ERR_INVALID_ARG;
  if (adapter->prepared) return ESY_ERR_BUSY;
  memset(adapter, 0, sizeof(*adapter));
  return ESY_OK;
}

const esy_os_tickless_ops_t *esy_power_tickless_adapter_ops(void) {
  return &k_tickless_ops;
}
