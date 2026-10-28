/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <limits.h>
#include <string.h>

#include "esy_middleware/dual_flash_store/esy_dual_flash_store.h"
#include "fake_esy_hal_flash.h"

#define TEST_FLASH_BASE 0x10000u
#define TEST_FLASH_BYTES 16384u
#define TEST_REGION_BYTES 4096u
#define TEST_REGION_OFFSET 1024u
#define TEST_PROGRAM_BYTES 16u
#define TEST_HEADER_BYTES 32u
#define TEST_DATA_KIND 1u
#define TEST_TOMBSTONE_KIND 2u

typedef struct {
  esy_dual_flash_store_t store;
  esy_dual_flash_store_t copied;
  esy_dual_flash_store_static_resources_t resources;
  esy_dual_flash_store_config_t config;
  fake_esy_hal_flash_state_t *flash;
  uint8_t media[TEST_FLASH_BYTES];
  uint8_t saved[TEST_FLASH_BYTES];
  uint8_t work[1024];
  uint8_t data[TEST_REGION_BYTES];
  uint8_t output[TEST_REGION_BYTES];
  uint8_t marks[TEST_FLASH_BYTES];
} test_ctx_t;

static test_ctx_t s_ctx;

static uint32_t fixture_crc(const uint8_t *bytes, size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0u; index < length; ++index) {
    crc ^= bytes[index];
    for (uint32_t bit = 0u; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^ ((crc & 1u) != 0u ? 0xedb88320u : 0u);
    }
  }
  return ~crc;
}

static void fixture_u32(uint8_t *bytes, uint32_t value) {
  for (uint32_t index = 0u; index < 4u; ++index) {
    bytes[index] = (uint8_t)(value >> (8u * index));
  }
}

static size_t header_span(void) {
  size_t program = s_ctx.flash->program_size;
  return ((TEST_HEADER_BYTES + program - 1u) / program) * program;
}

static uint8_t *region_bytes(uint32_t region) {
  return &s_ctx.media[s_ctx.config.regions[region].address - TEST_FLASH_BASE];
}

/* This independent wire fixture tests interoperability, not private structures. */
static void seed_record(uint32_t region, uint32_t version, uint64_t generation,
                        uint32_t kind, const uint8_t *data, uint32_t length) {
  uint8_t *header = region_bytes(region);
  uint8_t *commit = header + s_ctx.config.regions[region].size_bytes - header_span();
  memset(header, 0xff, s_ctx.config.regions[region].size_bytes);
  fixture_u32(header, 0x52595345u);
  fixture_u32(header + 4u, version);
  fixture_u32(header + 8u, (uint32_t)generation);
  fixture_u32(header + 12u, (uint32_t)(generation >> 32u));
  fixture_u32(header + 16u, length);
  fixture_u32(header + 20u, kind);
  fixture_u32(header + 24u, fixture_crc(data, length));
  fixture_u32(header + 28u, fixture_crc(header, 28u));
  if (length != 0u) memcpy(header + header_span(), data, length);
  fixture_u32(commit, 0x54494d43u);
  fixture_u32(commit + 4u, version);
  memcpy(commit + 8u, header + 8u, 8u);
  memcpy(commit + 16u, header + 28u, 4u);
  memcpy(commit + 20u, header + 24u, 4u);
  fixture_u32(commit + 24u, length);
  fixture_u32(commit + 28u, fixture_crc(commit, 28u));
}

static void init_store(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK,
                        esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
}

static void reopen_store(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
  init_store();
}

static esy_dual_flash_store_info_t store_info(void) {
  esy_dual_flash_store_info_t info = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_get_info(&s_ctx.store, &info));
  return info;
}

