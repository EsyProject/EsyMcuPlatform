/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_dma.h"
#include "esy_hal/esy_hal_dma_device.h"

#include "hal_test_fakes.h"
#include "unity.h"

static const esy_dma_inner_config_t k_channels[] = {
    {.dreq = TEST_TX_DREQ, .channel = TEST_TX_CHANNEL},
    {.dreq = TEST_RX_DREQ, .channel = TEST_RX_CHANNEL},
};

static void init_dma(esy_dma_t *dma) {
  esy_dma_controller_config_t config = {
      .channels = k_channels,
      .channel_count = sizeof(k_channels) / sizeof(k_channels[0]),
  };
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_dma_init(dma, &config));
}

static void dma_callback(void *arg) {
  uint32_t *count = arg;

  (*count)++;
}

void setUp(void) { platform_test_reset(); platform_test_set_dma_busy(false); }

void tearDown(void) {}

void test_esy_hal_dma_prepare_fills_board_irq_metadata(void) {
  esy_dma_t dma = {0};
  esy_dma_config_t cfg = {
      .src = "abc",
      .length = 3u,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
      .inner =
          {
              .dreq = TEST_TX_DREQ,
              .channel = TEST_TX_CHANNEL,
          },
  };

  init_dma(&dma);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_dma_prepare(NULL, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_dma_prepare(&dma, &cfg));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_prepare_calls);
  TEST_ASSERT_EQUAL_UINT8(TEST_DMA_IRQ_INDEX, state->last_dma_inner.irq_index);
  TEST_ASSERT_EQUAL_UINT8(TEST_DMA_IRQ_PRIORITY,
                          state->last_dma_inner.irq_priority);
  TEST_ASSERT_EQUAL_UINT32(TEST_TX_DREQ, state->last_dma_inner.dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_TX_CHANNEL, state->last_dma_inner.channel);
}

void test_esy_hal_dma_start_single_prepares_then_starts_selected_channel(
    void) {
  esy_dma_t dma = {0};
  esy_dma_config_t cfg = {
      .dst = (void *)0x1000u,
      .length = 4u,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_16,
      .inner =
          {
              .dreq = TEST_RX_DREQ,
              .channel = TEST_RX_CHANNEL,
          },
  };

  init_dma(&dma);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_dma_start_single(NULL, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_dma_start_single(&dma, &cfg));

  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_prepare_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_start_calls);
  TEST_ASSERT_EQUAL_UINT32(1u << TEST_RX_CHANNEL, state->last_channel_mask);
}

void test_esy_hal_dma_rejects_invalid_control_args(void) {
  esy_dma_t dma = {0};
  size_t remaining = 0u;

  init_dma(&dma);
  esy_hal_dma_default_cfg(NULL);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_dma_start(&dma, 0u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_hal_dma_start(&dma, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_dma_start(&dma, 1u << 2u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_hal_dma_set_trans_count(&dma, TEST_RX_DREQ, TEST_RX_CHANNEL, 1u,
                                       true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_dma_get_trans_count_remaining(
                              &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_dma_get_trans_count_remaining(
                              &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, &remaining));
  TEST_ASSERT_EQUAL_UINT(17u, remaining);
}

void test_esy_hal_dma_default_and_forwarding_helpers_route_to_driver(
    void) {
  esy_dma_t dma = {0};
  esy_dma_config_t cfg = {
      .callback = (esy_dma_callback_t)0x1,
      .callback_arg = (void *)0x2,
      .half_callback = (esy_dma_callback_t)0x3,
      .half_callback_arg = (void *)0x4,
      .error_callback = (esy_dma_callback_t)0x5,
      .error_callback_arg = (void *)0x6,
      .circular = true,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_32,
      .src = (const void *)0x1000u,
      .dst = (void *)0x2000u,
  };
  size_t remaining = 0u;

  init_dma(&dma);

  esy_hal_dma_default_cfg(&cfg);
  TEST_ASSERT_NULL(cfg.callback);
  TEST_ASSERT_NULL(cfg.callback_arg);
  TEST_ASSERT_NULL(cfg.half_callback);
  TEST_ASSERT_NULL(cfg.half_callback_arg);
  TEST_ASSERT_NULL(cfg.error_callback);
  TEST_ASSERT_NULL(cfg.error_callback_arg);
  TEST_ASSERT_FALSE(cfg.circular);
  TEST_ASSERT_EQUAL_INT(ESY_DMA_TRANSFER_SIZE_8, cfg.transfer_size);
  TEST_ASSERT_NULL(cfg.src);
  TEST_ASSERT_NULL(cfg.dst);

  cfg.src = (const void *)0x1000u;
  cfg.dst = (void *)0x2000u;
  cfg.length = 9u;
  cfg.inner = k_channels[0];
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_dma_prepare(&dma, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_dma_start(&dma,
                                                 1u));
  TEST_ASSERT_EQUAL_UINT32(1u << TEST_TX_CHANNEL,
                           platform_test_state()->last_channel_mask);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_dma_abort(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_FALSE(
      esy_hal_dma_is_busy(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_hal_dma_bind_irq_to_core(&dma, TEST_DMA_IRQ_INDEX, 1u));

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &cfg));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_dma_set_read_addr(
                  &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, cfg.src, false));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_dma_set_write_addr(
                  &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, cfg.dst, false));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_dma_set_trans_count(
                  &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, 9u, true));
  TEST_ASSERT_EQUAL_UINT32(9u, platform_test_state()->last_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_dma_get_trans_count_remaining(
                              &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, &remaining));
  TEST_ASSERT_EQUAL_UINT(17u, remaining);

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_BUSY, esy_hal_dma_chain_to(
                  &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, 1u));

  esy_hal_dma_handle_irq(TEST_RX_DREQ, TEST_RX_CHANNEL);
  TEST_ASSERT_EQUAL_UINT32(TEST_RX_DREQ, platform_test_state()->last_dreq);
  TEST_ASSERT_EQUAL_UINT8(TEST_RX_CHANNEL, platform_test_state()->last_channel);
}

