/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler.h"
#include "fake_esy_hal_uart.h"
#include "unity.h"
#include <string.h>

static esy_uart_t uart;
static esy_uart_tx_scheduler_t scheduler;
static esy_uart_tx_scheduler_tx_request_t slots[4];
static uint8_t bytes[4];
static unsigned completed;
static esy_uart_tx_scheduler_tx_status_t statuses[8];
static esy_err_t errors[8];
static bool resubmit;

static void done(const uint8_t *data, size_t length,
    esy_uart_tx_scheduler_tx_status_t status, esy_err_t error, void *arg) {
  (void)arg;
  TEST_ASSERT_EQUAL_PTR(bytes, data);
  TEST_ASSERT_EQUAL_size_t(sizeof(bytes), length);
  statuses[completed] = status;
  errors[completed++] = error;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_deinit(&scheduler));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_request_stop(&scheduler, true));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_request_abort(&scheduler));
  if (resubmit) {
    resubmit = false;
    esy_uart_tx_scheduler_tx_request_t request = {bytes, sizeof(bytes), done, NULL};
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_submit(&scheduler, &request));
  }
}
static esy_uart_tx_scheduler_tx_request_t request(void) {
  return (esy_uart_tx_scheduler_tx_request_t){bytes, sizeof(bytes), done, NULL};
}
static void init(void) {
  esy_uart_tx_scheduler_static_resources_t resources = {slots, 4u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_init(&scheduler, &uart, &resources));
}
static void submit(void) {
  esy_uart_tx_scheduler_tx_request_t req = request();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_submit(&scheduler, &req));
}
void setUp(void) {
  memset(&scheduler, 0, sizeof(scheduler));
  memset(&uart, 0, sizeof(uart));
  fake_esy_hal_uart_reset();
  fake_esy_hal_uart_set_send_dma_result(&uart, ESY_OK);
  completed = 0u;
  resubmit = false;
}
void tearDown(void) {}

