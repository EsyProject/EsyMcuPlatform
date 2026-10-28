/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_spi.h"
#include "unity.h"

typedef struct {
  unsigned calls;
  esy_spi_dma_transfer_status_t status;
  size_t tx_transferred;
  size_t rx_transferred;
} completion_t;

static void record_completion(esy_spi_t *spi,
    const esy_spi_dma_transfer_result_t *result, void *arg) {
  completion_t *completion = arg;
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_tx(spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_rx(spi));
  completion->calls++;
  completion->status = result->status;
  completion->tx_transferred = result->tx_transferred;
  completion->rx_transferred = result->rx_transferred;
}

void setUp(void) { fake_esy_hal_spi_reset(); }
void tearDown(void) {}

void test_spi_capabilities_follow_public_bits_and_configured_support(void) {
  esy_spi_t spi = {0};
  esy_spi_config_t config = {0};
  uint32_t caps = UINT32_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_get_caps(&spi, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED, esy_hal_spi_get_caps(&spi, &caps));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, caps);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&spi, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_get_caps(&spi, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_SPI_CAP_DMA_TRANSFER | ESY_SPI_CAP_TRANSFER16 |
                           ESY_SPI_CAP_DMA_REMAINING, caps);
  fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_NOT_SUPPORTED);
  fake_esy_hal_spi_set_get_dma_remaining_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_get_caps(&spi, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_SPI_CAP_TRANSFER16, caps);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));
}

void test_spi_fake_lifecycle_reports_invalid_registered_busy_and_repeat_init_states(void) {
  esy_spi_t spi = {0};
  esy_spi_config_t config = {0};
  uint8_t data[4] = {0};
  esy_dma_config_t transfer = {.src = data, .length = sizeof(data)};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_init(&spi, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&spi, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_init(&spi, &config));
  /* A non-NULL graph association must survive rejected local teardown. */
  fake_esy_hal_spi_set_device(&spi, (const esy_device_t *)(const void *)&config);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_spi_deinit(&spi));
  TEST_ASSERT_NOT_NULL(esy_hal_spi_get_device(&spi));
  fake_esy_hal_spi_set_device(&spi, NULL);
  fake_esy_hal_spi_set_transfer_dma_async(true);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_spi_transfer_dma(&spi, &transfer, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_deinit(&spi));
  TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_EQUAL_PTR(data, fake_esy_hal_spi_get_borrowed_tx(&spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_abort(&spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_init(&spi, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_deinit(&spi));
}

void test_failed_submission_default_rejects_without_borrow_or_callback(void) {
  esy_spi_t spi = {0};
  uint8_t data[8] = {0};
  esy_dma_config_t dma = {.src = data, .length = sizeof(data)};
  completion_t completion = {0};
  fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_transfer_dma(&spi, &dma, NULL,
      record_completion, &completion));
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_tx(&spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_poll(&spi));
  fake_esy_hal_spi_complete_last_dma(&spi);
  TEST_ASSERT_EQUAL_UINT(0u, completion.calls);
}

void test_retained_failure_preserves_buffers_across_poll_abort_failures_then_notifies_once(void) {
  esy_spi_t spi = {0};
  uint8_t tx[8] = {0}, rx[8] = {0}, replacement[8] = {0};
  esy_dma_config_t tx_dma = {.src = tx, .length = sizeof(tx)};
  esy_dma_config_t rx_dma = {.dst = rx, .length = sizeof(rx)};
  completion_t completion = {0}, rejected = {0};
  fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_IO);
  fake_esy_hal_spi_set_failed_submit_retains_borrow(true);
  fake_esy_hal_spi_set_poll_result(ESY_ERR_BUSY);
  fake_esy_hal_spi_set_abort_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_transfer_dma(&spi, &tx_dma,
      &rx_dma, record_completion, &completion));
  TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_EQUAL_PTR(tx, fake_esy_hal_spi_get_borrowed_tx(&spi));
  TEST_ASSERT_EQUAL_PTR(rx, fake_esy_hal_spi_get_borrowed_rx(&spi));
  tx_dma.src = replacement;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_transfer_dma(&spi, &tx_dma,
      NULL, record_completion, &rejected));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_poll(&spi));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_abort(&spi));
  TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_EQUAL_PTR(tx, fake_esy_hal_spi_get_borrowed_tx(&spi));
  TEST_ASSERT_EQUAL_UINT(0u, completion.calls);
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_hal_spi_get_poll_count(&spi));
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_hal_spi_get_abort_count(&spi));
  fake_esy_hal_spi_set_poll_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_poll(&spi));
  TEST_ASSERT_EQUAL_UINT(1u, completion.calls);
  TEST_ASSERT_EQUAL(ESY_SPI_DMA_TRANSFER_DMA_ERROR, completion.status);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_poll(&spi));
  fake_esy_hal_spi_complete_last_dma(&spi);
  TEST_ASSERT_EQUAL_UINT(1u, completion.calls);
  TEST_ASSERT_EQUAL_UINT(0u, rejected.calls);
}