void test_esy_hal_dma_device_blocks_busy_channel_then_suspends(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_dma_inner_config_t channel = {
      .dreq = TEST_TX_DREQ,
      .channel = TEST_TX_CHANNEL,
  };
  esy_dma_device_config_t config = {
      .device =
          {
              .name = "dma-test",
              .supported_state_mask =
                  ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                  ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
          },
  };
  esy_dma_controller_config_t controller_config = {
      .channels = &channel,
      .channel_count = 1u,
  };
  esy_dma_t dma = {0};
  esy_device_state_t state;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_dma_init(&dma, &controller_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_dma_register_device(&dma, &registry, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  platform_test_set_dma_busy(true);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get_state(&dma.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);

  platform_test_set_dma_busy(false);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_runtime_suspend_idle(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get_state(&dma.device, &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get(&dma.device));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put(&dma.device));
}

typedef struct {
  esy_dma_t *dma;
  uint32_t calls;
} dma_callback_probe_t;

static void dma_lease_callback(void *arg) {
  dma_callback_probe_t *probe = arg;
  uint32_t usage = 0u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&probe->dma->device, &usage));
  TEST_ASSERT_EQUAL_UINT32(1u, usage);
  TEST_ASSERT_GREATER_THAN_UINT32(0u, platform_test_state()->dma_abort_calls);
  probe->calls++;
}

void test_registered_dma_transfer_holds_reference_until_completion(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_dma_device_config_t device_config = {
      .device =
          {
              .name = "dma-test",
              .supported_state_mask =
                  ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                  ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
          },
  };
  esy_dma_t dma = {0};
  esy_dma_config_t transfer = {
      .src = "abc",
      .dst = (void *)0x1000u,
      .length = 3u,
      .callback = dma_lease_callback,
      .error_callback = dma_lease_callback,
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
      .inner = {.dreq = TEST_TX_DREQ, .channel = TEST_TX_CHANNEL},
  };
  dma_callback_probe_t probe = {.dma = &dma};
  uint32_t usage_count = 0u;

  transfer.callback_arg = &probe;
  transfer.error_callback_arg = &probe;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  init_dma(&dma);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_hal_dma_register_device(&dma, &registry, &device_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&dma.device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);

  platform_test_complete_dma(false);
  TEST_ASSERT_EQUAL_UINT32(0u, probe.calls);
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
  platform_test_set_dma_results(ESY_OK, ESY_ERR_TIMEOUT, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&dma.device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);
  TEST_ASSERT_EQUAL_UINT32(0u, probe.calls);
  platform_test_set_dma_results(ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(1u, probe.calls);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&dma.device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(0u, usage_count);
}

void test_dma_route_indices_select_stm32_streams_with_shared_request_channel(void) {
  const esy_dma_inner_config_t routes[] = {
      {.dreq = 15u, .channel = 4u},
      {.dreq = 10u, .channel = 4u},
  };
  const esy_dma_controller_config_t config = {
      .channels = routes, .channel_count = 2u,
  };
  esy_dma_t dma = {0};
  esy_dma_config_t transfer = {.src = "data", .length = 4u,
                              .inner = routes[0]};
  platform_test_set_dma_resource_mapping(true, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&dma, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_EQUAL_HEX32(1u << 15u, platform_test_state()->last_channel_mask);
  transfer.inner = routes[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&dma, 1u << 1u));
  TEST_ASSERT_EQUAL_HEX32(1u << 10u, platform_test_state()->last_channel_mask);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_chain_to(&dma, 15u, 4u, 1u));
}

