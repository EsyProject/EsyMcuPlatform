/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_uart.h"
#include "esy_hal/esy_hal_uart_device.h"
#include "esy_hal/xio/esy_hal_xio_uart.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) { platform_test_reset(); }

void tearDown(void) {}

void test_uart_unsupported_framing_is_rejected_before_claim_or_pin_changes(void) {
  esy_uart_t uart = {0};
  esy_uart_config_t config;
  esy_hal_uart_default_cfg(&config);
  config.phys_index = TEST_UART_INDEX;
  config.data_bits = 9u;
  platform_test_set_uart_validate_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_uart_init(&uart, &config));
  TEST_ASSERT_NULL(uart.ctx[0]);
  TEST_ASSERT_NULL(uart.ops);
  TEST_ASSERT_NULL(platform_test_state()->uart_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  config.data_bits = 8u;
  platform_test_set_uart_validate_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&uart, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&uart));
}

static void init_uart(esy_uart_t *uart) {
  esy_uart_config_t cfg = {
      .phys_index = TEST_UART_INDEX,
      .baudrate = 115200u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = ESY_UART_PARITY_NONE,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_init(uart, &cfg));
}

static void rx_stream_callback(esy_uart_t *uart,
                               esy_uart_rx_stream_event_t event,
                               void *user_data) {
  (void)uart;
  (void)event;
  (void)user_data;
}

void test_esy_hal_uart_poll_rejects_a_missing_backend_hook(void) {
  esy_uart_t uart = {0};

  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_uart_poll(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_poll(&uart));
  init_uart(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_poll(&uart));
  static const esy_uart_vtable_t k_no_poll_ops = {0};
  uart.ops = &k_no_poll_ops;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_poll(&uart));
  esy_hal_uart_deinit(&uart);
}

void test_esy_hal_uart_default_cfg_sets_standard_serial_defaults(void) {
  esy_uart_config_t cfg = {
      .phys_index = 9u,
      .baudrate = 1u,
      .data_bits = 7u,
      .stop_bits = 2u,
      .parity = ESY_UART_PARITY_ODD,
      .inner = {.irq_priority = 3u},
  };

  esy_hal_uart_default_cfg(NULL);
  esy_hal_uart_default_cfg(&cfg);

  TEST_ASSERT_EQUAL_UINT8(0u, cfg.phys_index);
  TEST_ASSERT_EQUAL_UINT32(115200u, cfg.baudrate);
  TEST_ASSERT_EQUAL_UINT8(8u, cfg.data_bits);
  TEST_ASSERT_EQUAL_UINT8(1u, cfg.stop_bits);
  TEST_ASSERT_EQUAL_INT(ESY_UART_PARITY_NONE, cfg.parity);
  TEST_ASSERT_EQUAL_UINT8(0u, cfg.inner.irq_priority);
}

void test_esy_hal_uart_init_forwards_board_irq_priority_without_changing_caller_config(void) {
  esy_uart_t uart = {0};
  esy_uart_config_t cfg;

  esy_hal_uart_default_cfg(&cfg);
  cfg.phys_index = TEST_UART_INDEX;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_init(&uart, &cfg));
  TEST_ASSERT_EQUAL_UINT8(TEST_UART_INDEX, platform_test_state()->configure_index);
  TEST_ASSERT_EQUAL_UINT8(TEST_UART_IRQ_PRIORITY,
                         platform_test_state()->last_irq_priority);
  TEST_ASSERT_TRUE(platform_test_state()->board_configure_order <
                   platform_test_state()->driver_init_order);
  TEST_ASSERT_EQUAL_UINT8(0u, cfg.inner.irq_priority);

  esy_hal_uart_deinit(&uart);
}

