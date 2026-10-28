/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_dma.h"
#include "unity.h"

static const esy_dma_inner_config_t k_routes[] = {
    {.dreq = 10u, .channel = 5u},
    {.dreq = 11u, .channel = 6u},
};

static const esy_dma_inner_config_t k_shared_request_routes[] = {
    {.dreq = 15u, .channel = 4u},
    {.dreq = 10u, .channel = 4u},
};

static void init_controller(esy_dma_t *dma,
                            const esy_dma_inner_config_t *routes,
                            uint32_t count) {
  const esy_dma_controller_config_t config = {
      .channels = routes, .channel_count = count,
  };
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(dma, &config));
}

static esy_dma_config_t transfer_for(const esy_dma_inner_config_t *route,
                                     uint16_t *buffer, size_t bytes) {
  const esy_dma_config_t transfer = {
      .src = buffer, .dst = buffer, .length = bytes,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16, .inner = *route,
  };
  return transfer;
}

static size_t remaining(esy_dma_t *dma, const esy_dma_inner_config_t *route) {
  size_t count = SIZE_MAX;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_get_trans_count_remaining(
      dma, route->dreq, route->channel, &count));
  return count;
}

static void count_completion(void *arg) { ++*(uint32_t *)arg; }

void setUp(void) { fake_esy_hal_dma_reset(); }
void tearDown(void) {}

void test_fake_dma_start_mask_selects_configured_routes(void) {
  esy_dma_t dma = {0};
  const esy_dma_controller_config_t config = {
      .channels = k_routes, .channel_count = 2u,
  };
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&dma, &config));
  uint16_t samples[8] = {0};
  esy_dma_config_t transfer = transfer_for(&k_routes[1], samples, sizeof(samples));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&dma, 2u));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 10u, 5u));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, 11u, 6u));
}

void test_fake_dma_invalid_route_mask_starts_nothing(void) {
  esy_dma_t dma = {0};
  const esy_dma_controller_config_t config = {
      .channels = k_routes, .channel_count = 2u,
  };
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&dma, &config));
  uint16_t samples[8] = {0};
  esy_dma_config_t transfer = transfer_for(&k_routes[0], samples, sizeof(samples));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                    esy_hal_dma_start(&dma, 1u | (1u << 6u)));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 10u, 5u));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 11u, 6u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&dma, 1u));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, 10u, 5u));
}

void test_fake_dma_same_request_streams_keep_counts_and_abort_independent(void) {
  esy_dma_t dma = {0};
  uint16_t samples[8] = {0};
  init_controller(&dma, k_shared_request_routes, 2u);
  esy_dma_config_t transfer = transfer_for(
      &k_shared_request_routes[0], samples, sizeof(samples));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 10u, 4u));
  TEST_ASSERT_EQUAL_UINT32(8u, remaining(&dma, &k_shared_request_routes[0]));
  TEST_ASSERT_EQUAL_UINT32(0u, remaining(&dma, &k_shared_request_routes[1]));
  transfer.inner = k_shared_request_routes[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_set_remaining(&dma, 15u, 4u, 3u));
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_set_remaining(&dma, 10u, 4u, 5u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&dma, 15u, 4u));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, 10u, 4u));
  TEST_ASSERT_EQUAL_UINT32(3u, remaining(&dma, &k_shared_request_routes[0]));
  TEST_ASSERT_EQUAL_UINT32(5u, remaining(&dma, &k_shared_request_routes[1]));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_deinit(&dma));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&dma, 10u, 4u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&dma));
  init_controller(&dma, k_shared_request_routes, 2u);
  TEST_ASSERT_EQUAL_UINT32(0u, remaining(&dma, &k_shared_request_routes[0]));
}

void test_fake_dma_owner_is_part_of_route_identity(void) {
  esy_dma_t first = {0}, second = {0};
  uint16_t samples[8] = {0};
  init_controller(&first, k_shared_request_routes, 1u);
  init_controller(&second, k_shared_request_routes, 1u);
  esy_dma_config_t transfer = transfer_for(
      &k_shared_request_routes[0], samples, sizeof(samples));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&first, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_set_remaining(&first, 15u, 4u, 3u));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&second, 15u, 4u));
  TEST_ASSERT_EQUAL_UINT32(0u, remaining(&second, &k_shared_request_routes[0]));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&second, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&first, 15u, 4u));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&second, 15u, 4u));
  TEST_ASSERT_EQUAL_UINT32(8u, remaining(&second, &k_shared_request_routes[0]));
  size_t count = 123u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_get_trans_count_remaining(
      &second, 15u, 5u, &count));
  TEST_ASSERT_EQUAL_UINT32(123u, count);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      fake_esy_hal_dma_set_remaining(&second, 10u, 4u, 99u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_abort(&second, 10u, 4u));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&second, 15u, 4u));
}

