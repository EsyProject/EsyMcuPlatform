/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake_esy_hal_uart.h"
#include "unity.h"

typedef struct { unsigned completions; } uart_fake_test_ctx_t;
static uart_fake_test_ctx_t s_ctx;
static void complete(void *arg) { ++((uart_fake_test_ctx_t *)arg)->completions; }
static void receive(esy_uart_t *uart, esy_uart_rx_stream_event_t event, void *arg) {
  (void)uart; (void)event; (void)arg;
}
void setUp(void) { fake_esy_hal_uart_reset(); s_ctx.completions = 0u; }
void tearDown(void) {}
void test_rejected_dma_never_calls_back_and_busy_deinit_retains_accepted_dma(void) {
  esy_uart_t uart = {0};
  uint8_t bytes[4] = {0};
  esy_dma_config_t dma = {.src = bytes, .length = sizeof(bytes),
      .callback = complete, .callback_arg = &s_ctx};
  fake_esy_hal_uart_set_send_dma_result(&uart, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_send_dma(&uart, &dma));
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(0u, s_ctx.completions);
  fake_esy_hal_uart_set_send_dma_result(&uart, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_send_dma(&uart, &dma));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_deinit(&uart));
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&uart));
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(1u, s_ctx.completions);
}
void test_failed_rx_start_retains_storage_until_stop_retry_and_rejects_other_owner(void) {
  esy_uart_t uart = {0};
  uint8_t bytes[8] = {0}, other[8] = {0};
  fake_esy_hal_uart_set_send_dma_result(&uart, ESY_OK);
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  slot->rx_stream_start_result = ESY_ERR_IO;
  slot->retain_failed_rx_start = true;
  slot->rx_stream_stop_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_rx_stream_start(&uart,
      bytes, sizeof(bytes), receive, &s_ctx));
  TEST_ASSERT_EQUAL_PTR(bytes, slot->rx_stream_storage);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_rx_stream_start(&uart,
      other, sizeof(other), receive, NULL));
  TEST_ASSERT_EQUAL_PTR(&s_ctx, slot->rx_stream_user_data);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_deinit(&uart));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_EQUAL_PTR(bytes, slot->rx_stream_storage);
  slot->rx_stream_stop_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_NULL(slot->rx_stream_callback);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&uart));
}
