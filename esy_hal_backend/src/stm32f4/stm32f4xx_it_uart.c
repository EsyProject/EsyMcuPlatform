/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_uart.h"

/* Physical UART indices match the backend's USART instance identifiers. */
void USART1_IRQHandler(void) { esy_hb_uart_handle_irq(1u); }
void USART2_IRQHandler(void) { esy_hb_uart_handle_irq(2u); }
void USART3_IRQHandler(void) { esy_hb_uart_handle_irq(3u); }