void test_fake_dma_chain_is_unsupported_and_preserves_both_routes(void) {
  esy_dma_t dma = {0};
  uint16_t samples[8] = {0};
  init_controller(&dma, k_shared_request_routes, 2u);
  for (uint8_t i = 0u; i < 2u; ++i) {
    esy_dma_config_t transfer = transfer_for(
        &k_shared_request_routes[i], samples, sizeof(samples));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  }
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_chain_to(&dma, 15u, 5u, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_chain_to(&dma, 15u, 4u, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_dma_chain_to(&dma, 15u, 4u, 1u));
  TEST_ASSERT_TRUE(dma.runtime[0].prepared && dma.runtime[1].prepared);
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 10u, 4u));
}

void test_fake_dma_start_single_checks_pair_and_shares_start_failure_semantics(void) {
  esy_dma_t dma = {0};
  uint16_t samples[8] = {0};
  init_controller(&dma, k_shared_request_routes, 2u);
  esy_dma_config_t transfer = transfer_for(
      &k_shared_request_routes[0], samples, sizeof(samples));
  transfer.inner.channel = 5u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_EQUAL_UINT32(0u, remaining(&dma, &k_shared_request_routes[0]));
  transfer.inner = k_shared_request_routes[0];
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_dma_start(&dma, 1u));
  fake_esy_hal_dma_set_prepare_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_dma_start_single(&dma, &transfer));
  fake_esy_hal_dma_set_prepare_result(ESY_OK);
  fake_esy_hal_dma_set_start_single_result(ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_EQUAL_UINT32(0u, remaining(&dma, &k_shared_request_routes[0]));
  fake_esy_hal_dma_set_start_single_result(ESY_OK);
  fake_esy_hal_dma_set_start_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  fake_esy_hal_dma_set_start_result(ESY_OK);
  TEST_ASSERT_TRUE(dma.runtime[0].prepared);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&dma, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, 15u, 4u));
}

void test_fake_dma_batch_validation_does_not_start_prepared_subset(void) {
  esy_dma_t dma = {0};
  uint16_t samples[8] = {0};
  init_controller(&dma, k_shared_request_routes, 2u);
  esy_dma_config_t transfer = transfer_for(
      &k_shared_request_routes[0], samples, sizeof(samples));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_dma_start(&dma, 3u));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 10u, 4u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&dma, 1u));
}

void test_fake_dma_capacity_failure_preserves_handle_and_deinit_releases_slots(void) {
  esy_dma_t first = {0}, second = {0}, extra = {0};
  esy_dma_inner_config_t routes[ESY_HAL_DMA_MAX_TRACKED_CHANNELS];
  for (uint32_t i = 0u; i < ESY_HAL_DMA_MAX_TRACKED_CHANNELS; ++i)
    routes[i] = (esy_dma_inner_config_t){.dreq = i, .channel = 4u};
  init_controller(&first, routes, ESY_HAL_DMA_MAX_TRACKED_CHANNELS);
  init_controller(&second, routes, ESY_HAL_DMA_MAX_TRACKED_CHANNELS);
  const esy_dma_controller_config_t config = {
      .channels = k_routes, .channel_count = 1u,
  };
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_hal_dma_init(&extra, &config));
  TEST_ASSERT_FALSE(extra.initialized);
  TEST_ASSERT_NULL(extra.channels);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&extra, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_dma_init(&extra, &config));
}

void test_fake_dma_duplicate_pair_init_preserves_unused_handle_for_retry(void) {
  esy_dma_t dma = {0};
  esy_dma_inner_config_t routes[] = {
      {.dreq = 15u, .channel = 4u}, {.dreq = 15u, .channel = 4u},
  };
  const esy_dma_controller_config_t config = {
      .channels = routes, .channel_count = 2u,
  };
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_init(&dma, &config));
  TEST_ASSERT_FALSE(dma.initialized);
  TEST_ASSERT_NULL(dma.channels);
  routes[1].dreq = 10u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&dma, &config));
}