void test_dma_batch_start_translates_all_routes_before_start(void) {
  const esy_dma_inner_config_t routes[] = {
      {.dreq = 15u, .channel = 4u},
      {.dreq = 10u, .channel = 4u},
  };
  const esy_dma_controller_config_t config = {
      .channels = routes, .channel_count = 2u,
  };
  esy_dma_t dma = {0};
  esy_dma_config_t transfer = {.src = "data", .length = 4u,
                              .inner = routes[0]};
  platform_test_set_dma_resource_mapping(true, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&dma, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_dma_start(&dma, 3u));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_start_calls);
  transfer.inner = routes[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&dma, 3u));
  TEST_ASSERT_EQUAL_HEX32((1u << 15u) | (1u << 10u),
                        platform_test_state()->last_channel_mask);
}

void test_dma_init_rejects_physical_aliases_and_preserves_unused_handle(void) {
  esy_dma_inner_config_t routes[] = {
      {.dreq = 15u, .channel = 4u},
      {.dreq = 15u, .channel = 5u},
  };
  const esy_dma_controller_config_t config = {
      .channels = routes, .channel_count = 2u,
  };
  esy_dma_t dma = {0};
  platform_test_set_dma_resource_mapping(true, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_init(&dma, &config));
  TEST_ASSERT_FALSE(dma.initialized);
  TEST_ASSERT_NULL(dma.channels);
  routes[1].dreq = 10u;
  platform_test_set_dma_resource_mapping(true, ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_dma_init(&dma, &config));
  TEST_ASSERT_FALSE(dma.initialized);
  platform_test_set_dma_resource_mapping(true, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&dma, &config));
}

void test_dma_init_rejects_pico_routes_sharing_one_physical_channel(void) {
  const esy_dma_inner_config_t routes[] = {
      {.dreq = 1u, .channel = 5u},
      {.dreq = 2u, .channel = 5u},
  };
  const esy_dma_controller_config_t config = {
      .channels = routes, .channel_count = 2u,
  };
  esy_dma_t dma = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_init(&dma, &config));
  TEST_ASSERT_FALSE(dma.initialized);
}

void test_dma_error_priority_and_failed_retirement_preserve_storage(void) {
  esy_dma_t dma = {0};
  uint32_t done = 0u, error = 0u, half = 0u;
  esy_dma_config_t transfer = {
      .src = "abc", .length = 3u, .inner = k_channels[0],
      .callback = dma_callback, .callback_arg = &done,
      .half_callback = dma_callback, .half_callback_arg = &half,
      .error_callback = dma_callback, .error_callback_arg = &error,
  };
  init_dma(&dma);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  platform_test_half_complete_dma();
  platform_test_complete_dma(false);
  platform_test_complete_dma(true);
  platform_test_set_dma_busy(true);
  platform_test_set_dma_results(ESY_OK, ESY_ERR_TIMEOUT, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(0u, done + half + error);
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_deinit(&dma));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_set_trans_count(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, 1u, true));
  platform_test_set_dma_busy(false);
  platform_test_set_dma_results(ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(1u, error);
  TEST_ASSERT_EQUAL_UINT32(0u, done + half);
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(1u, error);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&dma));
}