static void expect_record(const uint8_t *bytes, size_t expected) {
  size_t length = SIZE_MAX;
  memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_read(
                                  &s_ctx.store, s_ctx.output,
                                  sizeof(s_ctx.output), &length));
  TEST_ASSERT_EQUAL_UINT(expected, length);
  if (expected != 0u) TEST_ASSERT_EQUAL_MEMORY(bytes, s_ctx.output, expected);
  TEST_ASSERT_EQUAL_HEX8(0xa5u, s_ctx.output[expected]);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_flash_reset();
  s_ctx.flash = fake_esy_hal_flash_get_state();
  s_ctx.flash->storage_base_addr = TEST_FLASH_BASE;
  s_ctx.flash->size_bytes = TEST_FLASH_BYTES;
  s_ctx.flash->sector_size = 256u;
  s_ctx.flash->program_size = TEST_PROGRAM_BYTES;
  s_ctx.flash->backing_storage = s_ctx.media;
  s_ctx.flash->backing_storage_size = sizeof(s_ctx.media);
  memset(s_ctx.media, 0xff, sizeof(s_ctx.media));
  for (size_t index = 0u; index < sizeof(s_ctx.data); ++index) {
    s_ctx.data[index] = (uint8_t)(index * 17u + 3u);
  }
  s_ctx.resources.work_buffer = s_ctx.work;
  s_ctx.resources.work_buffer_size = sizeof(s_ctx.work);
  s_ctx.config.static_resources = &s_ctx.resources;
  s_ctx.config.regions[0] = (esy_dual_flash_store_region_t){
      TEST_FLASH_BASE + TEST_REGION_OFFSET, TEST_REGION_BYTES};
  s_ctx.config.regions[1] = (esy_dual_flash_store_region_t){
      TEST_FLASH_BASE + TEST_REGION_OFFSET + TEST_REGION_BYTES, TEST_REGION_BYTES};
}

void tearDown(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
}

void test_dual_flash_store_empty_lifecycle_borrows_flash_without_owning_it(void) {
  init_store();
  esy_dual_flash_store_info_t info = store_info();
  TEST_ASSERT_FALSE(info.has_record);
  TEST_ASSERT_EQUAL_UINT32(0u, info.length);
  TEST_ASSERT_EQUAL_UINT64(0u, info.generation);
  TEST_ASSERT_EQUAL_UINT32(TEST_REGION_BYTES - 64u, info.capacity);
  size_t length = 42u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_dual_flash_store_read(&s_ctx.store, NULL, 0u, &length));
  TEST_ASSERT_EQUAL_UINT(42u, length);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->init_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->deinit_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->safe_execute_set_count);
  reopen_store();
}

void test_dual_flash_store_replaces_reopens_and_commits_clear_tombstone(void) {
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 37u));
  expect_record(s_ctx.data, 37u);
  TEST_ASSERT_EQUAL_UINT64(1u, store_info().generation);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data + 50u, 51u));
  reopen_store();
  expect_record(s_ctx.data + 50u, 51u);
  TEST_ASSERT_EQUAL_UINT64(2u, store_info().generation);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_clear(&s_ctx.store));
  reopen_store();
  TEST_ASSERT_FALSE(store_info().has_record);
  TEST_ASSERT_EQUAL_UINT64(3u, store_info().generation);
  uint32_t erases = s_ctx.flash->erase_count;
  uint32_t writes = s_ctx.flash->write_count;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_EQUAL_UINT32(erases, s_ctx.flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(writes, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 2u));
  TEST_ASSERT_EQUAL_UINT64(4u, store_info().generation);
  expect_record(s_ctx.data, 2u);
}

void test_dual_flash_store_zero_length_data_is_distinct_from_empty_and_clear(void) {
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, NULL, 0u));
  TEST_ASSERT_TRUE(store_info().has_record);
  size_t length = 99u;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_read(&s_ctx.store, NULL, 0u, &length));
  TEST_ASSERT_EQUAL_UINT(0u, length);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_FALSE(store_info().has_record);
  TEST_ASSERT_EQUAL_UINT64(2u, store_info().generation);
}

void test_dual_flash_store_capacity_and_small_output_preserve_original_record(void) {
  init_store();
  size_t capacity = store_info().capacity;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, capacity));
  expect_record(s_ctx.data, capacity);
  uint32_t erases = s_ctx.flash->erase_count;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, capacity + 1u));
  TEST_ASSERT_EQUAL_UINT32(erases, s_ctx.flash->erase_count);
  size_t length = 123u;
  memset(s_ctx.output, 0xabu, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM, esy_dual_flash_store_read(
      &s_ctx.store, s_ctx.output, capacity - 1u, &length));
  TEST_ASSERT_EQUAL_UINT(123u, length);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xabu, s_ctx.output, sizeof(s_ctx.output));
  expect_record(s_ctx.data, capacity);
}

