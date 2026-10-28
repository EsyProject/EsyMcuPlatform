/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_hal_uart.h
 * @brief Reusable fake implementation contract for HAL UART tests.
 */

#ifndef FAKE_ESY_HAL_UART_H
#define FAKE_ESY_HAL_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_hal/esy_hal_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FAKE_ESY_HAL_UART_MAX_INSTANCES
#define FAKE_ESY_HAL_UART_MAX_INSTANCES 4u
#endif

/**
 * @brief Observable fake UART slot.
 */
typedef struct fake_esy_hal_uart_slot_s {
  bool used;
  esy_uart_t *uart;
  esy_uart_config_t cfg;
  bool initialized;
  bool deinitialized;

  const uint8_t *rx_data;
  size_t rx_len;
  size_t rx_pos;

  uint8_t last_tx[128];
  size_t last_tx_len;
  uint32_t send_count;
  /** Optional blocking-send result injection; negative errors and short writes. */
  bool override_send_result;
  int32_t send_result;
  /** Synchronous hooks during send, before returning; reset clears them. */
  void (*send_hook)(esy_uart_t *uart);
  void (*send_dma_hook)(esy_uart_t *uart);
  uint32_t receive_count;

  bool tx_busy;
  bool keep_tx_busy_after_dma; /**< Model source retirement before wire drain. */
  void (*busy_hook)(esy_uart_t *uart); /**< Optional synchronous TX busy-query hook. */
  bool dma_pending; /**< Accepted TX borrow survives a hardware-idle observation. */
  bool rx_busy;
  esy_err_t poll_result; /**< Injected owner poll result, initially ESY_OK. */
  uint32_t poll_count; /**< Number of owner polls. */
  void (*poll_hook)(esy_uart_t *uart); /**< Successful poll hook. */
  uint32_t send_dma_count;
  esy_err_t send_dma_result;
  bool retain_failed_submission; /**< Failed submit may retain TX storage. */
  esy_err_t abort_result; /**< Injected abort failure retains callbacks/storage. */
  uint32_t abort_count; /**< Number of abort attempts. */
  const void *last_dma_src;
  size_t last_dma_len;
  void (*last_dma_callback)(void *arg);
  void *last_dma_callback_arg;
  void (*last_dma_error_callback)(void *arg);
  void *last_dma_error_callback_arg;
  uint32_t caps;
  uint8_t phys_index;
  uint8_t *rx_stream_storage;
  size_t rx_stream_storage_size;
  size_t rx_stream_used;
  size_t rx_stream_peek_length;
  esy_uart_rx_stream_callback_t rx_stream_callback;
  void *rx_stream_user_data;
  /** Optional synchronous hook after RX start, cleared by fake reset. */
  void (*rx_stream_start_hook)(esy_uart_t *uart);
  bool retain_failed_rx_start; /**< Borrow storage/context on non-idle start errors. */
  esy_err_t rx_stream_start_result; /**< Injected start result; defaults to ESY_OK. */
  esy_err_t rx_stream_peek_result; /**< Injected peek result; defaults to ESY_OK. */
  esy_err_t rx_stream_consume_result; /**< Injected consume result; defaults to ESY_OK. */
  uint32_t rx_stream_start_count; /**< Number of start attempts. */
  uint32_t rx_stream_stop_count; /**< Number of stop attempts. */
  /** Optional hook after a successful peek, before its span is consumed. */
  void (*rx_stream_peek_hook)(esy_uart_t *uart);
  esy_err_t rx_stream_stop_result; /**< Injected stop result; defaults to ESY_OK. */
} fake_esy_hal_uart_slot_t;

void fake_esy_hal_uart_reset(void);
fake_esy_hal_uart_slot_t *fake_esy_hal_uart_find(esy_uart_t *uart);
void fake_esy_hal_uart_set_rx_data(esy_uart_t *uart, const uint8_t *data,
                                        size_t len);
void fake_esy_hal_uart_set_rx_text(esy_uart_t *uart, const char *text);
void fake_esy_hal_uart_set_send_dma_result(esy_uart_t *uart,
                                                esy_err_t ret);
void fake_esy_hal_uart_complete_last_dma(esy_uart_t *uart);
void fake_esy_hal_uart_fail_last_dma(esy_uart_t *uart);
size_t fake_esy_hal_uart_push_rx_stream(esy_uart_t *uart,
                                             const uint8_t *data, size_t len);
size_t fake_esy_hal_uart_copy_last_tx(esy_uart_t *uart, uint8_t *out,
                                           size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_UART_H */