void test_esy_hal_uart_contracts_are_enforced(void) {
  esy_uart_t uart = {0};
  uint8_t data[2] = {1u, 2u};
  uint8_t rx_data[2] = {0u};
  esy_dma_config_t dma = {
      .src = data,
      .length = sizeof(data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  esy_dma_config_t rx_dma = {
      .dst = rx_data,
      .length = sizeof(rx_data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  uint32_t caps = 0u;
  uint8_t index = 0u;
  size_t tx_remaining = 0u;
  size_t rx_remaining = 0u;
  const uint8_t *stream_data = NULL;
  size_t stream_length = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_init(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_init(&uart, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_uart_send_blocking(
                                                   NULL, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_uart_send_blocking(
                                                   &uart, NULL, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_send_blocking(&uart, data, 0u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_uart_send_blocking(&uart, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
                          esy_hal_uart_receive_blocking(&uart, NULL, sizeof(rx_data)));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
                          esy_hal_uart_receive_blocking(&uart, rx_data, 0u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_uart_receive_blocking(&uart, rx_data, sizeof(rx_data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_send_dma(NULL, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_send_dma(&uart, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_send_dma(&uart, &rx_dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_uart_send_dma(&uart, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_receive_dma(&uart, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_receive_dma(&uart, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_uart_receive_dma(&uart, &rx_dma));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_uart_rx_stream_start(NULL, rx_data, sizeof(rx_data),
                                        rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_uart_rx_stream_start(&uart, NULL, sizeof(rx_data),
                                        rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_rx_stream_start(
                              &uart, rx_data, 1u, rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_rx_stream_start(
                              &uart, rx_data, sizeof(rx_data), NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_hal_uart_rx_stream_start(&uart, rx_data, sizeof(rx_data),
                                        rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_uart_rx_stream_peek(NULL, &stream_data, &stream_length));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_hal_uart_rx_stream_peek(&uart, NULL, &stream_length));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_NOT_SUPPORTED,
      esy_hal_uart_rx_stream_peek(&uart, &stream_data, &stream_length));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_rx_stream_consume(NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_rx_stream_consume(&uart, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_uart_rx_stream_consume(&uart, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_rx_stream_stop(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_get_dma_trans_count_remaining(
                              NULL, &tx_remaining, NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
                          esy_hal_uart_get_dma_trans_count_remaining(&uart, NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_uart_suspend(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_uart_suspend(&uart));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_uart_resume(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_uart_resume(&uart));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(NULL, true));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_FALSE(esy_hal_uart_is_readable(NULL));
  TEST_ASSERT_FALSE(esy_hal_uart_is_readable(&uart));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_set_baudrate(NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_set_baudrate(&uart, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_uart_set_baudrate(&uart, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_get_caps(&uart, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_uart_get_caps(&uart, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_get_phys_index(NULL, &index));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_uart_get_phys_index(&uart, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_uart_get_phys_index(&uart, &index));

  init_uart(&uart);
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  TEST_ASSERT_NOT_NULL(uart.ops->send_blocking);
  TEST_ASSERT_NOT_NULL(uart.ops->send_dma);
  TEST_ASSERT_NOT_NULL(uart.ops->rx_stream_start);
  TEST_ASSERT_EQUAL_INT32(
      2, esy_hal_uart_send_blocking(&uart, data, sizeof(data)));
  TEST_ASSERT_EQUAL_INT32(
      2, esy_hal_uart_receive_blocking(&uart, rx_data, sizeof(rx_data)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, uart.ops->send_dma(NULL, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          uart.ops->receive_dma(NULL, &rx_dma));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_send_dma(&uart, &dma));
  TEST_ASSERT_EQUAL_UINT32(TEST_TX_DREQ,
                           platform_test_state()->last_tx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_TX_CHANNEL,
                          platform_test_state()->last_tx_inner.channel);
  esy_dma_config_t submitted_tx = platform_test_state()->submitted_dma;
  submitted_tx.callback(submitted_tx.callback_arg);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_uart_receive_dma(&uart, &rx_dma));
  TEST_ASSERT_EQUAL_UINT32(TEST_RX_DREQ,
                           platform_test_state()->last_rx_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_RX_CHANNEL,
                          platform_test_state()->last_rx_inner.channel);
  esy_dma_config_t submitted_rx = platform_test_state()->submitted_dma;
  submitted_rx.callback(submitted_rx.callback_arg);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_uart_rx_stream_start(&uart, rx_data, sizeof(rx_data),
                                                rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_rx_stream_peek(
                                      &uart, &stream_data, &stream_length));
  TEST_ASSERT_EQUAL_PTR(rx_data, stream_data);
  TEST_ASSERT_EQUAL_size_t(sizeof(rx_data), stream_length);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_uart_rx_stream_consume(&uart, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_rx_stream_stop(&uart));
  esy_hal_uart_handle_irq(TEST_UART_INDEX);
  TEST_ASSERT_EQUAL_UINT8(TEST_UART_INDEX, platform_test_state()->last_channel);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_uart_get_dma_trans_count_remaining(
                                      &uart, &tx_remaining, &rx_remaining));
  TEST_ASSERT_EQUAL_size_t(1u, tx_remaining);
  TEST_ASSERT_EQUAL_size_t(2u, rx_remaining);
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_suspend(&uart));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_resume(&uart));
  platform_test_set_dma_busy(true);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  platform_test_set_dma_busy(false);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_TRUE(esy_hal_uart_is_readable(&uart));
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_set_baudrate(&uart, 9600u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_uart_get_caps(&uart, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_UART_CAP_TX_DMA | ESY_UART_CAP_RX_DMA | ESY_UART_CAP_RX_STREAM, caps);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_uart_get_phys_index(&uart, &index));
  TEST_ASSERT_EQUAL_UINT8(TEST_UART_INDEX, index);

  platform_test_reset_order_counters();
  esy_hal_uart_deinit(&uart);
  state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT8(TEST_UART_INDEX, state->reset_index);
  TEST_ASSERT_TRUE(state->driver_get_index_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->driver_deinit_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->board_reset_order < state->uart_release_order);
}

void test_uart_claim_rejects_competitors_before_board_or_native_changes(void) {
  esy_uart_t owner = {0};
  esy_uart_t contender = {0};
  esy_uart_config_t config;
  uint8_t byte = 0x52u;
  esy_hal_uart_default_cfg(&config);
  config.phys_index = TEST_UART_INDEX;
  platform_test_set_dma_busy(false);

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&owner, &config));
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_TRUE(state->uart_claim_order < state->board_configure_order);
  uint32_t configure_order = state->board_configure_order;
  config.baudrate = 9600u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configure_order, state->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, state->board_reset_order);
  TEST_ASSERT_NULL(contender.ctx[0]);
  TEST_ASSERT_EQUAL_PTR(&owner, state->uart_owner);
  TEST_ASSERT_EQUAL_INT32(1, esy_hal_uart_send_blocking(&owner, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&contender));
}

void test_uart_failed_deinit_retains_handle_pins_and_claim_until_retry(void) {
  esy_uart_t owner = {0};
  esy_uart_t contender = {0};
  esy_uart_config_t config;
  esy_hal_uart_default_cfg(&config);
  config.phys_index = TEST_UART_INDEX;
  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_uart_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&owner));
  init_uart(&owner);
  void *context = owner.ctx[0];

  platform_test_reset_order_counters();
  platform_test_set_uart_cleanup_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_deinit(&owner));
  TEST_ASSERT_EQUAL_PTR(context, owner.ctx[0]);
  TEST_ASSERT_NOT_NULL(owner.ops->send_blocking);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->uart_release_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_init(&contender, &config));
  platform_test_set_uart_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&owner));
  TEST_ASSERT_NULL(owner.ctx[0]);
  TEST_ASSERT_NULL(owner.ops);
  TEST_ASSERT_NULL(platform_test_state()->uart_owner);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&contender));
}

void test_uart_failed_init_releases_only_after_cleanup_or_preserves_retry(void) {
  esy_uart_t owner = {0};
  esy_uart_t contender = {0};
  esy_uart_config_t config;
  esy_hal_uart_default_cfg(&config);
  config.phys_index = TEST_UART_INDEX;
  platform_test_set_dma_busy(false);
  platform_test_set_init_result(ESY_ERR_INVALID_ARG);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_uart_init(&owner, &config));
  TEST_ASSERT_NULL(owner.ctx[0]);
  TEST_ASSERT_NULL(platform_test_state()->uart_owner);
  TEST_ASSERT_TRUE(platform_test_state()->driver_deinit_order <
                   platform_test_state()->board_reset_order);
  TEST_ASSERT_TRUE(platform_test_state()->board_reset_order <
                   platform_test_state()->uart_release_order);

  platform_test_reset_order_counters();
  platform_test_set_uart_cleanup_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_init(&owner, &config));
  TEST_ASSERT_NOT_NULL(owner.ctx[0]);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->uart_owner);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_init(&contender, &config));
  platform_test_set_init_result(ESY_OK);
  platform_test_set_uart_cleanup_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&contender));
}

void test_uart_suspend_keeps_controller_reserved_until_owner_deinit(void) {
  esy_uart_t owner = {0};
  esy_uart_t contender = {0};
  esy_uart_config_t config;
  esy_hal_uart_default_cfg(&config);
  config.phys_index = TEST_UART_INDEX;
  platform_test_set_dma_busy(false);
  init_uart(&owner);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_suspend(&owner));
  uint32_t configure_order = platform_test_state()->board_configure_order;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configure_order,
                          platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->uart_owner);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_resume(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&owner));
}

void test_native_uart_lifecycle_rejects_xio_and_copied_backend_handles(void) {
  esy_uart_t owner = {0};
  esy_uart_t xio = {0};
  esy_uart_config_t config;
  esy_xio_uart_config_t rx = {.pin = TEST_XIO_RX_PIN};
  esy_hal_uart_default_cfg(&config);
  config.phys_index = TEST_UART_INDEX;
  platform_test_set_dma_busy(false);
  init_uart(&owner);
  esy_uart_t alias = owner;
  platform_test_reset_order_counters();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_deinit(&alias));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_deinit_order);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_xio_uart_init(&xio, NULL, &rx));
  void *rx_context = xio.ctx[1];
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_init(&xio, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_deinit(&xio));
  TEST_ASSERT_EQUAL_PTR(rx_context, xio.ctx[1]);
  TEST_ASSERT_EQUAL_PTR(&owner, platform_test_state()->uart_owner);
  esy_hal_xio_uart_deinit(&xio);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&owner));
}

