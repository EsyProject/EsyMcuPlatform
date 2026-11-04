/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_CONTEXT_INTERNAL_H
#define ESY_HB_CONTEXT_INTERNAL_H

#include <stdbool.h>

/* Backend-private runtime queries. An unbound context fails closed for blocking
 * and uses the caller's conservative hardware fallback for ISR detection. */
bool esy_hb_context_is_in_isr(bool hardware_fallback);
bool esy_hb_context_can_block(void);

#endif /* ESY_HB_CONTEXT_INTERNAL_H */