void test_successful_abort_releases_retained_failure_without_a_late_callback(void) {
  esy_spi_t spi = {0};
  uint8_t data[8] = {0};
  esy_dma_config_t dma = {.src = data, .length = sizeof(data)};
  completion_t completion = {0};
  fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_IO);
  fake_esy_hal_spi_set_failed_submit_retains_borrow(true);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_transfer_dma(&spi, &dma,
      NULL, record_completion, &completion));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_abort(&spi));
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_tx(&spi));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_poll(&spi));
  fake_esy_hal_spi_complete_last_dma(&spi);
  TEST_ASSERT_EQUAL_UINT(0u, completion.calls);
  fake_esy_hal_spi_reset();
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_spi_get_poll_count(&spi));
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_spi_get_abort_count(&spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_tx(&spi));
}

void test_dma_rejects_empty_descriptors_before_injected_failure_can_borrow(void) {
  esy_spi_t spi = {0};
  uint8_t data[8] = {0};
  esy_dma_config_t tx = {.src = data, .length = sizeof(data)};
  esy_dma_config_t rx = {.length = sizeof(data)};
  completion_t completion = {0};
  fake_esy_hal_spi_set_transfer_dma_result(ESY_ERR_IO);
  fake_esy_hal_spi_set_failed_submit_retains_borrow(true);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(NULL,
      &tx, NULL, record_completion, &completion));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(&spi,
      NULL, NULL, record_completion, &completion));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(&spi,
      &tx, &rx, record_completion, &completion));
  tx.src = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_read_dma(&spi, &rx));
  TEST_ASSERT_FALSE(esy_hal_spi_is_busy(&spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_tx(&spi));
  TEST_ASSERT_NULL(fake_esy_hal_spi_get_borrowed_rx(&spi));
  TEST_ASSERT_EQUAL_UINT(0u, completion.calls);
}

void test_dma_width_length_alignment_and_duplex_contract_match_the_public_wrapper(void) {
  esy_spi_t spi = {0};
  uint16_t data[8] = {0};
  esy_dma_config_t tx = {.src = data, .length = 8u,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  esy_dma_config_t rx = {.dst = data, .length = 8u,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  fake_esy_hal_spi_set_write_dma_result(ESY_OK);
  fake_esy_hal_spi_set_read_dma_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_transfer_dma(&spi, &tx, NULL, NULL, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_transfer_dma(&spi, NULL, &rx, NULL, NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_write_dma(&spi, &tx));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_read_dma(&spi, &rx));
  tx.length = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.length = SIZE_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.length = 7u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.length = 8u;
  tx.src = (uint8_t *)data + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.src = data;
  tx.circular = true;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_spi_write_dma(&spi, &tx));
  tx.circular = false;
  tx.transfer_size = ESY_DMA_TRANSFER_SIZE_32;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_spi_write_dma(&spi, &tx));
  tx.transfer_size = (esy_dma_transfer_size_t)99;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_write_dma(&spi, &tx));
  tx.transfer_size = ESY_DMA_TRANSFER_SIZE_8;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(&spi, &tx, &rx, NULL, NULL));
  tx.transfer_size = ESY_DMA_TRANSFER_SIZE_16;
  tx.length = 6u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_spi_transfer_dma(&spi, &tx, &rx, NULL, NULL));
}

