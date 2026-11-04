/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PWM_EVENT_PROBE_STDATOMIC_H
#define PWM_EVENT_PROBE_STDATOMIC_H

#include_next <stdatomic.h>

/* The standalone host consumer injects a public fake IRQ immediately before
 * the real atomic clear, after the owner's earlier terminal observation.
 * This keeps the production source and its private context unchanged. */
void pwm_event_probe_before_clear(void);

#undef atomic_fetch_and_explicit
#define atomic_fetch_and_explicit(object, operand, order) \
    (pwm_event_probe_before_clear(), __atomic_fetch_and((object), (operand), (order)))

#endif /* PWM_EVENT_PROBE_STDATOMIC_H */