static void count_uart_completion(void *arg) { ++*(unsigned *)arg; }

void test_uart_unregistered_dma_abort_retries_without_releasing_storage(void) {
  platform_test_set_dma_busy(false);
  esy_uart_t uart = {0};
  init_uart(&uart);
  uint8_t bytes[4] = {0};
  unsigned completed = 0u;
  esy_dma_config_t transfer = {.src = bytes, .dst = bytes, .length = sizeof(bytes),
      .callback = count_uart_completion, .callback_arg = &completed};
  const esy_dma_config_t original = transfer;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_send_dma(&uart, &transfer));
  TEST_ASSERT_EQUAL_MEMORY(&original, &transfer, sizeof(transfer));
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_send_dma(&uart, &transfer));
  esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  platform_test_set_uart_dma_results(ESY_ERR_TIMEOUT, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_uart_abort_dma(&uart, true));
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, true));
  esy_hal_uart_deinit(&uart);
  TEST_ASSERT_NOT_NULL(uart.ops->send_dma);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_poll(&uart));
  platform_test_set_uart_dma_results(ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_abort_dma(&uart, true));
  TEST_ASSERT_TRUE(platform_test_state()->uart_abort_is_tx);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_receive_dma(&uart, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_abort_dma(&uart, false));
  TEST_ASSERT_FALSE(platform_test_state()->uart_abort_is_tx);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_abort_dma(&uart, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_poll(&uart));
  esy_hal_uart_deinit(&uart);
  TEST_ASSERT_NULL(uart.ops);
}

