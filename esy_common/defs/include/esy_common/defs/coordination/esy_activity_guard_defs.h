/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ACTIVITY_GUARD_DEFS_H
#define ESY_ACTIVITY_GUARD_DEFS_H

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Optional, injected guard for a transaction that constrains sleep.
 *
 * The provider owns @ref ctx and keeps it valid for every consumer using this
 * object. Acquire and release run in task context and must be paired.
 */
typedef struct esy_activity_guard_s {
  void *ctx;
  esy_err_t (*acquire)(void *ctx);
  esy_err_t (*release)(void *ctx);
} esy_activity_guard_t;

/** @brief Acquire an optional activity guard. NULL is a successful no-op. */
static inline esy_err_t esy_activity_guard_acquire(
    const esy_activity_guard_t *guard) {
  if (guard == NULL) return ESY_OK;
  if ((guard->ctx == NULL) || (guard->acquire == NULL) ||
      (guard->release == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  return guard->acquire(guard->ctx);
}

/** @brief Release a previously acquired optional activity guard. */
static inline esy_err_t esy_activity_guard_release(
    const esy_activity_guard_t *guard) {
  if (guard == NULL) return ESY_OK;
  if ((guard->ctx == NULL) || (guard->acquire == NULL) ||
      (guard->release == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  return guard->release(guard->ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* ESY_ACTIVITY_GUARD_DEFS_H */
