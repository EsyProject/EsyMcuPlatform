/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_XIO_UART_INTERNAL_H
#define ESY_HAL_XIO_UART_INTERNAL_H
#include "esy_hal/xio/esy_hal_xio_uart.h"
esy_err_t esy_hal_xio_uart_tx_init(
    esy_uart_t *uart, const esy_xio_uart_config_t *config);
void esy_hal_xio_uart_tx_deinit(esy_uart_t *uart);
uint8_t esy_hal_xio_uart_tx_pin(esy_uart_t *uart);
esy_err_t esy_hal_xio_uart_rx_init(
    esy_uart_t *uart, const esy_xio_uart_config_t *config);
void esy_hal_xio_uart_rx_deinit(esy_uart_t *uart);
uint8_t esy_hal_xio_uart_rx_pin(esy_uart_t *uart);
esy_err_t esy_hal_xio_uart_tx_abort_dma(esy_uart_t *uart);
esy_err_t esy_hal_xio_uart_rx_abort_dma(esy_uart_t *uart);
esy_err_t esy_hal_xio_uart_tx_poll(esy_uart_t *uart);
bool esy_hal_xio_uart_tx_is_busy(esy_uart_t *uart);
esy_err_t esy_hal_xio_uart_rx_poll(esy_uart_t *uart);
int32_t esy_hal_xio_uart_tx_send_blocking(esy_uart_t *uart,
                                         const uint8_t *data, size_t length);
esy_err_t esy_hal_xio_uart_tx_send_dma(esy_uart_t *uart,
                                       esy_dma_config_t *config);
int32_t esy_hal_xio_uart_rx_receive_blocking(esy_uart_t *uart,
                                            uint8_t *data, size_t length);
esy_err_t esy_hal_xio_uart_rx_receive_dma(esy_uart_t *uart,
                                          esy_dma_config_t *config);
bool esy_hal_xio_uart_rx_is_readable(const esy_uart_t *uart);
#endif
