/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_xio_uart_internal.h"
#include "esy_hal_backend/xio/uart_tx/esy_hb_xio_uart_tx.h"
#include "esy_board/esy_board.h"

esy_err_t esy_hal_xio_uart_tx_send_dma(esy_uart_t *uart, esy_dma_config_t *config) {
  uint8_t pin = esy_hb_xio_uart_tx_get_pin(uart);
  if (!esy_board_xio_get_dma_info(pin, &config->inner))
    return ESY_ERR_NOT_SUPPORTED;
  return esy_hb_xio_uart_tx_write_dma(uart, config);
}

esy_err_t esy_hal_xio_uart_tx_init(
    esy_uart_t *uart, const esy_xio_uart_config_t *config) {
  esy_xio_inner_config_t inner = {
      .instance = config->instance, .sm_index = config->sm_index,
  };
  esy_board_xio_configure_pins(config->pin, &inner);
  esy_err_t ret = esy_hb_xio_uart_tx_init(uart, config);

  return ret;
}

void esy_hal_xio_uart_tx_deinit(esy_uart_t *uart) {
  esy_hb_xio_uart_tx_deinit(uart);
}

uint8_t esy_hal_xio_uart_tx_pin(esy_uart_t *uart) {
  return esy_hb_xio_uart_tx_get_pin(uart);
}

esy_err_t esy_hal_xio_uart_tx_abort_dma(esy_uart_t *uart) {
  return esy_hb_xio_uart_tx_abort_dma(uart);
}
esy_err_t esy_hal_xio_uart_tx_poll(esy_uart_t *uart) {
  return esy_hb_xio_uart_tx_poll(uart);
}

bool esy_hal_xio_uart_tx_is_busy(esy_uart_t *uart) {
  return esy_hb_xio_uart_tx_is_busy(uart);
}

int32_t esy_hal_xio_uart_tx_send_blocking(esy_uart_t *uart,
                                         const uint8_t *data, size_t length) {
  return esy_hb_xio_uart_tx_write_blocking(uart, data, length);
}
