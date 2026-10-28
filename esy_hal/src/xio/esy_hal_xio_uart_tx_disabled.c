/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_xio_uart_internal.h"

esy_err_t esy_hal_xio_uart_tx_init(
    esy_uart_t *uart, const esy_xio_uart_config_t *config) {
  (void)uart;
  (void)config;
  return ESY_ERR_NOT_SUPPORTED;
}

void esy_hal_xio_uart_tx_deinit(esy_uart_t *uart) { (void)uart; }

uint8_t esy_hal_xio_uart_tx_pin(esy_uart_t *uart) {
  (void)uart;
  return ESY_PIN_UNUSED;
}

esy_err_t esy_hal_xio_uart_tx_abort_dma(esy_uart_t *uart) {
  (void)uart;
  return ESY_ERR_NOT_SUPPORTED;
}
esy_err_t esy_hal_xio_uart_tx_poll(esy_uart_t *uart) {
  (void)uart;
  return ESY_OK;
}

bool esy_hal_xio_uart_tx_is_busy(esy_uart_t *uart) {
  (void)uart;
  return false;
}

int32_t esy_hal_xio_uart_tx_send_blocking(esy_uart_t *uart,
                                         const uint8_t *data, size_t length) {
  (void)uart; (void)data; (void)length;
  return ESY_ERR_NOT_SUPPORTED;
}
esy_err_t esy_hal_xio_uart_tx_send_dma(esy_uart_t *uart,
                                       esy_dma_config_t *config) {
  (void)uart; (void)config;
  return ESY_ERR_NOT_SUPPORTED;
}