void test_dma_circular_notifications_coalesce_until_error_termination(void) {
  esy_dma_t dma = {0};
  uint32_t done = 0u, half = 0u, error = 0u;
  esy_dma_config_t transfer = {
      .src = "abc", .length = 3u, .inner = k_channels[0], .circular = true,
      .callback = dma_callback, .callback_arg = &done,
      .half_callback = dma_callback, .half_callback_arg = &half,
      .error_callback = dma_callback, .error_callback_arg = &error,
  };
  init_dma(&dma);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  platform_test_half_complete_dma();
  platform_test_half_complete_dma();
  platform_test_complete_dma(false);
  TEST_ASSERT_EQUAL_UINT32(0u, done + half);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(1u, done);
  TEST_ASSERT_EQUAL_UINT32(1u, half);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->dma_abort_calls);
  TEST_ASSERT_TRUE(esy_hal_dma_is_busy(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
  platform_test_complete_dma(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_UINT32(1u, error);
  TEST_ASSERT_FALSE(esy_hal_dma_is_busy(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
}

void test_dma_start_and_control_errors_preserve_prepared_request(void) {
  esy_dma_t dma = {0};
  esy_dma_config_t transfer = {.src = "abc", .length = 3u,
                               .inner = k_channels[0]};
  init_dma(&dma);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  platform_test_set_dma_results(ESY_ERR_BUSY, ESY_OK, ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_start(&dma, 1u));
  TEST_ASSERT_TRUE(dma.runtime[0].prepared);
  TEST_ASSERT_FALSE(dma.runtime[0].started);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_dma_set_read_addr(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, "xyz", true));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_dma_set_write_addr(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, (void *)0x1000u, true));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_dma_set_trans_count(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, 1u, true));
  TEST_ASSERT_TRUE(dma.runtime[0].prepared);
  TEST_ASSERT_FALSE(dma.runtime[0].started);
  platform_test_set_dma_results(ESY_OK, ESY_OK, ESY_ERR_INVALID_ARG);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_set_trans_count(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, 65536u, true));
  TEST_ASSERT_TRUE(dma.runtime[0].prepared);
  platform_test_set_dma_results(ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_set_read_addr(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, "xyz", true));
  TEST_ASSERT_TRUE(dma.runtime[0].started);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_set_write_addr(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL, (void *)0x1000u, false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
}

void test_dma_chain_rejects_without_modifying_prepared_routes(void) {
  esy_dma_t dma = {0};
  esy_dma_config_t transfer = {.src = "abc", .length = 3u,
                               .inner = k_channels[0]};
  init_dma(&dma);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  transfer.inner = k_channels[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
                   esy_hal_dma_chain_to(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL, 1u));
  TEST_ASSERT_TRUE(dma.runtime[0].prepared && dma.runtime[1].prepared);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->dma_control_calls);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                   esy_hal_dma_chain_to(&dma, TEST_TX_DREQ, TEST_TX_CHANNEL, 2u));
}

void test_dma_poll_rejects_invalid_owner(void) {
  esy_dma_t dma = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_poll(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_dma_poll(&dma));
}

void test_dma_competing_controllers_preserve_prepared_request_and_callback(void) {
  esy_dma_t first = {0}, second = {0};
  uint32_t first_done = 0u, second_done = 0u;
  esy_dma_config_t transfer = {
      .src = "first", .length = 5u, .inner = k_channels[0],
      .callback = dma_callback, .callback_arg = &first_done,
  };
  init_dma(&first);
  init_dma(&second);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&first, &transfer));
  transfer.src = "second";
  transfer.length = 6u;
  transfer.callback_arg = &second_done;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL_PTR(&first,
                        platform_test_state()->dma_owners[TEST_TX_CHANNEL]);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_prepare_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(
      &second, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->dma_abort_calls);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_dma_set_trans_count(
      &second, TEST_TX_DREQ, TEST_TX_CHANNEL, 1u, true));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_hal_dma_start(&second, 1u));

  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&first, 1u));
  TEST_ASSERT_EQUAL_PTR(&first, platform_test_state()->last_dma_owner);
  platform_test_set_dma_busy(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&second));
  init_dma(&second);
  platform_test_set_dma_busy(false);
  platform_test_complete_dma(false);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&first));
  TEST_ASSERT_EQUAL_UINT32(1u, first_done);
  TEST_ASSERT_EQUAL_UINT32(0u, second_done);
  TEST_ASSERT_NULL(platform_test_state()->dma_owners[TEST_TX_CHANNEL]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&second, &transfer));
  platform_test_complete_dma(false);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&second));
  TEST_ASSERT_EQUAL_UINT32(1u, second_done);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&second));
}

