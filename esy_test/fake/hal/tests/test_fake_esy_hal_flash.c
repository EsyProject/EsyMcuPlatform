/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_flash.h"
#include <string.h>
#include "unity.h"

void setUp(void) { fake_esy_hal_flash_reset(); }
void tearDown(void) {}

void test_flash_fake_reports_configured_geometry_and_address_bounds(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  esy_flash_geometry_t geometry;
  flash->storage_base_addr = 0x10000000u;
  flash->size_bytes = 0x20000u;
  flash->program_size = 256u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_get_geometry(&geometry));
  TEST_ASSERT_EQUAL_UINT32(0x10000000u, geometry.base_address);
  TEST_ASSERT_EQUAL_UINT32(0x20000u, geometry.size_bytes);
  TEST_ASSERT_EQUAL_UINT32(256u, geometry.program_size);
  TEST_ASSERT_TRUE(esy_hal_flash_is_valid_address(0x10000000u));
  TEST_ASSERT_TRUE(esy_hal_flash_is_valid_address(0x1001FFFFu));
  TEST_ASSERT_FALSE(esy_hal_flash_is_valid_address(0x10020000u));
  TEST_ASSERT_FALSE(esy_hal_flash_is_valid_address(0x0FFFFFFFu));
  TEST_ASSERT_EQUAL_UINT32(0u, flash->init_count);
}

void test_flash_fake_preserves_outputs_on_query_failures(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  esy_flash_geometry_t geometry = {1u, 2u, 3u};
  esy_flash_sector_t sector = {4u, 5u};
  flash->geometry_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_flash_get_geometry(&geometry));
  TEST_ASSERT_EQUAL_UINT32(1u, geometry.base_address);
  TEST_ASSERT_EQUAL_UINT32(2u, geometry.size_bytes);
  TEST_ASSERT_EQUAL_UINT32(3u, geometry.program_size);
  TEST_ASSERT_FALSE(esy_hal_flash_is_valid_address(0x1000u));
  flash->geometry_result = ESY_OK;
  flash->sector_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_hal_flash_get_sector(0x1000u, &sector));
  TEST_ASSERT_EQUAL_UINT32(4u, sector.start_address);
  TEST_ASSERT_EQUAL_UINT32(5u, sector.size_bytes);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_get_geometry(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_get_sector(0x1000u, NULL));
}

void test_flash_fake_resolves_nonuniform_sector_start_and_size(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  esy_flash_sector_t sector;
  flash->sector_size_after_address = 0x1040u;
  flash->sector_size_after = 64u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_get_sector(0x103Fu, &sector));
  TEST_ASSERT_EQUAL_UINT32(0x1030u, sector.start_address);
  TEST_ASSERT_EQUAL_UINT32(16u, sector.size_bytes);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_get_sector(0x1040u, &sector));
  TEST_ASSERT_EQUAL_UINT32(0x1040u, sector.start_address);
  TEST_ASSERT_EQUAL_UINT32(64u, sector.size_bytes);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_get_sector(0x1081u, &sector));
  TEST_ASSERT_EQUAL_UINT32(0x1080u, sector.start_address);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_get_sector(0x1100u, &sector));
  TEST_ASSERT_EQUAL_UINT32(0x1080u, sector.start_address);
}

void test_flash_fake_rejects_unknown_or_incomplete_sector_geometry(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  esy_flash_geometry_t geometry = {1u, 2u, 3u};
  esy_flash_sector_t sector = {4u, 5u};
  flash->program_size = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_flash_get_geometry(&geometry));
  TEST_ASSERT_EQUAL_UINT32(1u, geometry.base_address);
  flash->program_size = 1u;
  flash->size_bytes = 15u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_flash_get_sector(0x1000u, &sector));
  TEST_ASSERT_EQUAL_UINT32(4u, sector.start_address);
  flash->size_bytes = 256u;
  flash->sector_size_after = 64u;
  flash->sector_size_after_address = 0x1041u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_hal_flash_get_sector(0x1041u, &sector));
}

void test_flash_fake_enforces_write_granularity_and_exact_erase_boundaries(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  uint8_t data[8] = {0};
  flash->program_size = 4u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x1001u, data, 4u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x1000u, data, 3u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_write(0x1000u, data, sizeof(data)));
  flash->sector_size_after_address = 0x1040u;
  flash->sector_size_after = 64u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1001u, 16u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1000u, 15u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1030u, 32u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_flash_erase(0x1030u, 80u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(UINT32_MAX, 2u));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1000u, SIZE_MAX));
}

void test_flash_fake_transient_erase_failures_expire_without_changing_permanent_result(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  flash->erase_failures_remaining = 2u;
  flash->erase_transient_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EQUAL_UINT32(0u, flash->erase_failures_remaining);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1000u, 16u));
  flash->erase_result = ESY_ERR_FAIL;
  flash->erase_failures_remaining = 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EQUAL_UINT32(5u, flash->erase_count);
}