void test_invalid_first_init_and_retry_and_owner_lifecycle(void) {
  esy_uart_tx_scheduler_static_resources_t resources = {slots, 4u};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_init(NULL, &uart, &resources));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_init(&scheduler, NULL, &resources));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_init(&scheduler, &uart, NULL));
  resources.request_count = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_init(&scheduler, &uart, &resources));
  resources.request_count = 4u;
  fake_esy_hal_uart_find(&uart)->tx_busy = true;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_init(&scheduler, &uart, &resources));
  fake_esy_hal_uart_find(&uart)->tx_busy = false;
  init();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_init(&scheduler, &uart, &resources));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
  init();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_invalid_submit_never_reserves_storage_and_full_queue_is_bounded(void) {
  esy_uart_tx_scheduler_tx_request_t req = request();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_submit(&scheduler, &req));
  init();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_submit(NULL, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_submit(&scheduler, NULL));
  req.length = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_submit(&scheduler, &req));
  req.length = SIZE_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_submit(&scheduler, &req));
  req = request(); req.on_complete = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_submit(&scheduler, &req));
  submit(); submit(); submit();
  req = request();
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_uart_tx_scheduler_submit(&scheduler, &req));
  TEST_ASSERT_EQUAL(0u, completed);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_stop(&scheduler, true));
  for (unsigned i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[i]);
  }
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_wire_idle_cannot_release_pending_dma_callback_or_start_next_transfer(void) {
  init(); submit(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  slot->tx_busy = false; /* Hardware idle before its terminal callback arrives. */
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(0u, completed);
  TEST_ASSERT_EQUAL(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(1u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_OK, statuses[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(2u, slot->send_dma_count);
  TEST_ASSERT_TRUE(esy_uart_tx_scheduler_is_busy(&scheduler));
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_dma_callback_does_not_release_bytes_still_on_wire(void) {
  init(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_complete_last_dma(&uart);
  fake_esy_hal_uart_find(&uart)->tx_busy = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(0u, completed);
  fake_esy_hal_uart_find(&uart)->tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(1u, completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_rejected_and_async_failed_tx_report_errors_once(void) {
  init(); submit();
  fake_esy_hal_uart_set_send_dma_result(&uart, ESY_ERR_BUSY);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_ERROR, statuses[0]);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, errors[0]);
  fake_esy_hal_uart_set_send_dma_result(&uart, ESY_OK);
  submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_fail_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_ERROR, statuses[1]);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, errors[1]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(2u, completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

static void complete_inline(esy_uart_t *device) {
  fake_esy_hal_uart_complete_last_dma(device);
}

void test_native_completion_waits_for_owner_callback_delivery_and_fifo_advance(void) {
  init(); submit(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_TRUE(esy_uart_tx_scheduler_is_busy(&scheduler));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT32(2u, slot->send_dma_count);
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(2u, completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}
void test_synchronous_dma_callback_is_deferred_to_next_owner_slice(void) {
  init(); submit();
  fake_esy_hal_uart_find(&uart)->send_dma_hook = complete_inline;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(0u, completed);
  resubmit = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(1u, completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(2u, completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_failed_submit_retains_queue_head_until_process_or_abort_retires_it(void) {
  for (unsigned retire_with_irq = 0u; retire_with_irq < 2u; ++retire_with_irq) {
    completed = 0u;
    init(); submit(); submit(); submit();
    fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
    const uint32_t sends_before = slot->send_dma_count;
    const uint32_t aborts_before = slot->abort_count;
    slot->send_dma_result = ESY_ERR_IO;
    slot->retain_failed_submission = true;
    slot->abort_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT(0u, completed);
    TEST_ASSERT_TRUE(esy_uart_tx_scheduler_is_busy(&scheduler));
    TEST_ASSERT_EQUAL_UINT32(aborts_before, slot->abort_count);
    esy_uart_tx_scheduler_tx_request_t extra = request();
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_uart_tx_scheduler_submit(&scheduler, &extra));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT32(aborts_before + 1u, slot->abort_count);
    TEST_ASSERT_EQUAL_UINT(0u, completed);
    TEST_ASSERT_EQUAL_UINT32(sends_before + 1u, slot->send_dma_count);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_stop(&scheduler, true));
    slot->abort_result = ESY_ERR_STATE;
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_TRUE(slot->tx_busy);
    TEST_ASSERT_EQUAL_UINT(0u, completed);
    TEST_ASSERT_EQUAL_UINT32(sends_before + 1u, slot->send_dma_count);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
    if (retire_with_irq != 0u) fake_esy_hal_uart_complete_last_dma(&uart);
    else slot->abort_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT(1u, completed);
    TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_ERROR, statuses[0]);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, errors[0]);
    TEST_ASSERT_FALSE(slot->tx_busy);
    fake_esy_hal_uart_complete_last_dma(&uart);
    TEST_ASSERT_EQUAL_UINT(1u, completed);
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT(3u, completed);
    TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[1]);
    TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[2]);
    TEST_ASSERT_EQUAL_UINT32(sends_before + 1u, slot->send_dma_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
  }
}

void test_stop_cancels_only_queued_requests_and_drains_active_dma(void) {
  init(); submit(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_stop(&scheduler, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_stop(&scheduler, false));
  esy_uart_tx_scheduler_tx_request_t req = request();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_submit(&scheduler, &req));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(0u, completed);
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_OK, statuses[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[1]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

static void rx_event(esy_uart_t *device, esy_uart_rx_stream_event_t event, void *arg) {
  (void)device; (void)event; (void)arg;
}
void test_rx_stream_remains_independent_while_tx_drains_and_deinitializes(void) {
  uint8_t rx[16];
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_start(&uart, rx, sizeof(rx), rx_event, NULL));
  init(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_stop(&scheduler, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_size_t(sizeof(bytes), fake_esy_hal_uart_push_rx_stream(&uart, bytes, sizeof(bytes)));
  const uint8_t *data; size_t length;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_peek(&uart, &data, &length));
  TEST_ASSERT_EQUAL_size_t(sizeof(bytes), length);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_consume(&uart, length));
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
  TEST_ASSERT_EQUAL_PTR(rx, fake_esy_hal_uart_find(&uart)->rx_stream_storage);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_stop(&uart));
}

void test_null_and_inactive_operations_are_explicit(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_process(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_request_stop(NULL, false));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_request_stop(&scheduler, false));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_request_abort(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_request_abort(&scheduler));
  TEST_ASSERT_FALSE(esy_uart_tx_scheduler_is_busy(NULL));
  TEST_ASSERT_FALSE(esy_uart_tx_scheduler_is_busy(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(NULL));
}

void test_abort_request_only_marks_state_and_cancels_one_active_or_queued_request_per_slice(void) {
  init(); submit(); submit(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_stop(&scheduler, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_stop(&scheduler, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_stop(&scheduler, true));
  TEST_ASSERT_EQUAL_UINT32(0u, slot->abort_count);
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  esy_uart_tx_scheduler_tx_request_t req = request();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_uart_tx_scheduler_submit(&scheduler, &req));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
  for (unsigned i = 0u; i < 3u; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT(i + 1u, completed);
    TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[i]);
    TEST_ASSERT_EQUAL(ESY_OK, errors[i]);
  }
  TEST_ASSERT_EQUAL_UINT32(1u, slot->abort_count);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_NULL(slot->last_dma_callback_arg);
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(3u, completed);
  TEST_ASSERT_FALSE(esy_uart_tx_scheduler_is_busy(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
  init(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_OK, statuses[3]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_active_abort_failure_retries_without_retiring_or_resubmitting_the_borrow(void) {
  init(); submit(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  slot->abort_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
  for (uint32_t i = 0u; i < 2u; ++i) {
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT32(i + 1u, slot->abort_count);
    TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
    TEST_ASSERT_EQUAL_UINT(0u, completed);
    TEST_ASSERT_EQUAL_PTR(bytes, slot->last_dma_src);
    TEST_ASSERT_NOT_NULL(slot->last_dma_callback_arg);
    TEST_ASSERT_TRUE(esy_uart_tx_scheduler_is_busy(&scheduler));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
  }
  slot->abort_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(2u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[1]);
  TEST_ASSERT_EQUAL_UINT32(3u, slot->abort_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

static void retain_wire_after_successful_abort(esy_uart_t *device) {
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(device);
  if (slot->abort_count != 0u && !slot->dma_pending) slot->tx_busy = true;
}

void test_successful_abort_retains_the_request_until_physical_wire_idle_without_aborting_again(void) {
  init(); submit(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  slot->busy_hook = retain_wire_after_successful_abort;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
  for (unsigned i = 0u; i < 3u; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT32(1u, slot->abort_count);
    TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
    TEST_ASSERT_EQUAL_UINT(0u, completed);
    TEST_ASSERT_TRUE(esy_uart_tx_scheduler_is_busy(&scheduler));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
  }
  TEST_ASSERT_NULL(slot->last_dma_callback_arg);
  slot->busy_hook = NULL;
  slot->tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(2u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[1]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

static void finish_during_busy_query(esy_uart_t *device) {
  fake_esy_hal_uart_find(device)->busy_hook = NULL;
  fake_esy_hal_uart_complete_last_dma(device);
}

void test_terminal_idle_completion_wins_an_abort_request_before_abort_has_succeeded(void) {
  for (unsigned failure = 0u; failure < 2u; ++failure) {
    completed = 0u;
    init(); submit();
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
    const uint32_t aborts = slot->abort_count;
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
    if (failure != 0u) fake_esy_hal_uart_fail_last_dma(&uart);
    else slot->busy_hook = finish_during_busy_query;
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT(1u, completed);
    TEST_ASSERT_EQUAL(failure != 0u ? ESY_UART_TX_SCHEDULER_TX_ERROR :
        ESY_UART_TX_SCHEDULER_TX_OK, statuses[0]);
    TEST_ASSERT_EQUAL(failure != 0u ? ESY_ERR_IO : ESY_OK, errors[0]);
    TEST_ASSERT_EQUAL_UINT32(aborts, slot->abort_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT(1u, completed);
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
  }
}

void test_failed_abort_does_not_override_a_later_terminal_result_and_state_alone_does_not_release(void) {
  init(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  slot->abort_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->abort_count);
  TEST_ASSERT_NOT_NULL(slot->last_dma_callback_arg);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_OK, statuses[0]);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->abort_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_explicit_abort_preserves_failed_retained_submission_error_until_idle(void) {
  init(); submit(); submit();
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  slot->send_dma_result = ESY_ERR_NO_MEM;
  slot->retain_failed_submission = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
  slot->abort_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  slot->abort_result = ESY_OK;
  slot->busy_hook = retain_wire_after_successful_abort;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  const uint32_t aborts = slot->abort_count;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT32(aborts, slot->abort_count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_uart_tx_scheduler_deinit(&scheduler));
  slot->busy_hook = NULL;
  slot->tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_ERROR, statuses[0]);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, errors[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(2u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[1]);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_explicit_abort_retires_native_dma_without_a_hal_progress_call(void) {
  init(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT32(1u, slot->abort_count);
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_CANCELLED, statuses[0]);
  TEST_ASSERT_NULL(slot->last_dma_callback_arg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_active_abort_and_queued_cancellation_leave_the_rx_stream_independent(void) {
  uint8_t rx[16];
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_start(&uart, rx, sizeof(rx), rx_event, NULL));
  init(); submit(); submit();
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_request_abort(&scheduler));
  TEST_ASSERT_EQUAL_size_t(sizeof(bytes), fake_esy_hal_uart_push_rx_stream(&uart, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  TEST_ASSERT_EQUAL_PTR(rx, slot->rx_stream_storage);
  TEST_ASSERT_EQUAL_UINT32(0u, slot->rx_stream_stop_count);
  const uint8_t *data = NULL;
  size_t length = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_peek(&uart, &data, &length));
  TEST_ASSERT_EQUAL_size_t(sizeof(bytes), length);
  TEST_ASSERT_EQUAL_MEMORY(bytes, data, length);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_consume(&uart, length));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_uart_rx_stream_stop(&uart));
}

void test_active_uart_observes_native_irq_completion_and_preserves_wire_tail(void) {
  init();
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, esy_uart_tx_scheduler_next_wait_ms(&scheduler));
  submit();
  TEST_ASSERT_EQUAL_UINT32(0u, esy_uart_tx_scheduler_next_wait_ms(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT32(1u, esy_uart_tx_scheduler_next_wait_ms(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  fake_esy_hal_uart_find(&uart)->keep_tx_busy_after_dma = true;
  fake_esy_hal_uart_complete_last_dma(&uart);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  TEST_ASSERT_EQUAL_UINT32(1u, esy_uart_tx_scheduler_next_wait_ms(&scheduler));
  fake_esy_hal_uart_find(&uart)->tx_busy = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, esy_uart_tx_scheduler_next_wait_ms(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_uart_sync_fallback_uses_default_budget_and_defers_success_callback(void) {
  init(); submit();
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
  slot->send_dma_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_dma_count);
  TEST_ASSERT_EQUAL_UINT32(1u, slot->send_timeout_count);
  TEST_ASSERT_EQUAL_UINT32(ESY_UART_TX_SCHEDULER_DEFAULT_TIMEOUT_MS, slot->last_timeout_ms);
  TEST_ASSERT_EQUAL_UINT(0u, completed);
  TEST_ASSERT_TRUE(esy_uart_tx_scheduler_is_busy(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
  TEST_ASSERT_EQUAL_UINT(1u, completed);
  TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_OK, statuses[0]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}

void test_uart_sync_fallback_respects_request_budget_and_reports_short_and_error_results(void) {
  const int32_t results[] = {2, ESY_ERR_TIMEOUT, ESY_ERR_IO};
  for (unsigned i = 0u; i < 3u; ++i) {
    init();
    esy_uart_tx_scheduler_tx_request_t req = request();
    req.timeout_ms = 37u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_submit(&scheduler, &req));
    fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_find(&uart);
    slot->send_dma_result = ESY_ERR_NOT_SUPPORTED;
    slot->override_send_result = true;
    slot->send_result = results[i];
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_process(&scheduler));
    TEST_ASSERT_EQUAL_UINT32(37u, slot->last_timeout_ms);
    TEST_ASSERT_EQUAL_UINT(i + 1u, completed);
    TEST_ASSERT_EQUAL(ESY_UART_TX_SCHEDULER_TX_ERROR, statuses[i]);
    TEST_ASSERT_EQUAL(results[i] < 0 ? results[i] : ESY_ERR_IO, errors[i]);
    TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
  }
}

void test_uart_sync_fallback_rejects_unbounded_budget_before_queueing(void) {
  init();
  esy_uart_tx_scheduler_tx_request_t req = request();
  req.timeout_ms = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_uart_tx_scheduler_submit(&scheduler, &req));
  TEST_ASSERT_FALSE(esy_uart_tx_scheduler_is_busy(&scheduler));
  TEST_ASSERT_EQUAL(ESY_OK, esy_uart_tx_scheduler_deinit(&scheduler));
}
