/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_flash.h"

#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) {
  platform_test_reset();
}

void tearDown(void) {}

void test_esy_hal_flash_routes_operations_to_driver(void) {
  uint8_t buffer[4] = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_init());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_set_safe_execute(NULL));
  TEST_ASSERT_EQUAL_UINT32(
      1u, platform_test_state()->driver_flash_safe_execute_calls);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_read(0x1000u, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_read_calls);
  TEST_ASSERT_EQUAL_UINT32(0x1000u, platform_test_state()->last_address);
  TEST_ASSERT_EQUAL_UINT(sizeof(buffer), platform_test_state()->last_length);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_write(0x1004u, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_write_calls);
  TEST_ASSERT_EQUAL_UINT32(0x1004u, platform_test_state()->last_address);
  TEST_ASSERT_EQUAL_UINT(sizeof(buffer), platform_test_state()->last_length);

  TEST_ASSERT_TRUE(esy_hal_flash_is_valid_address(0x1000u));
  TEST_ASSERT_FALSE(esy_hal_flash_is_valid_address(0x2000u));

}

void test_esy_hal_flash_rejects_invalid_buffers_and_lengths(void) {
  uint8_t buffer[4] = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_flash_read(0x1000u, NULL, sizeof(buffer)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_flash_read(0x1000u, buffer, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_flash_write(0x1000u, NULL, sizeof(buffer)));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_hal_flash_write(0x1000u, buffer, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1000u, 0u));

  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_read_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_write_calls);
}

static esy_err_t test_flash_safe_execute(esy_hal_flash_operation_t operation,
                                        void *arg) {
  (void)operation;
  (void)arg;
  return ESY_OK;
}

void test_esy_hal_flash_propagates_hook_registration_result_and_callback(void) {
  platform_test_set_flash_safe_execute_result(ESY_ERR_NOT_SUPPORTED);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
      esy_hal_flash_set_safe_execute(test_flash_safe_execute));
  TEST_ASSERT_TRUE(platform_test_state()->driver_flash_safe_execute ==
                   test_flash_safe_execute);
  platform_test_set_flash_safe_execute_result(ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
      esy_hal_flash_set_safe_execute(test_flash_safe_execute));
  platform_test_set_flash_safe_execute_result(ESY_OK);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_hal_flash_set_safe_execute(test_flash_safe_execute));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_set_safe_execute(NULL));
  TEST_ASSERT_NULL(platform_test_state()->driver_flash_safe_execute);
  TEST_ASSERT_EQUAL_UINT32(4u, platform_test_state()->driver_flash_safe_execute_calls);
}

void test_esy_hal_flash_queries_geometry_before_init_and_routes_sector_address(void) {
  esy_flash_geometry_t geometry = {0};
  esy_flash_sector_t sector = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_get_geometry(&geometry));
  TEST_ASSERT_EQUAL_UINT32(0x1000u, geometry.base_address);
  TEST_ASSERT_EQUAL_UINT32(4096u, geometry.size_bytes);
  TEST_ASSERT_EQUAL_UINT32(4u, geometry.program_size);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_get_sector(0x1234u, &sector));
  TEST_ASSERT_EQUAL_UINT32(0x1234u, platform_test_state()->last_address);
  TEST_ASSERT_EQUAL_UINT32(0x1000u, sector.start_address);
  TEST_ASSERT_EQUAL_UINT32(4096u, sector.size_bytes);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_init_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_flash_geometry_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->driver_flash_sector_calls);
}

void test_esy_hal_flash_queries_reject_null_outputs_without_calling_backend(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_get_geometry(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_get_sector(0x1000u, NULL));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_flash_geometry_calls);
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->driver_flash_sector_calls);
}

void test_esy_hal_flash_queries_preserve_outputs_and_propagate_errors(void) {
  esy_flash_geometry_t geometry = {.base_address = 1u, .size_bytes = 2u,
                                   .program_size = 3u};
  esy_flash_sector_t sector = {.start_address = 4u, .size_bytes = 5u};

  platform_test_set_flash_query_results(ESY_ERR_NOT_SUPPORTED, ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_flash_get_geometry(&geometry));
  TEST_ASSERT_EQUAL_UINT32(1u, geometry.base_address);
  TEST_ASSERT_EQUAL_UINT32(2u, geometry.size_bytes);
  TEST_ASSERT_EQUAL_UINT32(3u, geometry.program_size);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_flash_get_sector(0x1000u, &sector));
  TEST_ASSERT_EQUAL_UINT32(4u, sector.start_address);
  TEST_ASSERT_EQUAL_UINT32(5u, sector.size_bytes);
  TEST_ASSERT_FALSE(esy_hal_flash_is_valid_address(0x1000u));

  platform_test_set_flash_query_results(ESY_OK, ESY_OK);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_get_sector(0x2000u, &sector));
  TEST_ASSERT_EQUAL_UINT32(4u, sector.start_address);
  TEST_ASSERT_EQUAL_UINT32(5u, sector.size_bytes);
  TEST_ASSERT_TRUE(esy_hal_flash_is_valid_address(0x1FFFu));
  TEST_ASSERT_FALSE(esy_hal_flash_is_valid_address(0x0FFFu));
  TEST_ASSERT_FALSE(esy_hal_flash_is_valid_address(0x2000u));
}
