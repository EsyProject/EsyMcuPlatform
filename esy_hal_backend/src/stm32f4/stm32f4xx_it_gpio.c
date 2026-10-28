/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_gpio.h"

/* Each vector dispatches the EXTI line or shared line group it owns. */
void EXTI0_IRQHandler(void) { esy_hb_gpio_handle_irq(0u); }
void EXTI1_IRQHandler(void) { esy_hb_gpio_handle_irq(1u); }
void EXTI2_IRQHandler(void) { esy_hb_gpio_handle_irq(2u); }
void EXTI3_IRQHandler(void) { esy_hb_gpio_handle_irq(3u); }
void EXTI4_IRQHandler(void) { esy_hb_gpio_handle_irq(4u); }
void EXTI9_5_IRQHandler(void) { esy_hb_gpio_handle_irq(5u); }
void EXTI15_10_IRQHandler(void) { esy_hb_gpio_handle_irq(10u); }
