/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_system.h"
#include "esy_hb_context_internal.h"

#include <stdatomic.h>

typedef struct {
    _Atomic(const esy_system_context_callbacks_t *) callbacks;
} esy_hb_context_ctx_t;

/* One startup writer publishes an immutable system-lifetime binding. IRQs and
 * tasks acquire it; there is no runtime replacement or callback retirement. */
static esy_hb_context_ctx_t s_ctx;

esy_err_t esy_hb_system_set_context_callbacks(
    const esy_system_context_callbacks_t *callbacks) {
    if (callbacks == NULL || callbacks->is_in_isr == NULL ||
        callbacks->can_block == NULL) return ESY_ERR_INVALID_ARG;
    const esy_system_context_callbacks_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&s_ctx.callbacks, &expected,
            callbacks, memory_order_acq_rel, memory_order_acquire) ||
        expected == callbacks) return ESY_OK;
    return ESY_ERR_STATE;
}

bool esy_hb_context_is_in_isr(bool hardware_fallback) {
    const esy_system_context_callbacks_t *callbacks =
        atomic_load_explicit(&s_ctx.callbacks, memory_order_acquire);
    return callbacks != NULL ? callbacks->is_in_isr() : hardware_fallback;
}

bool esy_hb_context_can_block(void) {
    const esy_system_context_callbacks_t *callbacks =
        atomic_load_explicit(&s_ctx.callbacks, memory_order_acquire);
    return callbacks != NULL && callbacks->can_block();
}
