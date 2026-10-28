/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/power_manager/esy_power_watchdog_constraint.h"

#include <string.h>

#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor_core.h"

static esy_err_t read_deadline(void *ctx, uint64_t now_us,
                               esy_power_state_t state,
                               uint64_t *out_deadline_us) {
  esy_power_watchdog_constraint_t *adapter = ctx;
  (void)now_us;
  (void)state;
  if ((adapter == NULL) || (out_deadline_us == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!adapter->initialized) return ESY_ERR_STATE;
  return esy_watchdog_monitor_get_hard_deadline_atomic(&adapter->clock,
                                                       out_deadline_us);
}

static esy_err_t prepare(void *ctx, esy_power_state_t state,
                         uint64_t wake_time_us) {
  esy_power_watchdog_constraint_t *adapter = ctx;
  (void)state;
  (void)wake_time_us;
  if ((adapter == NULL) || !adapter->initialized || adapter->prepared)
    return ESY_ERR_STATE;
  adapter->prepared = true;
  return ESY_OK;
}

static esy_err_t resume(void *ctx, esy_power_state_t state,
                        uint64_t resumed_at_us) {
  esy_power_watchdog_constraint_t *adapter = ctx;
  (void)state;
  (void)resumed_at_us;
  if ((adapter == NULL) || !adapter->initialized || !adapter->prepared)
    return ESY_ERR_STATE;
  adapter->prepared = false;
  return ESY_OK;
}

static const esy_power_constraint_ops_t k_ops = {
    .get_hard_deadline = read_deadline,
    .prepare = prepare,
    .revalidate_atomic = read_deadline,
    .resume = resume,
};

esy_err_t esy_power_watchdog_constraint_init(
    esy_power_watchdog_constraint_t *adapter,
    const esy_monotonic_clock_t *clock) {
  if ((adapter == NULL) || (clock == NULL) || (clock->ops == NULL) ||
      (clock->ops->read_atomic == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (adapter->initialized) return ESY_ERR_STATE;
  memset(adapter, 0, sizeof(*adapter));
  adapter->clock = *clock;
  adapter->initialized = true;
  return ESY_OK;
}

esy_err_t esy_power_watchdog_constraint_deinit(
    esy_power_watchdog_constraint_t *adapter) {
  if (adapter == NULL) return ESY_ERR_INVALID_ARG;
  if (!adapter->initialized) return ESY_ERR_STATE;
  if (adapter->prepared) return ESY_ERR_BUSY;
  memset(adapter, 0, sizeof(*adapter));
  return ESY_OK;
}

const esy_power_constraint_ops_t *esy_power_watchdog_constraint_ops(void) {
  return &k_ops;
}