void test_dma_physical_aliases_conflict_between_distinct_controller_routes(void) {
  const esy_dma_inner_config_t first_route = {.dreq = 1u, .channel = 5u};
  const esy_dma_inner_config_t second_route = {.dreq = 2u, .channel = 5u};
  const esy_dma_controller_config_t first_config = {
      .channels = &first_route, .channel_count = 1u,
  };
  const esy_dma_controller_config_t second_config = {
      .channels = &second_route, .channel_count = 1u,
  };
  esy_dma_t first = {0}, second = {0};
  esy_dma_config_t transfer = {
      .src = "bytes", .length = 5u, .inner = first_route,
  };
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&first, &first_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&second, &second_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&first, &transfer));
  transfer.inner = second_route;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&first, 1u, 5u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&second, 2u, 5u));
}

void test_dma_failed_start_and_retirement_retain_exclusive_physical_claim(void) {
  esy_dma_t first = {0}, second = {0};
  esy_dma_config_t transfer = {
      .src = "abc", .length = 3u, .inner = k_channels[0],
  };
  init_dma(&first);
  init_dma(&second);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&first, &transfer));
  platform_test_set_dma_results(ESY_ERR_TIMEOUT, ESY_ERR_TIMEOUT, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_dma_start(&first, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_hal_dma_abort(
      &first, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL_PTR(&first,
                        platform_test_state()->dma_owners[TEST_TX_CHANNEL]);
  platform_test_set_dma_results(ESY_OK, ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(
      &first, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(
      &second, TEST_TX_DREQ, TEST_TX_CHANNEL));
}

void test_dma_independent_streams_with_same_selector_retain_separate_owners(void) {
  const esy_dma_inner_config_t routes[] = {
      {.dreq = 15u, .channel = 4u}, {.dreq = 10u, .channel = 4u},
  };
  esy_dma_controller_config_t config = {.channels = routes, .channel_count = 1u};
  esy_dma_t first = {0}, second = {0};
  esy_dma_config_t transfer = {
      .src = "bytes", .length = 5u, .inner = routes[0],
  };
  platform_test_set_dma_resource_mapping(true, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&first, &config));
  config.channels = &routes[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_init(&second, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&first, &transfer));
  transfer.inner = routes[1];
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&first, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start(&second, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&first, 15u, 4u));
  TEST_ASSERT_EQUAL_PTR(&second, platform_test_state()->dma_owners[10u]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(&second, 10u, 4u));
}

void test_dma_peripheral_claim_blocks_generic_prepare_and_survives_idle_abort(void) {
  esy_dma_t dma = {0};
  const uint8_t peripheral_owner = 0u;
  esy_dma_config_t transfer = {
      .src = "abc", .length = 3u, .inner = k_channels[0],
  };
  init_dma(&dma);
  platform_test_set_dma_owner(TEST_TX_CHANNEL, &peripheral_owner);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_prepare(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_poll(&dma));
  TEST_ASSERT_EQUAL_PTR(&peripheral_owner,
                        platform_test_state()->dma_owners[TEST_TX_CHANNEL]);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->dma_abort_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_prepare_calls);
  platform_test_set_dma_owner(TEST_TX_CHANNEL, NULL);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_start_single(&dma, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(
      &dma, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&dma));
}

void test_dma_competing_prepare_releases_only_the_rejected_controllers_graph_ref(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t config;
  esy_device_registry_t registry = {0};
  esy_dma_t first = {0}, second = {0};
  esy_dma_device_config_t device_config = {
      .device = {.name = "first", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)},
  };
  esy_dma_config_t transfer = {
      .src = "abc", .length = 3u, .inner = k_channels[0],
  };
  uint32_t usage;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  config.static_resources = &resources;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &config));
  init_dma(&first);
  init_dma(&second);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_register_device(
      &first, &registry, &device_config));
  device_config.device.name = "second";
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_register_device(
      &second, &registry, &device_config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_prepare(&first, &transfer));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_dma_prepare(&second, &transfer));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&first.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(1u, usage);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&second.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(
      &second, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&first.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(1u, usage);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_abort(
      &first, TEST_TX_DREQ, TEST_TX_CHANNEL));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&first.device, &usage));
  TEST_ASSERT_EQUAL_UINT32(0u, usage);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_unregister_device(&first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_unregister_device(&second));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&first));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_dma_deinit(&second));
}