typedef struct {
  esy_uart_t *uart;
  esy_device_t *device;
  unsigned notifications;
} uart_callback_probe_t;

static void check_uart_callback_retains_ownership(void *arg) {
  uart_callback_probe_t *probe = arg;
  probe->notifications++;
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(probe->uart, true));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_poll(probe->uart));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_abort_dma(probe->uart, true));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_unregister_device(
      (esy_uart_device_binding_t *)probe->device->driver_ctx));
  uint32_t references;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(probe->device, &references));
  TEST_ASSERT_EQUAL_UINT32(1u, references);
}

void test_uart_completion_retires_once_and_holds_pm_through_callback_return(void) {
  platform_test_set_dma_busy(false);
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t cfg = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "uart", .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)};
  esy_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  init_uart(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_register_device(
      &binding, &uart, &registry, &registration));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  uint8_t byte = 0u;
  uart_callback_probe_t probe = {.uart = &uart, .device = &binding.device};
  esy_dma_config_t transfer = {.src = &byte, .length = sizeof(byte),
      .callback = check_uart_callback_retains_ownership, .callback_arg = &probe};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_send_dma(&uart, &transfer));
  esy_dma_config_t submitted = platform_test_state()->submitted_dma;
  submitted.callback(submitted.callback_arg);
  submitted.error_callback(submitted.error_callback_arg);
  submitted.callback(submitted.callback_arg);
  TEST_ASSERT_EQUAL_UINT(1u, probe.notifications);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  uint32_t references;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&binding.device, &references));
  TEST_ASSERT_EQUAL_UINT32(0u, references);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_unregister_device(&binding));
  esy_hal_uart_deinit(&uart);
}

typedef struct {
  esy_dma_config_t submitted;
  esy_err_t submit_result;
  esy_err_t abort_result;
  bool inline_completion;
  bool complete_during_abort;
} uart_submission_probe_t;