void test_fake_dma_control_trigger_uses_owner_pair_and_requires_prepare(void) {
  esy_dma_t dma = {0};
  uint16_t samples[8] = {0};
  init_controller(&dma, k_shared_request_routes, 2u);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,
      esy_hal_dma_set_trans_count(&dma, 15u, 4u, 4u, true));
  TEST_ASSERT_EQUAL_UINT32(0u, remaining(&dma, &k_shared_request_routes[0]));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_dma_set_read_addr(&dma, 15u, 5u, samples, true));
  esy_dma_config_t transfer = transfer_for(
      &k_shared_request_routes[0], samples, sizeof(samples));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_dma_set_write_addr(&dma, 15u, 4u, samples, false));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_hal_dma_set_read_addr(&dma, 15u, 4u, samples, true));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 10u, 4u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_set_trans_count(&dma, 15u, 4u, 2u, false));
  TEST_ASSERT_EQUAL_UINT32(8u, remaining(&dma, &k_shared_request_routes[0]));
  TEST_ASSERT_EQUAL_UINT32(0u, remaining(&dma, &k_shared_request_routes[1]));
}

void test_fake_dma_irq_waits_for_owner_poll_before_terminal_callback(void) {
  esy_dma_t dma = {0};
  uint16_t samples[8] = {0};
  uint32_t callbacks = 0u;
  init_controller(&dma, k_shared_request_routes, 2u);
  esy_dma_config_t transfer = transfer_for(
      &k_shared_request_routes[0], samples, sizeof(samples));
  transfer.callback = count_completion;
  transfer.callback_arg = &callbacks;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  esy_hal_dma_handle_irq(15u, 4u);
  TEST_ASSERT_EQUAL_UINT32(0u, callbacks);
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_deinit(&dma));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_set_trans_count(&dma, 15u, 4u, 1u, true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(1u, callbacks);
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(1u, callbacks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&dma));
}

void test_fake_dma_error_beats_completion_and_retirement_failure_is_retryable(void) {
  esy_dma_t dma = {0};
  uint16_t samples[8] = {0};
  uint32_t done = 0u, error = 0u;
  init_controller(&dma, k_shared_request_routes, 1u);
  esy_dma_config_t transfer = transfer_for(
      &k_shared_request_routes[0], samples, sizeof(samples));
  transfer.callback = count_completion;
  transfer.callback_arg = &done;
  transfer.error_callback = count_completion;
  transfer.error_callback_arg = &error;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, fake_esy_hal_dma_signal_error(&dma, 15u, 4u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  esy_hal_dma_handle_irq(15u, 4u);
  TEST_ASSERT_EQUAL(ESY_OK, fake_esy_hal_dma_signal_error(&dma, 15u, 4u));
  fake_esy_hal_dma_set_abort_result(ESY_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_dma_abort(&dma, 15u, 4u));
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, 15u, 4u));
  TEST_ASSERT_EQUAL_UINT32(0u, error + done);
  fake_esy_hal_dma_set_abort_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(1u, error);
  TEST_ASSERT_EQUAL_UINT32(0u, done);
}

void test_fake_dma_control_failure_preserves_prepared_transfer(void) {
  esy_dma_t dma = {0};
  uint16_t samples[8] = {0};
  init_controller(&dma, k_shared_request_routes, 1u);
  esy_dma_config_t transfer = transfer_for(
      &k_shared_request_routes[0], samples, sizeof(samples));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  fake_esy_hal_dma_set_control_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
      esy_hal_dma_set_read_addr(&dma, 15u, 4u, samples, true));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
      esy_hal_dma_set_write_addr(&dma, 15u, 4u, samples, true));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
      esy_hal_dma_set_trans_count(&dma, 15u, 4u, 2u, true));
  TEST_ASSERT_TRUE(dma.runtime[0].prepared);
  TEST_ASSERT_FALSE(dma.runtime[0].started);
  TEST_ASSERT_EQUAL_UINT32(8u, remaining(&dma, &k_shared_request_routes[0]));
  fake_esy_hal_dma_set_control_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&dma, 1u));
}
