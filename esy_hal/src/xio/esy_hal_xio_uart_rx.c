/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_xio_uart_internal.h"
#include "esy_hal_backend/xio/uart_rx/esy_hb_xio_uart_rx.h"
#include "esy_board/esy_board.h"

esy_err_t esy_hal_xio_uart_rx_receive_dma(esy_uart_t *uart, esy_dma_config_t *config) {
  uint8_t pin = esy_hb_xio_uart_rx_get_pin(uart);
  if (!esy_board_xio_get_dma_info(pin, &config->inner))
    return ESY_ERR_NOT_SUPPORTED;
  return esy_hb_xio_uart_rx_read_dma(uart, config);
}

esy_err_t esy_hal_xio_uart_rx_init(
    esy_uart_t *uart, const esy_xio_uart_config_t *config) {
  esy_xio_inner_config_t inner = {
      .instance = config->instance, .sm_index = config->sm_index,
  };
  esy_board_xio_configure_pins(config->pin, &inner);
  esy_err_t ret = esy_hb_xio_uart_rx_init(uart, config);

  return ret;
}

void esy_hal_xio_uart_rx_deinit(esy_uart_t *uart) {
  esy_hb_xio_uart_rx_deinit(uart);
}

uint8_t esy_hal_xio_uart_rx_pin(esy_uart_t *uart) {
  return esy_hb_xio_uart_rx_get_pin(uart);
}

esy_err_t esy_hal_xio_uart_rx_abort_dma(esy_uart_t *uart) {
  return esy_hb_xio_uart_rx_abort_dma(uart);
}
esy_err_t esy_hal_xio_uart_rx_poll(esy_uart_t *uart) {
  return esy_hb_xio_uart_rx_poll(uart);
}

int32_t esy_hal_xio_uart_rx_receive_blocking(esy_uart_t *uart,
                                            uint8_t *data, size_t length) {
  return esy_hb_xio_uart_rx_read_blocking(uart, data, length);
}
bool esy_hal_xio_uart_rx_is_readable(const esy_uart_t *uart) {
  return esy_hb_xio_uart_rx_is_readable(uart);
}
