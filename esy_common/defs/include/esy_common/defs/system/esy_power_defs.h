/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_POWER_DEFS_H
#define ESY_POWER_DEFS_H

#include <stdint.h>

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
