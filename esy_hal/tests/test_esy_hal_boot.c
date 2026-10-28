/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_boot.h"
#include "esy_hal/esy_hal_system.h"
#include "hal_test_fakes.h"
#include "unity.h"

void setUp(void) { platform_test_reset(); }
void tearDown(void) {}

void test_esy_hal_boot_copies_board_layout_without_image_policy(void) {
  esy_bootloader_layout_t layout = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_boot_get_layout(&layout));
  TEST_ASSERT_EQUAL_UINT32(OTA_SLOT_A_ADDR, layout.slot_addr[0]);
  TEST_ASSERT_EQUAL_UINT32(OTA_SLOT_B_ADDR, layout.slot_addr[1]);
  TEST_ASSERT_EQUAL_UINT32(OTA_SLOT_A_META_ADDR, layout.meta_addr[0]);
  TEST_ASSERT_EQUAL_UINT32(OTA_SLOT_B_META_ADDR, layout.meta_addr[1]);
  TEST_ASSERT_EQUAL_UINT32(OTA_SLOT_TOTAL_SIZE, layout.slot_size);
  TEST_ASSERT_EQUAL_UINT32(0x1000u, layout.meta_size[0]);
  TEST_ASSERT_EQUAL_UINT32(0x2000u, layout.meta_size[1]);
}

void test_esy_hal_boot_rejects_null_and_preserves_output_on_failure(void) {
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_hal_boot_get_layout(NULL));
  esy_bootloader_layout_t layout = {.slot_size = 123u, .meta_size = {456u, 789u}};
  platform_test_set_boot_layout_available(false);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, esy_hal_boot_get_layout(&layout));
  TEST_ASSERT_EQUAL_UINT32(123u, layout.slot_size);
  TEST_ASSERT_EQUAL_UINT32(456u, layout.meta_size[0]);
  TEST_ASSERT_EQUAL_UINT32(789u, layout.meta_size[1]);
}

void test_esy_hal_boot_forwards_handoff_and_system_reports_diagnostic_pc(void) {
  platform_test_set_pc(OTA_SLOT_A_ADDR + 16u);
  TEST_ASSERT_EQUAL_UINT32(OTA_SLOT_A_ADDR + 16u, esy_hal_system_get_pc());
  esy_hal_boot_jump_to_app(OTA_SLOT_B_ADDR);
  TEST_ASSERT_EQUAL_UINT32(OTA_SLOT_B_ADDR, platform_test_state()->last_address);
}

void test_esy_hal_boot_uses_native_flash_address_without_changing_diagnostic_pc(void) {
  platform_test_set_pc(0x400D0100u);
  platform_test_set_boot_runtime((esy_boot_runtime_t){
      .mode = ESY_BOOT_MODE_MANAGED, .running_address = 0x10000u,
      .update_allowed = false}, ESY_OK);
  esy_boot_runtime_t runtime;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_boot_get_runtime(&runtime));
  TEST_ASSERT_EQUAL_UINT32(0x10000u, runtime.running_address);
  TEST_ASSERT_FALSE(runtime.update_allowed);
  TEST_ASSERT_EQUAL_UINT32(0x400D0100u, esy_hal_system_get_pc());
}

void test_esy_hal_boot_preserves_runtime_on_provider_failure_and_rejects_bad_mode(void) {
  esy_boot_runtime_t runtime = {.running_address = 123u};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_hal_boot_get_runtime(NULL));
  platform_test_set_boot_runtime((esy_boot_runtime_t){0}, ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_hal_boot_get_runtime(&runtime));
  TEST_ASSERT_EQUAL_UINT32(123u, runtime.running_address);
  platform_test_set_boot_runtime((esy_boot_runtime_t){.mode = (esy_boot_mode_t)99}, ESY_OK);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_hal_boot_get_runtime(&runtime));
  TEST_ASSERT_EQUAL_UINT32(123u, runtime.running_address);
}

void test_esy_hal_boot_validates_managed_partition_layout_before_returning_it(void) {
  esy_bootloader_layout_t layout = {.slot_size = 123u};
  platform_test_set_boot_runtime((esy_boot_runtime_t){
      .mode = ESY_BOOT_MODE_MANAGED}, ESY_OK);
  platform_test_set_boot_layout_result(ESY_ERR_INVALID_ARG);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_hal_boot_get_layout(&layout));
  TEST_ASSERT_EQUAL_UINT32(123u, layout.slot_size);
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->boot_layout_validate_calls);
  platform_test_set_boot_layout_result(ESY_OK);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_hal_boot_get_layout(&layout));
  TEST_ASSERT_EQUAL_UINT32(OTA_SLOT_TOTAL_SIZE, layout.slot_size);
  platform_test_set_boot_runtime((esy_boot_runtime_t){0}, ESY_ERR_NOT_SUPPORTED);
  layout.slot_size = 123u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, esy_hal_boot_get_layout(&layout));
  TEST_ASSERT_EQUAL_UINT32(123u, layout.slot_size);
}

void test_esy_hal_boot_validates_pending_range_and_propagates_native_errors(void) {
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_hal_boot_set_pending_image(0u, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                       esy_hal_boot_set_pending_image(UINT32_MAX, 2u));
  TEST_ASSERT_EQUAL_UINT32(0u, platform_test_state()->boot_pending_calls);
  platform_test_set_boot_action_results(ESY_ERR_IO, ESY_ERR_STATE);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_hal_boot_set_pending_image(0x10000u, 1024u));
  TEST_ASSERT_EQUAL_UINT32(0x10000u, platform_test_state()->last_address);
  TEST_ASSERT_EQUAL_UINT32(1024u, platform_test_state()->last_length);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_hal_boot_confirm_running_image());
  TEST_ASSERT_EQUAL_UINT32(1u, platform_test_state()->boot_confirm_calls);
}
