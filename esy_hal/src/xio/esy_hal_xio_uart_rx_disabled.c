/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_xio_uart_internal.h"

esy_err_t esy_hal_xio_uart_rx_init(
    esy_uart_t *uart, const esy_xio_uart_config_t *config) {
  (void)uart;
  (void)config;
  return ESY_ERR_NOT_SUPPORTED;
}

void esy_hal_xio_uart_rx_deinit(esy_uart_t *uart) { (void)uart; }

uint8_t esy_hal_xio_uart_rx_pin(esy_uart_t *uart) {
  (void)uart;
  return ESY_PIN_UNUSED;
}

esy_err_t esy_hal_xio_uart_rx_abort_dma(esy_uart_t *uart) {
  (void)uart;
  return ESY_ERR_NOT_SUPPORTED;
}
esy_err_t esy_hal_xio_uart_rx_poll(esy_uart_t *uart) {
  (void)uart;
  return ESY_OK;
}

int32_t esy_hal_xio_uart_rx_receive_blocking(esy_uart_t *uart,
                                            uint8_t *data, size_t length) {
  (void)uart; (void)data; (void)length;
  return ESY_ERR_NOT_SUPPORTED;
}
esy_err_t esy_hal_xio_uart_rx_receive_dma(esy_uart_t *uart,
                                          esy_dma_config_t *config) {
  (void)uart; (void)config;
  return ESY_ERR_NOT_SUPPORTED;
}
bool esy_hal_xio_uart_rx_is_readable(const esy_uart_t *uart) {
  (void)uart;
  return false;
}
