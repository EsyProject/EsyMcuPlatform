/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_IO_SCHEDULER_CORE_INTERNAL_H
#define ESY_IO_SCHEDULER_CORE_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "esy_common/defs/peripheral/esy_dma_defs.h"
static inline void esy_io_scheduler_atomic_flag_init(atomic_bool *flag,
                                                bool value) {
  atomic_init(flag, value);
}

/* Sequential consistency closes the two-flag start/quiesce handshake. */
static inline bool esy_io_scheduler_atomic_flag_get(atomic_bool *flag) {
  return atomic_load_explicit(flag, memory_order_seq_cst);
}

static inline bool esy_io_scheduler_atomic_flag_get_const(const atomic_bool *flag) {
  return atomic_load_explicit(flag, memory_order_seq_cst);
}

static inline void esy_io_scheduler_atomic_flag_set(atomic_bool *flag, bool value) {
  atomic_store_explicit(flag, value, memory_order_seq_cst);
}

static inline bool esy_io_scheduler_transfer_valid(const void *tx, const void *rx,
                                             size_t len,
                                             esy_dma_transfer_size_t width) {
  size_t unit;
  switch (width) {
    case ESY_DMA_TRANSFER_SIZE_8: unit = 1u; break;
    case ESY_DMA_TRANSFER_SIZE_16: unit = 2u; break;
    case ESY_DMA_TRANSFER_SIZE_32: unit = 4u; break;
    default: return false;
  }
  return len > 0u && len <= INT32_MAX && (len % unit) == 0u &&
         ((uintptr_t)tx % unit) == 0u && ((uintptr_t)rx % unit) == 0u;
}

#endif /* ESY_IO_SCHEDULER_CORE_INTERNAL_H */
