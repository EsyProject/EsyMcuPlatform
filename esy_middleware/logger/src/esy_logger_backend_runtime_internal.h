/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_BACKEND_RUNTIME_INTERNAL_H
#define ESY_LOGGER_BACKEND_RUNTIME_INTERNAL_H
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_hal/esy_hal_system.h"
/* Logger-private scheduling bridge. Only the OS adapter creates/deletes workers.
 * Caller-driven backends serialize producers/process/lifecycle on one core;
 * local IRQ masking protects shared pool and DMA-completion leases. Other cores
 * must not enter these instances, including through raw callbacks. */
typedef struct {
  uint32_t (*lock)(void);
  void (*unlock)(uint32_t key);
  esy_err_t (*start)(void *ctx, const void *config, void **worker);
  esy_err_t (*stop)(void *worker);
  esy_err_t (*notify)(void *worker);
} esy_logger_backend_runtime_t;
static inline esy_logger_backend_runtime_t esy_logger_poll_runtime(void) {
  esy_logger_backend_runtime_t runtime = {
    .lock = esy_hal_system_irq_save,
    .unlock = esy_hal_system_irq_restore,
    .start = NULL, .stop = NULL, .notify = NULL,
  };
  return runtime;
}
#endif /* ESY_LOGGER_BACKEND_RUNTIME_INTERNAL_H */
