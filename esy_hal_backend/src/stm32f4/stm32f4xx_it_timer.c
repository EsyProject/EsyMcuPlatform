/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_timer.h"

/* Logical timers exclusively own TIM2/TIM5 and their compare interrupt paths. */
void TIM2_IRQHandler(void) { esy_hb_timer_handle_ms_irq(); }
void TIM5_IRQHandler(void) { esy_hb_timer_handle_us_irq(); }