void test_dual_flash_store_reads_independent_little_endian_wire_and_64bit_generations(void) {
  seed_record(0u, 1u, 0x12345678ffffffffull, TEST_DATA_KIND, s_ctx.data, 17u);
  seed_record(1u, 1u, 0x1234567900000000ull, TEST_DATA_KIND, s_ctx.data + 20u, 33u);
  init_store();
  TEST_ASSERT_EQUAL_UINT64(0x1234567900000000ull, store_info().generation);
  expect_record(s_ctx.data + 20u, 33u);
}

void test_dual_flash_store_rejects_ambiguous_valid_generations_and_exhaustion(void) {
  seed_record(0u, 1u, 8u, TEST_DATA_KIND, s_ctx.data, 1u);
  seed_record(1u, 1u, 8u, TEST_DATA_KIND, s_ctx.data, 1u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL,
      esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  seed_record(1u, 1u, 10u, TEST_DATA_KIND, s_ctx.data, 1u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL,
      esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  memset(region_bytes(0u), 0xff, TEST_REGION_BYTES);
  seed_record(1u, 1u, UINT64_MAX, TEST_DATA_KIND, s_ctx.data, 1u);
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  expect_record(s_ctx.data, 1u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
  seed_record(1u, 1u, UINT64_MAX, TEST_TOMBSTONE_KIND, NULL, 0u);
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 1u));
}

void test_dual_flash_store_ignores_torn_or_corrupt_records_but_rejects_unknown_format(void) {
  seed_record(0u, 1u, 1u, TEST_DATA_KIND, s_ctx.data, 17u);
  region_bytes(0u)[header_span() + 3u] ^= 1u;
  init_store();
  TEST_ASSERT_FALSE(store_info().has_record);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
  seed_record(0u, 1u, 1u, TEST_DATA_KIND, s_ctx.data, 17u);
  region_bytes(0u)[TEST_REGION_BYTES - header_span() + 28u] ^= 1u;
  init_store();
  TEST_ASSERT_FALSE(store_info().has_record);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
  seed_record(0u, 1u, 1u, TEST_DATA_KIND, s_ctx.data, 17u);
  region_bytes(0u)[28u] ^= 1u;
  init_store();
  TEST_ASSERT_FALSE(store_info().has_record);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
  seed_record(0u, 2u, 1u, TEST_DATA_KIND, s_ctx.data, 17u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED,
      esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
}

void test_dual_flash_store_init_errors_preserve_zero_handle_and_allow_retry(void) {
  esy_dual_flash_store_t zero = {0};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_init(&s_ctx.store, NULL));
  s_ctx.flash->geometry_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.store, sizeof(zero));
  s_ctx.flash->geometry_result = ESY_OK;
  s_ctx.flash->sector_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.flash->sector_result = ESY_OK;
  s_ctx.flash->read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.store, sizeof(zero));
  s_ctx.flash->read_result = ESY_OK;
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
}

void test_dual_flash_store_copied_handle_is_rejected_and_original_remains_usable(void) {
  init_store();
  s_ctx.copied = s_ctx.store;
  esy_dual_flash_store_info_t info;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_get_info(&s_ctx.copied, &info));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_write(&s_ctx.copied, NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_clear(&s_ctx.copied));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_deinit(&s_ctx.copied));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 1u));
  expect_record(s_ctx.data, 1u);
}

void test_dual_flash_store_rejects_memory_aliases_and_invalid_public_arguments(void) {
  init_store();
  size_t length = 77u;
  esy_dual_flash_store_info_t info;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_get_info(&s_ctx.store, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_get_info(
      &s_ctx.store, (esy_dual_flash_store_info_t *)&s_ctx.store));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_get_info(
      &s_ctx.store, (esy_dual_flash_store_info_t *)s_ctx.work));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_write(&s_ctx.store, NULL, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_write(&s_ctx.store, s_ctx.work, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_write(&s_ctx.store, &s_ctx.store, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read(&s_ctx.store, NULL, 1u, &length));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read(&s_ctx.store, s_ctx.output, 1u, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read(&s_ctx.store, s_ctx.work, 1u, &length));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read(&s_ctx.store, &s_ctx.store, 1u, &length));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read(
      &s_ctx.store, s_ctx.output, sizeof(s_ctx.output), (size_t *)s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_get_info(NULL, &info));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_deinit(NULL));
}