void test_blocking_halfword_counts_and_duplex_write_errors_match_public_contract(void) {
  esy_spi_t spi = {0};
  uint16_t tx[4] = {1u, 2u, 3u, 4u}, rx[4] = {0};
  TEST_ASSERT_EQUAL(4, esy_hal_spi_write16_blocking(&spi, tx, 4u));
  TEST_ASSERT_EQUAL(4, esy_hal_spi_read16_blocking(&spi, rx, 4u));
  TEST_ASSERT_EQUAL(4, esy_hal_spi_transfer16_blocking(&spi, tx, rx, 4u));
  fake_esy_hal_spi_set_write_blocking_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_transfer_blocking(&spi,
      (const uint8_t *)tx, (uint8_t *)rx, sizeof(tx)));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_spi_transfer16_blocking(&spi, tx, rx, 4u));
  fake_esy_hal_spi_set_write_blocking_result(ESY_OK);
  fake_esy_hal_spi_fail_write_after(0u, 3);
  TEST_ASSERT_EQUAL(3, esy_hal_spi_transfer_blocking(&spi,
      (const uint8_t *)tx, (uint8_t *)rx, sizeof(tx)));
  fake_esy_hal_spi_fail_write_after(0u, 3);
  TEST_ASSERT_EQUAL(1, esy_hal_spi_write16_blocking(&spi, tx, 4u));
}

void test_successful_pending_dma_cannot_be_overwritten_by_another_submission(void) {
  esy_spi_t spi = {0};
  uint8_t first[4] = {0}, second[4] = {0};
  esy_dma_config_t dma = {.src = first, .length = sizeof(first)};
  completion_t accepted = {0}, rejected = {0};
  fake_esy_hal_spi_set_transfer_dma_async(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_transfer_dma(&spi, &dma, NULL,
      record_completion, &accepted));
  dma.src = second;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_spi_transfer_dma(&spi, &dma, NULL,
      record_completion, &rejected));
  TEST_ASSERT_EQUAL_PTR(first, fake_esy_hal_spi_get_borrowed_tx(&spi));
  fake_esy_hal_spi_complete_last_dma(&spi);
  fake_esy_hal_spi_complete_last_dma(&spi);
  TEST_ASSERT_EQUAL(1u, accepted.calls);
  TEST_ASSERT_EQUAL(0u, rejected.calls);
}

void test_dma_remaining_uses_transfer_units_while_completion_reports_requested_bytes(void) {
  for (unsigned halfword = 0u; halfword < 2u; ++halfword) {
    for (unsigned directions = 1u; directions < 4u; ++directions) {
      esy_spi_t spi = {0};
      uint16_t tx_data[4] = {0}, rx_data[4] = {0};
      const esy_dma_transfer_size_t width = halfword ?
          ESY_DMA_TRANSFER_SIZE_16 : ESY_DMA_TRANSFER_SIZE_8;
      esy_dma_config_t tx = {.src = tx_data, .length = sizeof(tx_data),
          .transfer_size = width};
      esy_dma_config_t rx = {.dst = rx_data, .length = sizeof(rx_data),
          .transfer_size = width};
      completion_t completion = {0};
      size_t tx_remaining = SIZE_MAX, rx_remaining = SIZE_MAX;
      const size_t expected_units = halfword ? 4u : 8u;
      fake_esy_hal_spi_reset();
      fake_esy_hal_spi_set_transfer_dma_async(true);
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_transfer_dma(&spi,
          (directions & 1u) ? &tx : NULL, (directions & 2u) ? &rx : NULL,
          record_completion, &completion));
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_get_dma_trans_count_remaining(
          &spi, &tx_remaining, &rx_remaining));
      TEST_ASSERT_EQUAL((directions & 1u) ? expected_units : 0u, tx_remaining);
      TEST_ASSERT_EQUAL((directions & 2u) ? expected_units : 0u, rx_remaining);
      fake_esy_hal_spi_set_dma_remaining(&spi,
          (directions & 1u) ? 1u : 0u, (directions & 2u) ? 2u : 0u);
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_get_dma_trans_count_remaining(
          &spi, &tx_remaining, &rx_remaining));
      TEST_ASSERT_EQUAL((directions & 1u) ? 1u : 0u, tx_remaining);
      TEST_ASSERT_EQUAL((directions & 2u) ? 2u : 0u, rx_remaining);
      fake_esy_hal_spi_complete_last_dma(&spi);
      TEST_ASSERT_EQUAL_UINT(1u, completion.calls);
      TEST_ASSERT_EQUAL(ESY_SPI_DMA_TRANSFER_OK, completion.status);
      TEST_ASSERT_EQUAL((directions & 1u) ? sizeof(tx_data) : 0u,
          completion.tx_transferred);
      TEST_ASSERT_EQUAL((directions & 2u) ? sizeof(rx_data) : 0u,
          completion.rx_transferred);
      TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_get_dma_trans_count_remaining(
          &spi, &tx_remaining, &rx_remaining));
      TEST_ASSERT_EQUAL_UINT(0u, tx_remaining);
      TEST_ASSERT_EQUAL_UINT(0u, rx_remaining);
    }
  }
}

