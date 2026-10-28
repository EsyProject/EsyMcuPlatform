/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_adc.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) {
  platform_test_reset();
}

void tearDown(void) {}

static void init_adc(esy_adc_t *adc) {
  esy_adc_config_t cfg = {
      .phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES,
      .inner = {
          .channel = 2u,
      },
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_init(adc, &cfg));
}

void test_esy_hal_adc_default_cfg_fills_sample_time_default(void) {
  esy_adc_config_t cfg = {0};

  esy_hal_adc_default_cfg(NULL);
  esy_hal_adc_default_cfg(&cfg);

  TEST_ASSERT_EQUAL_INT(ESY_ADC_SAMPLETIME_15CYCLES, cfg.sample_time);

  cfg.sample_time = ESY_ADC_SAMPLETIME_84CYCLES;
  esy_hal_adc_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_INT(ESY_ADC_SAMPLETIME_15CYCLES, cfg.sample_time);
}

void test_esy_hal_adc_contracts_are_enforced(void) {
  esy_adc_t adc = {0};
  uint16_t sample[2] = {0};
  uint8_t indexes[2] = {1u, 2u};
  esy_dma_config_t dma = {
      .dst = sample,
      .length = sizeof(sample),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  uint32_t caps = 0u;
  uint8_t index = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_init(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_init(&adc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_sample_once(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_adc_sample_once(&adc));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_start_dma(NULL, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_start_dma(&adc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_adc_start_dma(&adc, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_adc_start_continuous_dma(NULL, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_adc_start_continuous_dma(&adc, NULL));
  dma.length = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_adc_start_continuous_dma(&adc, &dma));
  dma.length = sizeof(sample);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_adc_start_continuous_dma(&adc, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_adc_start_multi_channel_dma(NULL, indexes, 2u, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_adc_start_multi_channel_dma(&adc, NULL, 2u, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_adc_start_multi_channel_dma(&adc, indexes, 0u, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_adc_start_multi_channel_dma(&adc, indexes, 2u, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_adc_start_multi_channel_dma(&adc, indexes, 2u, &dma));
  TEST_ASSERT_FALSE(esy_hal_adc_is_busy(NULL));
  TEST_ASSERT_FALSE(esy_hal_adc_is_busy(&adc));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_set_trigger(NULL,
                                                                            ESY_ADC_TRIGGER_EDGE_RISING,
                                                                            ESY_ADC_TRIGGER_SRC_TIM2_TRGO));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_adc_set_trigger(
                                                   &adc, ESY_ADC_TRIGGER_EDGE_RISING,
                                                   ESY_ADC_TRIGGER_SRC_TIM2_TRGO));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_get_caps(NULL, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_get_caps(&adc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_adc_get_caps(&adc, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_get_phys_index(NULL, &index));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_adc_get_phys_index(&adc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED, esy_hal_adc_get_phys_index(&adc, &index));

  init_adc(&adc);
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_TRUE(state->adc_available_order < state->board_configure_order);
  TEST_ASSERT_TRUE(state->board_configure_order < state->driver_init_order);
  TEST_ASSERT_NOT_NULL(adc.ops->sample_once);
  TEST_ASSERT_NOT_NULL(adc.ops->transfer_dma);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, adc.ops->transfer_dma(NULL, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, adc.ops->transfer_dma(&adc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          adc.ops->start_continuous_dma(NULL, &dma));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          adc.ops->start_continuous_dma(&adc, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          adc.ops->transfer_dma_multi_channel(NULL, &dma, indexes, 2u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          adc.ops->transfer_dma_multi_channel(&adc, NULL, indexes, 2u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          adc.ops->transfer_dma_multi_channel(&adc, &dma, NULL, 2u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          adc.ops->transfer_dma_multi_channel(&adc, &dma, indexes, 0u));
  TEST_ASSERT_EQUAL_INT32(1234, esy_hal_adc_sample_once(&adc));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_sample_calls);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_start_dma(&adc, &dma));
  state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL_INT(ESY_DMA_TRANSFER_SIZE_8, dma.transfer_size);
  TEST_ASSERT_EQUAL_INT(ESY_DMA_PERIPH_TO_MEM, state->last_adc_inner.direction);
  TEST_ASSERT_EQUAL_UINT32(TEST_ADC_DREQ, state->last_adc_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_ADC_CHANNEL, state->last_adc_inner.channel);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_start_continuous_dma(&adc, &dma));
  TEST_ASSERT_EQUAL_INT(ESY_DMA_TRANSFER_SIZE_8, dma.transfer_size);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_adc_start_multi_channel_dma(&adc, indexes, 2u, &dma));
  TEST_ASSERT_TRUE(esy_hal_adc_is_busy(&adc));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_set_trigger(
                                      &adc, ESY_ADC_TRIGGER_EDGE_RISING,
                                      ESY_ADC_TRIGGER_SRC_TIM2_TRGO));
  TEST_ASSERT_EQUAL(ESY_ADC_TRIGGER_EDGE_RISING,
                    platform_test_state()->last_adc_trigger_edge);
  TEST_ASSERT_EQUAL(ESY_ADC_TRIGGER_SRC_TIM2_TRGO,
                    platform_test_state()->last_adc_trigger_source);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_get_caps(&adc, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_ADC_CAP_DMA | ESY_ADC_CAP_SCAN | ESY_ADC_CAP_EXTERNAL_TRIGGER |
              ESY_ADC_CAP_CIRCULAR_DMA, caps);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_adc_get_phys_index(&adc, &index));
  TEST_ASSERT_EQUAL_UINT8(TEST_ADC_INDEX, index);

  platform_test_reset_order_counters();
  esy_hal_adc_deinit(&adc);
  state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT8(TEST_ADC_INDEX, state->reset_index);
  TEST_ASSERT_TRUE(state->driver_get_index_order < state->board_reset_order);
  TEST_ASSERT_TRUE(state->driver_deinit_order < state->board_reset_order);
}

void test_adc_dma_rejects_partial_samples_and_partial_scan_groups(void) {
  esy_adc_t adc = {0};
  uint16_t samples[4] = {0};
  uint8_t channels[] = {1u, 2u};
  esy_dma_config_t dma = {.dst = samples, .length = 3u};
  init_adc(&adc);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_adc_start_dma(&adc, &dma));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_adc_start_continuous_dma(&adc, &dma));
  dma.length = 6u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
      esy_hal_adc_start_multi_channel_dma(&adc, channels, 2u, &dma));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&adc));
}

void test_adc_abort_and_deinit_failure_retain_handle_for_retry(void) {
  esy_adc_t adc = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_adc_abort_dma(NULL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_abort_dma(&adc));
  init_adc(&adc);
  void *context = adc.ctx;
  platform_test_set_adc_results(ESY_OK, ESY_ERR_TIMEOUT, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_adc_deinit(&adc));
  TEST_ASSERT_EQUAL_PTR(context, adc.ctx);
  TEST_ASSERT_NOT_NULL(adc.ops->abort_dma);
  platform_test_reset_order_counters();
  platform_test_set_adc_results(ESY_OK, ESY_OK, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_adc_deinit(&adc));
  TEST_ASSERT_EQUAL_PTR(context, adc.ctx);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  platform_test_set_adc_results(ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&adc));
  TEST_ASSERT_NULL(adc.ctx);
  TEST_ASSERT_NULL(adc.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&adc));
}