void test_dual_flash_store_rejects_bad_regions_workspace_and_address_overflow(void) {
  esy_dual_flash_store_config_t original = s_ctx.config;
  s_ctx.config.regions[1] = s_ctx.config.regions[0];
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.config = original;
  s_ctx.config.regions[0].address++;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.config = original;
  s_ctx.config.regions[0].size_bytes--;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.config = original;
  s_ctx.config.regions[1].address = UINT32_MAX - 15u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.config = original;
  s_ctx.resources.work_buffer_size = TEST_PROGRAM_BYTES - 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.resources.work_buffer_size = sizeof(s_ctx.work);
  s_ctx.resources.work_buffer = (uint8_t *)&s_ctx.store;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.resources.work_buffer = s_ctx.work;
  s_ctx.config.static_resources = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.config = original;
  init_store();
}

void test_dual_flash_store_supports_mixed_sectors_and_larger_program_units(void) {
  s_ctx.flash->sector_size_after_address = TEST_FLASH_BASE + 5120u;
  s_ctx.flash->sector_size_after = 1024u;
  s_ctx.flash->program_size = 512u;
  init_store();
  TEST_ASSERT_EQUAL_UINT32(TEST_REGION_BYTES - 1024u, store_info().capacity);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 513u));
  reopen_store();
  expect_record(s_ctx.data, 513u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_FALSE(store_info().has_record);
}

void test_dual_flash_store_hal_read_errors_fail_closed_without_mutation_or_info_changes(void) {
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 11u));
  esy_dual_flash_store_info_t info;
  memset(&info, 0x7a, sizeof(info));
  esy_dual_flash_store_info_t original = info;
  uint32_t erases = s_ctx.flash->erase_count;
  uint32_t writes = s_ctx.flash->write_count;
  s_ctx.flash->read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_dual_flash_store_get_info(&s_ctx.store, &info));
  TEST_ASSERT_EQUAL_MEMORY(&original, &info, sizeof(info));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 10u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_EQUAL_UINT32(erases, s_ctx.flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(writes, s_ctx.flash->write_count);
  s_ctx.flash->read_result = ESY_OK;
  expect_record(s_ctx.data, 11u);
}

static void clear_injections_and_counts(void) {
  s_ctx.flash->fail_read_call = 0u;
  s_ctx.flash->fail_write_call = 0u;
  s_ctx.flash->fail_erase_call = 0u;
  s_ctx.flash->read_count = 0u;
  s_ctx.flash->write_count = 0u;
  s_ctx.flash->erase_count = 0u;
}

static void transaction_fixture(uint32_t scenario) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
  memset(s_ctx.media, 0xff, sizeof(s_ctx.media));
  seed_record(0u, 1u, 1u, TEST_DATA_KIND, s_ctx.data, 19u);
  seed_record(1u, 1u, 2u, TEST_DATA_KIND, s_ctx.data + 64u, 37u);
  if (scenario == 2u) seed_record(0u, 1u, 3u, TEST_TOMBSTONE_KIND, NULL, 0u);
  if (scenario == 3u) memset(s_ctx.media, 0xff, sizeof(s_ctx.media));
  memcpy(s_ctx.saved, s_ctx.media, sizeof(s_ctx.media));
  init_store();
  clear_injections_and_counts();
}

static void restore_transaction(void) {
  clear_injections_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
  memcpy(s_ctx.media, s_ctx.saved, sizeof(s_ctx.media));
  init_store();
  clear_injections_and_counts();
}

static esy_err_t perform_transaction(uint32_t scenario) {
  if (scenario == 1u) return esy_dual_flash_store_clear(&s_ctx.store);
  return esy_dual_flash_store_write(&s_ctx.store, s_ctx.data + 128u, 41u);
}

static void expect_atomic_recovery(uint32_t scenario) {
  clear_injections_and_counts();
  reopen_store();
  esy_dual_flash_store_info_t info = store_info();
  uint64_t previous = scenario == 2u ? 3u : scenario == 3u ? 0u : 2u;
  TEST_ASSERT_TRUE(info.generation == previous || info.generation == previous + 1u);
  if (info.generation == previous) {
    if (scenario == 2u || scenario == 3u) {
      TEST_ASSERT_FALSE(info.has_record);
    } else {
      TEST_ASSERT_TRUE(info.has_record);
      expect_record(s_ctx.data + 64u, 37u);
    }
  } else if (scenario == 1u) {
    TEST_ASSERT_FALSE(info.has_record);
  } else {
    TEST_ASSERT_TRUE(info.has_record);
    expect_record(s_ctx.data + 128u, 41u);
  }
  if (scenario != 3u) {
    uint32_t previous_region = scenario == 2u ? 0u : 1u;
    size_t previous_offset = s_ctx.config.regions[previous_region].address - TEST_FLASH_BASE;
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved + previous_offset,
        s_ctx.media + previous_offset, TEST_REGION_BYTES);
  }
  TEST_ASSERT_EACH_EQUAL_HEX8(0xffu, s_ctx.media, TEST_REGION_OFFSET);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xffu,
      s_ctx.media + TEST_REGION_OFFSET + 2u * TEST_REGION_BYTES,
      TEST_FLASH_BYTES - TEST_REGION_OFFSET - 2u * TEST_REGION_BYTES);
}