void test_explicit_dma_completion_reports_partial_bytes_and_retires_only_once(void) {
  esy_spi_t spi = {0};
  uint16_t data[4] = {0};
  esy_dma_config_t tx = {.src = data, .length = sizeof(data),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16};
  completion_t completion = {0};
  const esy_spi_dma_transfer_result_t partial = {
      .status = ESY_SPI_DMA_TRANSFER_DMA_ERROR, .tx_transferred = 6u};
  fake_esy_hal_spi_set_transfer_dma_async(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_spi_transfer_dma(&spi, &tx, NULL,
      record_completion, &completion));
  fake_esy_hal_spi_complete_last_dma_with_result(&spi, NULL);
  TEST_ASSERT_TRUE(esy_hal_spi_is_busy(&spi));
  fake_esy_hal_spi_complete_last_dma_with_result(&spi, &partial);
  TEST_ASSERT_EQUAL_UINT(1u, completion.calls);
  TEST_ASSERT_EQUAL(ESY_SPI_DMA_TRANSFER_DMA_ERROR, completion.status);
  TEST_ASSERT_EQUAL_UINT(6u, completion.tx_transferred);
  TEST_ASSERT_EQUAL_UINT(0u, completion.rx_transferred);
  fake_esy_hal_spi_complete_last_dma_with_result(&spi, &partial);
  TEST_ASSERT_EQUAL_UINT(1u, completion.calls);
}

void test_pm_write_requires_the_active_direct_parent_and_transitioning_requester(void) {
  esy_spi_t spi = {0};
  esy_device_registry_t registry = {0};
  esy_device_registry_t other_registry = {0};
  esy_device_t parent = {.registry = &registry};
  esy_device_t requester = {.registry = &registry};
  esy_device_t *dependencies[] = {&parent};
  const uint8_t data[] = {1u, 2u};
  atomic_init(&parent.state, ESY_DEVICE_STATE_ACTIVE);
  atomic_init(&requester.state, ESY_DEVICE_STATE_TRANSITIONING);
  requester.dependencies = dependencies;
  requester.dependency_count = 1u;
  TEST_ASSERT_NULL(esy_hal_spi_get_device(NULL));
  TEST_ASSERT_NULL(esy_hal_spi_get_device(&spi));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_blocking_for_pm(NULL, &requester, data, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_blocking_for_pm(&spi, NULL, data, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, NULL, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, data, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, data, sizeof(data)));
  fake_esy_hal_spi_set_device(&spi, &parent);
  const esy_spi_vtable_t *ops = spi.ops;
  TEST_ASSERT_EQUAL_PTR(&parent, esy_hal_spi_get_device(&spi));
  TEST_ASSERT_EQUAL(sizeof(data),
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, data, sizeof(data)));
  TEST_ASSERT_EQUAL_PTR(ops, spi.ops);
  TEST_ASSERT_EQUAL_UINT32(0u, atomic_load(&parent.usage_count));
  TEST_ASSERT_EQUAL_UINT32(0u, atomic_load(&requester.usage_count));
  atomic_store(&requester.state, ESY_DEVICE_STATE_ACTIVE);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, data, sizeof(data)));
  atomic_store(&requester.state, ESY_DEVICE_STATE_TRANSITIONING);
  requester.registry = &other_registry;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, data, sizeof(data)));
  requester.registry = &registry;
  requester.dependency_count = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, data, sizeof(data)));
  requester.dependency_count = 1u;
  atomic_store(&parent.state, ESY_DEVICE_STATE_RUNTIME_SUSPENDED);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_spi_write_blocking_for_pm(&spi, &requester, data, sizeof(data)));
  TEST_ASSERT_EQUAL_UINT(1u, fake_esy_hal_spi_get_tx_count(&spi));
}