void test_flash_fake_erases_exact_bytes_and_enforces_nor_programming(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  const uint8_t first[] = {0xF0u, 0x0Fu};
  const uint8_t second[] = {0xE0u, 0x0Fu};
  const uint8_t invalid[] = {0xC0u, 0xFFu};
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, flash->storage, sizeof(flash->storage));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1000u, first, sizeof(first)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1000u, second, sizeof(second)));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_write(0x1000u, invalid, sizeof(invalid)));
  TEST_ASSERT_EQUAL_MEMORY(second, flash->storage, sizeof(second));
  flash->storage[16] = 0x55u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, flash->storage, 16u);
  TEST_ASSERT_EQUAL_HEX8(0x55u, flash->storage[16]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1000u, first, sizeof(first)));
}

void test_flash_fake_failed_erase_and_program_preserve_external_storage(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  uint8_t backing[64];
  memset(backing, 0, sizeof(backing));
  flash->backing_storage = backing;
  flash->backing_storage_size = sizeof(backing);
  flash->size_bytes = sizeof(backing);
  flash->erase_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, backing, sizeof(backing));
  flash->erase_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1000u, 17u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, backing, sizeof(backing));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1010u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, backing, 16u);
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, &backing[16], 16u);
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, &backing[32], 32u);
  flash->write_result = ESY_ERR_FAIL;
  const uint8_t data[] = {0x80u};
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_flash_write(0x1010u, data, sizeof(data)));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, &backing[16], 16u);
  flash->write_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_write(0x1000u, data, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1010u, data, sizeof(data)));
  flash->size_bytes = 128u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1040u, 16u));
  TEST_ASSERT_EQUAL_HEX8(0x80u, backing[16]);
}

void test_flash_fake_read_and_program_preflight_physical_geometry(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  const uint8_t data[2] = {0u, 0u};
  uint8_t output[2] = {0xA5u, 0xA5u};
  flash->size_bytes = 16u;

  TEST_ASSERT_FALSE(esy_hal_flash_is_valid_address(0x1010u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x1010u, data, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x100Fu, data, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x0FFFu, data, 1u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, flash->storage, sizeof(flash->storage));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_read(0x1010u, output, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_read(0x100Fu, output, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_read(0x0FFFu, output, 1u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xA5u, output, sizeof(output));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x100Fu, data, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_read(0x100Fu, output, 1u));
  TEST_ASSERT_EQUAL_HEX8(0u, output[0]);

  flash->geometry_result = ESY_ERR_NOT_SUPPORTED;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_flash_read(0x1000u, output, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_hal_flash_write(0x1000u, data, 1u));
  TEST_ASSERT_EQUAL_HEX8(0xFFu, flash->storage[0]);
  TEST_ASSERT_EQUAL_HEX8(0u, output[0]);
}

void test_flash_fake_read_program_and_erase_also_require_complete_backing_range(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  uint8_t backing[8];
  const uint8_t data[2] = {0u, 0u};
  uint8_t output[2] = {0xA5u, 0xA5u};
  memset(backing, 0xFF, sizeof(backing));
  memset(flash->storage, 0x3C, sizeof(flash->storage));
  flash->backing_storage = backing;
  flash->backing_storage_size = sizeof(backing);
  flash->storage_base_addr = 0x2100u;
  flash->size_bytes = 16u;

  TEST_ASSERT_TRUE(esy_hal_flash_is_valid_address(0x2108u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x2108u, data, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x2107u, data, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_read(0x2108u, output, 1u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_read(0x2107u, output, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x2100u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xA5u, output, sizeof(output));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, backing, sizeof(backing));
  TEST_ASSERT_EACH_EQUAL_UINT8(0x3Cu, flash->storage, sizeof(flash->storage));

  flash->size_bytes = 4u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x2104u, data, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x2103u, data, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_read(0x2103u, output, 1u));
  TEST_ASSERT_EQUAL_HEX8(0u, output[0]);
  TEST_ASSERT_EQUAL_HEX8(0u, backing[3]);
  TEST_ASSERT_EACH_EQUAL_UINT8(0x3Cu, flash->storage, sizeof(flash->storage));
}

void test_flash_fake_range_checks_do_not_wrap_at_the_end_of_address_space(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  const uint8_t data[2] = {0u, 0u};
  uint8_t output[2] = {0xA5u, 0xA5u};
  flash->storage_base_addr = UINT32_MAX - 15u;
  flash->size_bytes = 16u;
  TEST_ASSERT_TRUE(esy_hal_flash_is_valid_address(UINT32_MAX));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(UINT32_MAX, data, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(flash->storage_base_addr, data, SIZE_MAX));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_read(UINT32_MAX, output, 2u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_read(flash->storage_base_addr, output, SIZE_MAX));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xA5u, output, sizeof(output));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, flash->storage, sizeof(flash->storage));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(UINT32_MAX, data, 1u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_read(UINT32_MAX, output, 1u));
  TEST_ASSERT_EQUAL_HEX8(0u, output[0]);
  TEST_ASSERT_EQUAL_HEX8(0u, flash->storage[15]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(flash->storage_base_addr, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, flash->storage, sizeof(flash->storage));
}

void test_flash_fake_read_injection_selects_exact_call_and_copies_only_prefix(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  uint8_t output[4] = {0u};
  const uint8_t stored[] = {1u, 2u, 3u, 4u};
  memcpy(flash->storage, stored, sizeof(stored));
  flash->fail_read_call = 2u;
  flash->read_fail_bytes = 2u;
  flash->injected_error = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_read(0x1000u, output, sizeof(output)));
  memset(output, 0xA5, sizeof(output));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_read(0x1000u, output, sizeof(output)));
  TEST_ASSERT_EQUAL_UINT8(1u, output[0]);
  TEST_ASSERT_EQUAL_UINT8(2u, output[1]);
  TEST_ASSERT_EACH_EQUAL_UINT8(0xA5u, &output[2], 2u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_read(0x1000u, output, sizeof(output)));
  TEST_ASSERT_EQUAL_MEMORY(stored, output, sizeof(output));

  flash->fail_read_call = flash->read_count + 1u;
  flash->read_fail_bytes = 0u;
  memset(output, 0xA5, sizeof(output));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_read(0x1000u, output, sizeof(output)));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xA5u, output, sizeof(output));
  flash->fail_read_call = flash->read_count + 1u;
  flash->read_fail_bytes = SIZE_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_read(0x1000u, output, sizeof(output)));
  TEST_ASSERT_EQUAL_MEMORY(stored, output, sizeof(output));
}

