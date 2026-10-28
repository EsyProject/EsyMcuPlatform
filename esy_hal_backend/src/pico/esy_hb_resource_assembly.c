/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hb_resource_assembly_internal.h"

#include <stdatomic.h>

typedef struct {
  atomic_flag reserved;
} esy_hb_resource_assembly_ctx_t;

static esy_hb_resource_assembly_ctx_t s_ctx = {
    .reserved = ATOMIC_FLAG_INIT,
};

bool esy_hb_resource_assembly_try_acquire(void) {
  return !atomic_flag_test_and_set_explicit(&s_ctx.reserved, memory_order_acquire);
}

void esy_hb_resource_assembly_release(void) {
  atomic_flag_clear_explicit(&s_ctx.reserved, memory_order_release);
}