static esy_err_t probe_uart_submit(esy_uart_t *uart, esy_dma_config_t *config) {
  uart_submission_probe_t *probe = uart->ctx[1];
  probe->submitted = *config;
  if (probe->inline_completion) config->callback(config->callback_arg);
  return probe->submit_result;
}
static esy_err_t probe_uart_abort(esy_uart_t *uart, bool is_tx) {
  (void)is_tx;
  uart_submission_probe_t *probe = uart->ctx[1];
  if (probe->complete_during_abort)
    probe->submitted.callback(probe->submitted.callback_arg);
  return probe->abort_result;
}

void test_uart_inline_completion_and_failed_submission_cleanup_share_tracking(void) {
  esy_uart_t uart = {0};
  uart_submission_probe_t probe = {.inline_completion = true};
  uart.ctx[1] = &probe;
  static const esy_uart_vtable_t k_probe_ops = {
      .send_dma = probe_uart_submit,
      .abort_dma = probe_uart_abort,
  };
  uart.ops = &k_probe_ops;
  uint8_t byte = 0u;
  unsigned completed = 0u;
  esy_dma_config_t transfer = {.src = &byte, .length = sizeof(byte),
      .callback = count_uart_completion, .callback_arg = &completed};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_send_dma(&uart, &transfer));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  probe.inline_completion = false;
  probe.submit_result = ESY_ERR_IO;
  probe.abort_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_uart_send_dma(&uart, &transfer));
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_send_blocking(&uart, &byte, 1u));
  probe.abort_result = ESY_OK;
  probe.complete_during_abort = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_abort_dma(&uart, true));
  TEST_ASSERT_EQUAL_UINT(2u, completed);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  probe.complete_during_abort = false;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_send_dma(&uart, &transfer));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
}

static void register_uart_stream_device(esy_uart_t *uart,
    esy_uart_device_binding_t *binding, esy_device_registry_t *registry,
    const esy_device_registry_static_resources_t *resources) {
  esy_device_registry_config_t config = {.static_resources = resources};
  esy_device_registration_config_t registration = {
      .name = "uart", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)};
  init_uart(uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(registry, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_register_device(
      binding, uart, registry, &registration));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(registry));
}

void test_uart_stream_failed_start_cleanup_retains_graph_until_stop_succeeds(void) {
  platform_test_set_dma_busy(false);
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_t registry = {0};
  esy_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  uint8_t bytes[8] = {0};
  uint32_t references = 0u;
  const uint8_t *borrowed = NULL;
  size_t length = 0u;
  register_uart_stream_device(&uart, &binding, &registry, &resources);
  platform_test_set_uart_stream_results(ESY_ERR_IO, ESY_ERR_TIMEOUT, true);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_uart_rx_stream_start(
      &uart, bytes, sizeof(bytes), rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->uart_stream_stop_calls);
  TEST_ASSERT_TRUE(binding.rx_stream_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_device_get_usage_count(&binding.device, &references));
  TEST_ASSERT_EQUAL_UINT32(1u, references);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_uart_rx_stream_peek(&uart, &borrowed, &length));
  TEST_ASSERT_EQUAL_PTR(bytes, borrowed);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_rx_stream_start(
      &uart, bytes, sizeof(bytes), rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->uart_stream_start_calls);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_device_get_usage_count(&binding.device, &references));
  TEST_ASSERT_EQUAL_UINT32(1u, references);
  platform_test_set_uart_stream_results(ESY_OK, ESY_OK, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_FALSE(binding.rx_stream_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_device_get_usage_count(&binding.device, &references));
  TEST_ASSERT_EQUAL_UINT32(0u, references);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_uart_rx_stream_peek(&uart, &borrowed, &length));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_unregister_device(&binding));
  esy_hal_uart_deinit(&uart);
}

void test_uart_stream_failed_start_successful_cleanup_preserves_submit_error(void) {
  platform_test_set_dma_busy(false);
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_t registry = {0};
  esy_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  uint8_t bytes[8] = {0};
  uint32_t references = 0u;
  register_uart_stream_device(&uart, &binding, &registry, &resources);
  platform_test_set_uart_stream_results(ESY_ERR_IO, ESY_OK, true);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_rx_stream_start(
      &uart, bytes, sizeof(bytes), rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->uart_stream_stop_calls);
  TEST_ASSERT_FALSE(binding.rx_stream_ref_held);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_device_get_usage_count(&binding.device, &references));
  TEST_ASSERT_EQUAL_UINT32(0u, references);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_unregister_device(&binding));
  esy_hal_uart_deinit(&uart);
}