void test_dual_flash_store_power_cut_at_each_program_prefix_preserves_atomic_replace_and_clear(void) {
  for (uint32_t scenario = 0u; scenario < 4u; ++scenario) {
    transaction_fixture(scenario);
    TEST_ASSERT_EQUAL_INT(ESY_OK, perform_transaction(scenario));
    uint32_t write_calls = s_ctx.flash->write_count;
    for (uint32_t call = 1u; call <= write_calls; ++call) {
      for (size_t prefix = 0u; prefix <= TEST_PROGRAM_BYTES; ++prefix) {
        restore_transaction();
        s_ctx.flash->fail_write_call = call;
        s_ctx.flash->write_fail_bytes = prefix;
        TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, perform_transaction(scenario));
        expect_atomic_recovery(scenario);
      }
    }
    /* A fully programmed final commit may become visible despite the error. */
    restore_transaction();
    s_ctx.flash->fail_write_call = write_calls;
    s_ctx.flash->write_fail_bytes = SIZE_MAX;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, perform_transaction(scenario));
    expect_atomic_recovery(scenario);
    TEST_ASSERT_EQUAL_UINT64(scenario == 2u ? 4u : scenario == 3u ? 1u : 3u,
        store_info().generation);
  }
}

void test_dual_flash_store_power_cut_during_erase_never_erases_current_record(void) {
  const size_t prefixes[] = {0u, 1u, 31u, 255u, 256u, 2048u, TEST_REGION_BYTES, SIZE_MAX};
  for (uint32_t scenario = 0u; scenario < 4u; ++scenario) {
    transaction_fixture(scenario);
    for (size_t index = 0u; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
      restore_transaction();
      s_ctx.flash->fail_erase_call = 1u;
      s_ctx.flash->erase_fail_bytes = prefixes[index];
      TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, perform_transaction(scenario));
      expect_atomic_recovery(scenario);
      TEST_ASSERT_EQUAL_UINT64(scenario == 2u ? 3u : scenario == 3u ? 0u : 2u,
          store_info().generation);
    }
  }
}

void test_dual_flash_store_every_read_error_fails_closed_even_after_commit_effect(void) {
  for (uint32_t scenario = 0u; scenario < 4u; ++scenario) {
    transaction_fixture(scenario);
    TEST_ASSERT_EQUAL_INT(ESY_OK, perform_transaction(scenario));
    uint32_t read_calls = s_ctx.flash->read_count;
    for (uint32_t call = 1u; call <= read_calls; ++call) {
      restore_transaction();
      s_ctx.flash->fail_read_call = call;
      s_ctx.flash->read_fail_bytes = SIZE_MAX;
      TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, perform_transaction(scenario));
      expect_atomic_recovery(scenario);
    }
    restore_transaction();
    s_ctx.flash->fail_read_call = read_calls;
    s_ctx.flash->read_fail_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, perform_transaction(scenario));
    expect_atomic_recovery(scenario);
    TEST_ASSERT_EQUAL_UINT64(scenario == 2u ? 4u : scenario == 3u ? 1u : 3u,
        store_info().generation);
  }
}

void test_dual_flash_store_read_failure_in_either_region_blocks_mutation_and_preserves_outputs(void) {
  transaction_fixture(0u);
  esy_dual_flash_store_info_t info;
  memset(&info, 0x3cu, sizeof(info));
  esy_dual_flash_store_info_t original = info;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_get_info(&s_ctx.store, &info));
  uint32_t reads = s_ctx.flash->read_count;
  for (uint32_t call = 1u; call <= reads; ++call) {
    restore_transaction();
    info = original;
    s_ctx.flash->fail_read_call = call;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_dual_flash_store_get_info(&s_ctx.store, &info));
    TEST_ASSERT_EQUAL_MEMORY(&original, &info, sizeof(info));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  }
  clear_injections_and_counts();
}

