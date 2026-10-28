/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_usb_dev.h"
#include "esy_hal/esy_hal_usb_dev_device.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) { platform_test_reset(); }

void tearDown(void) {}

static void init_usb(esy_usb_dev_t *dev) {
  esy_usb_dev_config_t cfg = {
      .phys_index = TEST_USB_INDEX,
      .speed_hint = ESY_USB_SPEED_FULL,
      .ep0_mps = 64u,
  };

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_usb_dev_init(dev, &cfg));
}

void test_usb_lifecycles_reuse_immutable_ops_and_clear_the_retired_handle(void) {
  esy_usb_dev_t first = {0};
  esy_usb_dev_t second = {0};
  init_usb(&first);
  TEST_ASSERT_NOT_NULL(first.ops);
  const esy_usb_vtable_dev_t *provider = first.ops;
  esy_hal_usb_dev_deinit(&first);
  TEST_ASSERT_NULL(first.ops);
  init_usb(&second);
  TEST_ASSERT_EQUAL_PTR(provider, second.ops);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_usb_dev_set_address(&second, 4u));
  esy_hal_usb_dev_deinit(&second);
}

void test_esy_hal_usb_dev_contracts_are_enforced(void) {
  esy_usb_dev_t dev = {0};
  uint8_t buffer[8] = {0};
  esy_usb_xfer_t xfer = {
      .buf = buffer,
      .len = sizeof(buffer),
  };

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_init(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_set_address(NULL, 5u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_usb_dev_set_address(&dev, 5u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_ep_xfer(&dev, 0x81u, NULL));

  init_usb(&dev);
  const platform_test_state_t *state = platform_test_state();
  TEST_ASSERT_EQUAL_UINT32(1u, state->driver_init_calls);
  TEST_ASSERT_NOT_NULL(dev.ops->set_address);
  TEST_ASSERT_NOT_NULL(dev.ops->ep_xfer);
  TEST_ASSERT_NOT_NULL(dev.ops->get_caps);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_usb_dev_set_address(&dev, 5u));
  TEST_ASSERT_EQUAL_UINT8(5u, platform_test_state()->last_usb_addr);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_usb_dev_ep_xfer(&dev, 0x81u, &xfer));
  TEST_ASSERT_EQUAL_UINT32(1u,
                           platform_test_state()->driver_transfer_dma_calls);
  TEST_ASSERT_EQUAL_UINT8(0x81u, platform_test_state()->last_usb_ep_addr);
  TEST_ASSERT_EQUAL_UINT16(sizeof(buffer), platform_test_state()->last_usb_len);
}

void test_usb_device_adapter_is_cpu_idle_only(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_device_registration_config_t registration = {
      .name = "usb",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                              ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE),
  };
  esy_usb_dev_device_binding_t binding = {0};
  esy_usb_dev_t usb = {0};
  esy_power_state_t state;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  init_usb(&usb);
  registration.supported_state_mask |=
      ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_register_device(
                              &binding, &usb, &registry, &registration));
  registration.supported_state_mask &=
      ~ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_usb_dev_register_device(&binding, &usb, &registry,
                                                   &registration));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_get_deepest_allowed_state(&registry, &state));
  TEST_ASSERT_EQUAL_INT(ESY_POWER_STATE_CPU_IDLE, state);
}

void test_esy_hal_usb_dev_endpoint_and_query_apis_route_through_ops(void) {
  esy_usb_dev_t dev = {0};
  esy_usb_ep_cfg_t ep_cfg = {
      .ep_num = 1u,
      .dir = ESY_USB_DIR_IN,
      .type = ESY_USB_EP_BULK,
      .mps = 64u,
  };
  uint32_t caps = 0u;
  uint8_t index = 0u;

  init_usb(&dev);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_usb_dev_ep_init(&dev, &ep_cfg));
  TEST_ASSERT_EQUAL_UINT8(1u, platform_test_state()->last_usb_ep_cfg.ep_num);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_usb_dev_get_caps(&dev, &caps));
  TEST_ASSERT_EQUAL_UINT32(ESY_USB_CAP_DEVICE | ESY_USB_CAP_DMA, caps);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_usb_dev_get_phys_index(&dev, &index));
  TEST_ASSERT_EQUAL_UINT8(TEST_USB_INDEX, index);
}

void test_esy_hal_usb_dev_default_deinit_and_optional_ep_helpers(void) {
  esy_usb_dev_t dev = {0};
  esy_usb_dev_config_t cfg = {
      .phys_index = TEST_USB_INDEX,
      .speed_hint = ESY_USB_SPEED_FULL,
      .ep0_mps = 0u,
  };
  uint8_t buffer[8] = {0};

  esy_hal_usb_dev_default_cfg(NULL);
  esy_hal_usb_dev_default_cfg(&cfg);
  TEST_ASSERT_EQUAL_UINT16(64u, cfg.ep0_mps);

  esy_hal_usb_dev_board_init();
  TEST_ASSERT_NOT_EQUAL(0u, platform_test_state()->board_configure_order);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_usb_dev_init(&dev, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_usb_dev_ep_deinit(&dev, 0x81u));
  TEST_ASSERT_EQUAL_UINT8(0x81u, platform_test_state()->last_usb_ep_addr);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_hal_usb_dev_ep_stall(&dev, 0x81u, true));
  TEST_ASSERT_TRUE(platform_test_state()->last_bool_arg);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_usb_dev_ep_cancel(&dev, 0x81u));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_usb_dev_ep0_read(&dev, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_UINT16(sizeof(buffer), platform_test_state()->last_usb_len);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_hal_usb_dev_ep0_write(&dev, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_UINT16(sizeof(buffer), platform_test_state()->last_usb_len);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_usb_dev_ep0_status(&dev));

  esy_hal_usb_dev_deinit(&dev);
  TEST_ASSERT_NOT_EQUAL(0u, platform_test_state()->driver_deinit_order);
}

void test_esy_hal_usb_dev_rejects_missing_optional_ops_and_buffers(void) {
  esy_usb_dev_t dev = {0};
  esy_usb_xfer_t invalid_xfer = {
      .buf = NULL,
      .len = 1u,
  };
  uint32_t caps = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_get_caps(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_get_phys_index(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_ep_init(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_ep_deinit(NULL, 0x81u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_ep_stall(NULL, 0x81u, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_usb_dev_ep_xfer(
                                                   &dev, 0x81u, &invalid_xfer));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_ep_cancel(NULL, 0x81u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_ep0_read(&dev, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_ep0_write(&dev, NULL, 1u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_usb_dev_ep0_status(NULL));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_IMPLEMENTED,
                          esy_hal_usb_dev_get_caps(&dev, &caps));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_usb_dev_ep_deinit(&dev, 0x81u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_usb_dev_ep_stall(&dev, 0x81u, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_usb_dev_ep_cancel(&dev, 0x81u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_hal_usb_dev_ep0_status(&dev));
}
