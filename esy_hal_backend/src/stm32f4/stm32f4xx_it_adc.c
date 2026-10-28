/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_adc.h"

void ADC_IRQHandler(void) { esy_hb_adc_handle_irq(); }