void test_dual_flash_store_partial_final_read_preserves_length_and_reports_buffer_effect(void) {
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 37u));
  clear_injections_and_counts();
  size_t length;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_read(
      &s_ctx.store, s_ctx.output, sizeof(s_ctx.output), &length));
  uint32_t final_read_call = s_ctx.flash->read_count;
  clear_injections_and_counts();
  memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
  length = 99u;
  s_ctx.flash->fail_read_call = final_read_call;
  s_ctx.flash->read_fail_bytes = 7u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_dual_flash_store_read(
      &s_ctx.store, s_ctx.output, sizeof(s_ctx.output), &length));
  TEST_ASSERT_EQUAL_UINT(99u, length);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.data, s_ctx.output, 7u);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xa5u, s_ctx.output + 7u, sizeof(s_ctx.output) - 7u);
  clear_injections_and_counts();
}

void test_dual_flash_store_never_reprograms_a_unit_and_uses_caller_page_staging(void) {
  s_ctx.flash->programmed_units = s_ctx.marks;
  s_ctx.flash->programmed_units_size = sizeof(s_ctx.marks);
  init_store();
  for (uint32_t iteration = 0u; iteration < 8u; ++iteration) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(
        &s_ctx.store, s_ctx.data + iteration, 101u + iteration));
    expect_record(s_ctx.data + iteration, 101u + iteration);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_clear(&s_ctx.store));
    reopen_store();
    TEST_ASSERT_FALSE(store_info().has_record);
  }
}

void test_dual_flash_store_program_geometry_smaller_and_larger_than_wire_header(void) {
  const uint32_t sizes[] = {1u, 3u, 8u, 32u, 64u, 256u, 1024u};
  for (size_t index = 0u; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
    memset(s_ctx.media, 0xff, sizeof(s_ctx.media));
    s_ctx.flash->program_size = sizes[index];
    if (sizes[index] == 3u) {
      /* A deliberately non-power-of-two provider remains a valid HAL geometry. */
      s_ctx.flash->storage_base_addr = 0u;
      s_ctx.config.regions[0] = (esy_dual_flash_store_region_t){0u, 3072u};
      s_ctx.config.regions[1] = (esy_dual_flash_store_region_t){3072u, 3072u};
    } else {
      s_ctx.flash->storage_base_addr = TEST_FLASH_BASE;
      s_ctx.config.regions[0] = (esy_dual_flash_store_region_t){
          TEST_FLASH_BASE + TEST_REGION_OFFSET, TEST_REGION_BYTES};
      s_ctx.config.regions[1] = (esy_dual_flash_store_region_t){
          TEST_FLASH_BASE + TEST_REGION_OFFSET + TEST_REGION_BYTES, TEST_REGION_BYTES};
    }
    init_store();
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 103u));
    reopen_store();
    expect_record(s_ctx.data, 103u);
  }
}

void test_dual_flash_store_torn_metadata_never_wins_over_a_valid_predecessor(void) {
  for (uint32_t variant = 0u; variant < 8u; ++variant) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
    seed_record(0u, 1u, 1u, TEST_DATA_KIND, s_ctx.data, 19u);
    seed_record(1u, 1u, 2u, TEST_DATA_KIND, s_ctx.data + 64u, 37u);
    uint8_t *header = region_bytes(1u);
    uint8_t *commit = header + TEST_REGION_BYTES - header_span();
    switch (variant) {
      case 0u: fixture_u32(header + 8u, 0u); break;
      case 1u: fixture_u32(header + 16u, TEST_REGION_BYTES); break;
      case 2u: fixture_u32(header + 20u, 3u); break;
      case 3u: fixture_u32(header + 20u, TEST_TOMBSTONE_KIND); break;
      case 4u: fixture_u32(commit + 8u, 3u); break;
      case 5u: commit[16u] ^= 1u; break;
      case 6u: commit[20u] ^= 1u; break;
      default: fixture_u32(commit + 24u, 38u); break;
    }
    if (variant < 4u) {
      fixture_u32(header + 28u, fixture_crc(header, 28u));
      memcpy(commit + 16u, header + 28u, 4u);
    }
    fixture_u32(commit + 28u, fixture_crc(commit, 28u));
    init_store();
    TEST_ASSERT_EQUAL_UINT64(1u, store_info().generation);
    expect_record(s_ctx.data, 19u);
  }
}

