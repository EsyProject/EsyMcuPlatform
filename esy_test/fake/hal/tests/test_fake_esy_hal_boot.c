/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include "fake_esy_hal_boot.h"
#include "fake_esy_hal_system.h"

#define TEST_IMAGE_ADDRESS 0x10010000u

void setUp(void) {
  fake_esy_hal_boot_reset();
  fake_esy_hal_system_reset();
}

void tearDown(void) {}

void test_fake_boot_copies_independent_metadata_capacities_and_preserves_failed_output(void) {
  fake_esy_hal_boot_get_state()->layout = (esy_bootloader_layout_t){
      .meta_size = {0x10000u, 0x20000u}};
  esy_bootloader_layout_t layout = {0};
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_boot_get_layout(&layout));
  TEST_ASSERT_EQUAL_UINT32(0x10000u, layout.meta_size[0]);
  TEST_ASSERT_EQUAL_UINT32(0x20000u, layout.meta_size[1]);
  fake_esy_hal_boot_get_state()->result = ESY_ERR_IO;
  fake_esy_hal_boot_get_state()->layout.meta_size[0] = 0u;
  fake_esy_hal_boot_get_state()->layout.meta_size[1] = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_boot_get_layout(&layout));
  TEST_ASSERT_EQUAL_UINT32(0x10000u, layout.meta_size[0]);
  TEST_ASSERT_EQUAL_UINT32(0x20000u, layout.meta_size[1]);
}

void test_fake_boot_handoff_is_recorded_without_changing_system_state(void) {
  fake_esy_hal_system_get_ctx()->pc = 0x10000010u;
  fake_esy_hal_system_set_tick(123u);
  esy_hal_boot_jump_to_app(TEST_IMAGE_ADDRESS);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_boot_get_state()->jump_count);
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_ADDRESS,
                           fake_esy_hal_boot_get_state()->last_jump_address);
  TEST_ASSERT_EQUAL_UINT32(0x10000010u, fake_esy_hal_system_get_ctx()->pc);
  TEST_ASSERT_EQUAL_UINT32(123u, fake_esy_hal_system_get_ctx()->tick_ms);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_system_get_reset_count());
}

void test_fake_boot_reset_clears_handoff_observations(void) {
  esy_hal_boot_jump_to_app(TEST_IMAGE_ADDRESS);
  fake_esy_hal_boot_reset();
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_boot_get_state()->jump_count);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_boot_get_state()->last_jump_address);
  esy_hal_boot_jump_to_app(TEST_IMAGE_ADDRESS + 4u);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_hal_boot_get_state()->jump_count);
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_ADDRESS + 4u,
                           fake_esy_hal_boot_get_state()->last_jump_address);
}