void test_competing_adc_init_rejects_before_board_changes_and_allows_retry(void) {
  esy_adc_t owner = {0};
  esy_adc_t contender = {0};
  esy_adc_config_t config = {.phys_index = TEST_ADC_INDEX,
      .sample_time = ESY_ADC_SAMPLETIME_15CYCLES};
  init_adc(&owner);
  uint32_t configured = platform_test_state()->board_configure_order;
  uint32_t initialized = platform_test_state()->driver_init_calls;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_adc_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configured, platform_test_state()->board_configure_order);
  TEST_ASSERT_EQUAL_UINT32(initialized, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->board_reset_order);
  TEST_ASSERT_NULL(contender.ctx);
  TEST_ASSERT_NULL(contender.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&contender));
  TEST_ASSERT_EQUAL(1234, esy_hal_adc_sample_once(&owner));

  platform_test_set_adc_results(ESY_OK, ESY_OK, ESY_ERR_IO);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_adc_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_adc_init(&contender, &config));
  TEST_ASSERT_EQUAL_UINT32(configured, platform_test_state()->board_configure_order);
  platform_test_set_adc_results(ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&owner));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&contender, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&contender));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_init(&owner, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&owner));
}

void test_adc_native_ops_are_shared_and_trigger_state_is_per_handle(void) {
  esy_adc_t first = {0};
  esy_adc_t second = {0};
  init_adc(&first);
  const esy_adc_vtable_t *ops = first.ops;
  TEST_ASSERT_NOT_NULL(ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_set_trigger(
      &first, ESY_ADC_TRIGGER_EDGE_RISING, ESY_ADC_TRIGGER_SRC_TIM2_TRGO));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED, esy_hal_adc_sample_once(&second));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&first));
  TEST_ASSERT_NULL(first.ops);

  init_adc(&second);
  TEST_ASSERT_EQUAL_PTR(ops, second.ops);
  TEST_ASSERT_EQUAL(ESY_ADC_TRIGGER_EDGE_NONE,
                    platform_test_state()->last_adc_trigger_edge);
  TEST_ASSERT_EQUAL(ESY_ADC_TRIGGER_SRC_SOFTWARE,
                    platform_test_state()->last_adc_trigger_source);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_deinit(&second));
}

void test_adc_custom_immutable_ops_may_omit_optional_hooks(void) {
  static const esy_adc_vtable_t k_empty_ops = {0};
  esy_adc_t adc = {.ops = &k_empty_ops};
  uint32_t caps = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED, esy_hal_adc_sample_once(&adc));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_IMPLEMENTED, esy_hal_adc_get_caps(&adc, &caps));
  TEST_ASSERT_FALSE(esy_hal_adc_is_busy(&adc));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_adc_abort_dma(&adc));
}