void test_flash_fake_program_injection_preserves_exact_effect_before_error(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  const uint8_t data[4] = {0xE0u, 0xC0u, 0x80u, 0u};
  flash->fail_write_call = 2u;
  flash->write_fail_bytes = 2u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1000u, data, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_write(0x1004u, data, sizeof(data)));
  TEST_ASSERT_EQUAL_MEMORY(data, &flash->storage[4], 2u);
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, &flash->storage[6], 2u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1008u, data, sizeof(data)));

  flash->fail_write_call = flash->write_count + 1u;
  flash->write_fail_bytes = 0u;
  flash->injected_error = ESY_ERR_FAIL;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_flash_write(0x1010u, data, sizeof(data)));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, &flash->storage[16], sizeof(data));
  flash->fail_write_call = flash->write_count + 1u;
  flash->write_fail_bytes = SIZE_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_flash_write(0x1010u, data, sizeof(data)));
  TEST_ASSERT_EQUAL_MEMORY(data, &flash->storage[16], sizeof(data));
}

void test_flash_fake_erase_injection_changes_only_selected_prefix(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  memset(flash->storage, 0, sizeof(flash->storage));
  flash->fail_erase_call = 2u;
  flash->erase_fail_bytes = 5u;
  flash->injected_error = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_erase(0x1010u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, &flash->storage[16], 5u);
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, &flash->storage[21], 11u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1020u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, &flash->storage[32], 16u);
  flash->fail_erase_call = flash->erase_count + 1u;
  flash->erase_fail_bytes = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_erase(0x1030u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, &flash->storage[48], 16u);
  flash->fail_erase_call = flash->erase_count + 1u;
  flash->erase_fail_bytes = SIZE_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_erase(0x1030u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, &flash->storage[48], 16u);
}

void test_flash_fake_injection_runs_after_full_validation_and_existing_controls(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  const uint8_t data[4] = {0u};
  uint8_t output[4] = {0u};
  flash->fail_read_call = 1u;
  flash->fail_write_call = 1u;
  flash->fail_erase_call = 1u;
  flash->read_fail_bytes = SIZE_MAX;
  flash->write_fail_bytes = SIZE_MAX;
  flash->erase_fail_bytes = SIZE_MAX;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_read(0x1100u, output, sizeof(output)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x1100u, data, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1001u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, output, sizeof(output));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, flash->storage, sizeof(flash->storage));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1000u, data, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_read(0x1000u, output, sizeof(output)));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1000u, 16u));

  flash->fail_write_call = flash->write_count + 1u;
  flash->write_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_hal_flash_write(0x1000u, data, sizeof(data)));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, flash->storage, sizeof(data));
  flash->fail_erase_call = flash->erase_count + 1u;
  flash->erase_failures_remaining = 1u;
  flash->erase_transient_result = ESY_ERR_FAIL;
  memset(flash->storage, 0, 16u);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, flash->storage, 16u);
}