void test_dual_flash_store_unknown_committed_version_blocks_read_write_and_clear(void) {
  transaction_fixture(0u);
  seed_record(1u, 2u, 2u, TEST_DATA_KIND, s_ctx.data + 64u, 37u);
  esy_dual_flash_store_info_t info;
  memset(&info, 0x5a, sizeof(info));
  esy_dual_flash_store_info_t original = info;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED,
      esy_dual_flash_store_get_info(&s_ctx.store, &info));
  TEST_ASSERT_EQUAL_MEMORY(&original, &info, sizeof(info));
  size_t length = 77u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, esy_dual_flash_store_read(
      &s_ctx.store, s_ctx.output, sizeof(s_ctx.output), &length));
  TEST_ASSERT_EQUAL_UINT(77u, length);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED,
      esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 7u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  /* A recognizable future header must also fail closed with a current commit. */
  uint8_t *commit = region_bytes(1u) + TEST_REGION_BYTES - header_span();
  fixture_u32(commit + 4u, 1u);
  fixture_u32(commit + 28u, fixture_crc(commit, 28u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED,
      esy_dual_flash_store_get_info(&s_ctx.store, &info));
}

void test_dual_flash_store_uses_smaller_region_capacity_and_accepts_physical_end_at_4gib(void) {
  s_ctx.config.regions[1].size_bytes = TEST_REGION_BYTES / 2u;
  init_store();
  TEST_ASSERT_EQUAL_UINT32(TEST_REGION_BYTES / 2u - 64u, store_info().capacity);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.store));
  s_ctx.flash->storage_base_addr = UINT32_MAX - TEST_FLASH_BYTES + 1u;
  s_ctx.config.regions[0] = (esy_dual_flash_store_region_t){
      s_ctx.flash->storage_base_addr + TEST_FLASH_BYTES - 2u * TEST_REGION_BYTES,
      TEST_REGION_BYTES};
  s_ctx.config.regions[1] = (esy_dual_flash_store_region_t){
      s_ctx.flash->storage_base_addr + TEST_FLASH_BYTES - TEST_REGION_BYTES,
      TEST_REGION_BYTES};
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 37u));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data + 64u, 39u));
  reopen_store();
  expect_record(s_ctx.data + 64u, 39u);
}

void test_dual_flash_store_uninitialized_calls_and_tiny_regions_fail_without_flash_changes(void) {
  esy_dual_flash_store_info_t info;
  size_t length;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_get_info(&s_ctx.store, &info));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_dual_flash_store_read(&s_ctx.store, s_ctx.output, sizeof(s_ctx.output), &length));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_clear(&s_ctx.store));
  s_ctx.flash->sector_size = 32u;
  s_ctx.config.regions[0].size_bytes = 64u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM, esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  s_ctx.config.regions[0].size_bytes = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_dual_flash_store_init(&s_ctx.store, &s_ctx.config));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
}

void test_dual_flash_store_range_read_validates_the_whole_record_and_copies_only_the_requested_span(void) {
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, s_ctx.data, 37u));
  memset(s_ctx.output, 0xA5, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_read_range(&s_ctx.store, 7u, s_ctx.output, 13u));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.data + 7u, s_ctx.output, 13u);
  TEST_ASSERT_EQUAL_HEX8(0xA5u, s_ctx.output[13]);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_read_range(&s_ctx.store, 37u, NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read_range(&s_ctx.store, 38u, NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read_range(&s_ctx.store, UINT32_MAX, s_ctx.output, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read_range(&s_ctx.store, 0u, s_ctx.work, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_dual_flash_store_read_range(&s_ctx.store, 0u, NULL, 1u));
}

void test_dual_flash_store_range_read_rejects_absence_and_rescans_after_tombstone_or_io_failure(void) {
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_read_range(&s_ctx.store, 0u, NULL, 0u));
  init_store();
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_read_range(&s_ctx.store, 0u, NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_write(&s_ctx.store, NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_read_range(&s_ctx.store, 0u, NULL, 0u));
  s_ctx.flash->read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_dual_flash_store_read_range(&s_ctx.store, 0u, NULL, 0u));
  s_ctx.flash->read_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_dual_flash_store_clear(&s_ctx.store));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_dual_flash_store_read_range(&s_ctx.store, 0u, NULL, 0u));
}
