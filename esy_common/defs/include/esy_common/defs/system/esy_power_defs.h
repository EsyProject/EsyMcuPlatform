/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_POWER_DEFS_H
#define ESY_POWER_DEFS_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief System power states ordered from shallowest to deepest. */
typedef enum esy_power_state_e {
  ESY_POWER_STATE_ACTIVE = 0,
  ESY_POWER_STATE_CPU_IDLE,
  ESY_POWER_STATE_RETENTION,
  ESY_POWER_STATE_COUNT
} esy_power_state_t;

/** @brief Build a capability bit for an @ref esy_power_state_t value. */
#define ESY_POWER_STATE_MASK(state) (1u << (uint32_t)(state))

/** @brief Conservative timing limits for one resumable power state. */
typedef struct esy_power_state_capability_s {
  uint64_t entry_latency_us;
  uint64_t exit_latency_us;
  uint64_t min_residency_us;
} esy_power_state_capability_t;

/** @brief Resumable state mask and per-state timing capabilities. */
typedef struct esy_power_capabilities_s {
  uint32_t supported_state_mask;
  esy_power_state_capability_t states[ESY_POWER_STATE_COUNT];
} esy_power_capabilities_t;

/**
 * @brief Explicit, task-context sleep request, separate from atomic idle entry.
 *
 * A port may support this synchronous provider operation without supporting the
 * automatic power-manager/tickless transaction. The caller owns system-wide
 * quiescence and exclusively owns native sleep/wakeup configuration. No context
 * or pointer is retained after return. Currently ESP32 supports RETENTION only.
 */
typedef struct esy_power_sleep_config_s {
  esy_power_state_t state; /**< Requested resumable state; currently RETENTION. */
  /** @brief Positive relative RTC wake delay in microseconds, up to UINT32_MAX.
   * Relative native sleep interval, not an absolute manager deadline.
   * Provider entry/exit overhead can extend the observed call duration. */
  uint32_t duration_us;
} esy_power_sleep_config_t;

/** @brief Outcome of an explicit task-context sleep or pending cleanup retry. */
typedef struct esy_power_sleep_result_s {
  /** @brief Monotonic microseconds across the native call, including overhead.
   * This is diagnostic elapsed time, not a guarantee of FreeRTOS tick settlement.
   * A cleanup-only retry reports zero. */
  uint64_t elapsed_us;
  bool entered; /**< True only when the provider reports successful sleep. */
  bool timer_wakeup; /**< Native provider reported the owned timer wake source. */
  bool gpio_wakeup; /**< Native provider reported borrowed GPIO wake bindings. */
} esy_power_sleep_result_t;

/**
 * @brief Shared monotonic microsecond clock contract.
 *
 * Every absolute deadline exchanged by cooperating modules must use the same
 * clock instance. Reads used in an interrupt-masked window must be bounded,
 * allocation-free, logging-free, and safe with interrupts masked.
 */
typedef struct esy_monotonic_clock_ops_s {
  esy_err_t (*read_atomic)(void *ctx, uint64_t *out_time_us);
} esy_monotonic_clock_ops_t;

/** @brief Caller-owned reference to one monotonic clock domain. */
typedef struct esy_monotonic_clock_s {
  const esy_monotonic_clock_ops_t *ops;
  void *ctx;
} esy_monotonic_clock_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_POWER_DEFS_H */