void test_uart_stream_idle_rejections_release_graph_without_stop(void) {
  platform_test_set_dma_busy(false);
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_t registry = {0};
  esy_uart_device_binding_t binding = {0};
  esy_uart_t uart = {0};
  uint8_t bytes[8] = {0};
  uint32_t references = 0u;
  const esy_err_t rejections[] = {
      ESY_ERR_INVALID_ARG, ESY_ERR_NOT_SUPPORTED, ESY_ERR_NOT_IMPLEMENTED};
  register_uart_stream_device(&uart, &binding, &registry, &resources);
  for (size_t i = 0u; i < sizeof(rejections) / sizeof(rejections[0]); ++i) {
    platform_test_set_uart_stream_results(rejections[i], ESY_ERR_TIMEOUT, false);
    TEST_ASSERT_EQUAL(rejections[i], esy_hal_uart_rx_stream_start(
        &uart, bytes, sizeof(bytes), rx_stream_callback, NULL));
    TEST_ASSERT_FALSE(binding.rx_stream_ref_held);
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_device_get_usage_count(&binding.device, &references));
    TEST_ASSERT_EQUAL_UINT32(0u, references);
  }
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->uart_stream_stop_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_unregister_device(&binding));
  esy_hal_uart_deinit(&uart);
}

void test_uart_unregistered_stream_failed_start_preserves_storage_for_stop_retry(void) {
  esy_uart_t uart = {0};
  uint8_t bytes[8] = {0};
  const uint8_t *borrowed = NULL;
  size_t length = 0u;
  init_uart(&uart);
  platform_test_set_uart_stream_results(ESY_ERR_IO, ESY_ERR_TIMEOUT, true);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_uart_rx_stream_start(
      &uart, bytes, sizeof(bytes), rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->uart_stream_stop_calls);
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
      esy_hal_uart_receive_blocking(&uart, bytes, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_uart_rx_stream_start(
      &uart, bytes, sizeof(bytes), rx_stream_callback, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_uart_rx_stream_peek(&uart, &borrowed, &length));
  TEST_ASSERT_EQUAL_PTR(bytes, borrowed);
  platform_test_set_uart_stream_results(ESY_OK, ESY_OK, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_uart_rx_stream_peek(&uart, &borrowed, &length));
  esy_hal_uart_deinit(&uart);
}

void test_uart_stream_excludes_other_rx_until_successful_stop_but_allows_tx(void) {
  esy_uart_t uart = {0};
  uint8_t storage[8] = {0};
  uint8_t byte = 0u;
  esy_dma_config_t receive = {.dst = &byte, .length = sizeof(byte)};
  platform_test_set_dma_busy(false);
  init_uart(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_start(
      &uart, storage, sizeof(storage), rx_stream_callback, NULL));
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_receive_blocking(&uart, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_receive_dma(&uart, &receive));
  TEST_ASSERT_EQUAL(1, esy_hal_uart_send_blocking(&uart, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_deinit(&uart));
  platform_test_set_uart_stream_results(ESY_OK, ESY_ERR_TIMEOUT, false);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_receive_blocking(&uart, &byte, 1u));
  platform_test_set_uart_stream_results(ESY_OK, ESY_OK, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_stop(&uart));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(1, esy_hal_uart_receive_blocking(&uart, &byte, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&uart));
}

void test_uart_foreign_dma_does_not_block_idle_owner_configuration_or_pm(void) {
  esy_uart_t uart = {0};
  init_uart(&uart);
  platform_test_set_dma_busy(true);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_set_baudrate(&uart, 9600u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_suspend(&uart));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_resume(&uart));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&uart));
}

void test_uart_own_dma_and_physical_tx_remain_busy_without_raw_dma_activity(void) {
  esy_uart_t uart = {0};
  init_uart(&uart);
  platform_test_set_dma_busy(false);
  platform_test_set_uart_busy(true, false, false);
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_set_baudrate(&uart, 9600u));
  platform_test_set_uart_busy(false, true, false);
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_suspend(&uart));
  platform_test_set_uart_busy(false, false, true);
  TEST_ASSERT_TRUE(esy_hal_uart_is_busy(&uart, true));
  TEST_ASSERT_FALSE(esy_hal_uart_is_busy(&uart, false));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_uart_suspend(&uart));
  platform_test_set_uart_busy(false, false, false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_deinit(&uart));
}