void test_flash_fake_optional_program_unit_marks_reject_repeated_programming(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  uint8_t marks[64] = {0u};
  const uint8_t erased[8] = {0xFFu, 0xFFu, 0xFFu, 0xFFu,
                             0xFFu, 0xFFu, 0xFFu, 0xFFu};
  flash->program_size = 4u;
  flash->programmed_units = marks;
  flash->programmed_units_size = sizeof(marks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1000u, erased, sizeof(erased)));
  TEST_ASSERT_EACH_EQUAL_UINT8(1u, marks, 2u);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_write(0x1004u, erased, 4u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, marks, 4u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1000u, erased, 4u));
  flash->fail_write_call = flash->write_count + 1u;
  flash->write_fail_bytes = 5u;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_write(0x1008u, erased, sizeof(erased)));
  TEST_ASSERT_EACH_EQUAL_UINT8(1u, &marks[2], 2u);
  flash->fail_erase_call = flash->erase_count + 1u;
  flash->erase_fail_bytes = 10u;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, marks, 2u);
  TEST_ASSERT_EACH_EQUAL_UINT8(1u, &marks[2], 2u);
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_write(0x1008u, erased, 4u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1008u, erased, sizeof(erased)));
}

void test_flash_fake_program_marks_preflight_capacity_and_preserve_caller_state_on_reset(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  uint8_t marks[2] = {0u};
  uint8_t backing[32];
  const uint8_t data[4] = {0u};
  memset(backing, 0xFF, sizeof(backing));
  flash->backing_storage = backing;
  flash->backing_storage_size = sizeof(backing);
  flash->program_size = 4u;
  flash->programmed_units = marks;
  flash->programmed_units_size = sizeof(marks);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_write(0x1008u, data, sizeof(data)));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, marks, sizeof(marks));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, backing, sizeof(backing));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1000u, data, sizeof(data)));
  fake_esy_hal_flash_reset();
  TEST_ASSERT_EQUAL_UINT8(1u, marks[0]);
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, backing, sizeof(data));
  TEST_ASSERT_NULL(flash->backing_storage);
  TEST_ASSERT_NULL(flash->programmed_units);
  TEST_ASSERT_EQUAL_UINT32(0u, flash->fail_write_call);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, flash->injected_error);
}

void test_flash_fake_program_marks_follow_physical_units_with_unaligned_geometry_origin(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  uint8_t marks[5] = {1u, 0u, 0u, 0u, 1u};
  const uint8_t data[4] = {0u};
  flash->storage_base_addr = 0x1001u;
  flash->size_bytes = 16u;
  flash->program_size = 4u;
  flash->programmed_units = marks;
  flash->programmed_units_size = sizeof(marks);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_write(0x1004u, data, sizeof(data)));
  TEST_ASSERT_EQUAL_UINT8(1u, marks[1]);
  flash->fail_erase_call = 1u;
  flash->erase_fail_bytes = 5u;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_erase(0x1001u, 16u));
  TEST_ASSERT_EQUAL_UINT8(1u, marks[0]);
  TEST_ASSERT_EQUAL_UINT8(1u, marks[1]);
  TEST_ASSERT_EQUAL_UINT8(1u, marks[4]);
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_flash_erase(0x1001u, 16u));
  TEST_ASSERT_EQUAL_UINT8(1u, marks[0]);
  TEST_ASSERT_EACH_EQUAL_UINT8(0u, &marks[1], 3u);
  TEST_ASSERT_EQUAL_UINT8(1u, marks[4]);
}

void test_flash_fake_injection_clamps_effects_and_always_reports_an_error(void) {
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  const uint8_t data[2] = {0u, 0u};
  uint8_t output[2] = {0xA5u, 0xA5u};
  flash->injected_error = ESY_OK;
  flash->fail_write_call = 1u;
  flash->write_fail_bytes = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_write(0x1000u, data, sizeof(data)));
  TEST_ASSERT_EQUAL_MEMORY(data, flash->storage, sizeof(data));
  TEST_ASSERT_EQUAL_HEX8(0xFFu, flash->storage[2]);
  flash->fail_read_call = 1u;
  flash->read_fail_bytes = 3u;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_read(0x1000u, output, sizeof(output)));
  TEST_ASSERT_EQUAL_MEMORY(data, output, sizeof(data));
  flash->storage[16] = 0u;
  flash->fail_erase_call = 1u;
  flash->erase_fail_bytes = 17u;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_hal_flash_erase(0x1000u, 16u));
  TEST_ASSERT_EACH_EQUAL_UINT8(0xFFu, flash->storage, 16u);
  TEST_ASSERT_EQUAL_HEX8(0u, flash->storage[16]);
}
