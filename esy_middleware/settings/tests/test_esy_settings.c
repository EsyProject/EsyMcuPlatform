/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <limits.h>
#include <string.h>

#include "esy_middleware/settings/esy_settings.h"
#include "esy_middleware/file/esy_file_dual_flash_store.h"
#include "esy_middleware/file/esy_file_ram.h"
#include "fake_esy_hal_flash.h"
#include "settings_wire_fixture.h"

#define TEST_FLASH_BASE 0x10000u
#define TEST_FLASH_BYTES 16384u
#define TEST_REGION_BYTES 4096u
#define TEST_REGION_OFFSET 1024u
#define TEST_PROGRAM_BYTES 16u
#define TEST_BUFFER_BYTES 512u
#define TEST_MAX_KEY_BYTES 31u
#define TEST_SNAPSHOT_HEADER_BYTES 16u

typedef struct {
  esy_settings_t settings;
  esy_settings_t copied;
  esy_settings_config_t config;
  esy_settings_static_resources_t resources;
  esy_file_t file;
  esy_file_dual_flash_store_t file_adapter;
  esy_file_ram_t ram_adapter;
  esy_dual_flash_store_t dual_flash_store;
  esy_dual_flash_store_config_t record_config;
  esy_dual_flash_store_static_resources_t record_resources;
  fake_esy_hal_flash_state_t *flash;
  uint8_t media[TEST_FLASH_BYTES];
  uint8_t saved_media[TEST_FLASH_BYTES];
  uint8_t work[TEST_BUFFER_BYTES];
  uint8_t scratch[TEST_BUFFER_BYTES];
  uint8_t record_work[128];
  uint8_t wire[TEST_REGION_BYTES];
  uint8_t output[TEST_REGION_BYTES];
  uint8_t saved_work[TEST_BUFFER_BYTES];
  uint8_t ram_data[TEST_REGION_BYTES];
} test_ctx_t;

static test_ctx_t s_ctx;

static esy_err_t attach_record_file(void) {
  esy_err_t result = esy_dual_flash_store_init(&s_ctx.dual_flash_store, &s_ctx.record_config);
  if (result != ESY_OK) return result;
  const esy_file_dual_flash_store_config_t config = {.store = &s_ctx.dual_flash_store};
  result = esy_file_dual_flash_store_init(&s_ctx.file_adapter, &config);
  if (result != ESY_OK) return result;
  esy_file_ops_t ops;
  result = esy_file_dual_flash_store_get_ops(&s_ctx.file_adapter, &ops);
  return result == ESY_OK ? esy_file_init(&s_ctx.file, &ops) : result;
}

static esy_err_t detach_record_file(void) {
  esy_err_t result = esy_file_deinit(&s_ctx.file);
  if (result != ESY_OK) return result;
  result = esy_file_ram_deinit(&s_ctx.ram_adapter);
  if (result != ESY_OK) return result;
  result = esy_file_dual_flash_store_deinit(&s_ctx.file_adapter);
  return result == ESY_OK ? esy_dual_flash_store_deinit(&s_ctx.dual_flash_store) : result;
}

static void init_settings(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
}

static esy_settings_info_t settings_info(void) {
  esy_settings_info_t info = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_get_info(&s_ctx.settings, &info));
  return info;
}

static void expect_info_equal(esy_settings_info_t expected) {
  esy_settings_info_t actual = settings_info();
  TEST_ASSERT_EQUAL_INT(expected.state, actual.state);
  TEST_ASSERT_EQUAL_INT(expected.has_snapshot, actual.has_snapshot);
  TEST_ASSERT_EQUAL_UINT32(expected.entry_count, actual.entry_count);
  TEST_ASSERT_EQUAL_UINT(expected.used_bytes, actual.used_bytes);
  TEST_ASSERT_EQUAL_UINT(expected.capacity, actual.capacity);
}

static void expect_value(const char *key, const void *value, size_t expected) {
  size_t length = SIZE_MAX;
  bool found = false;
  memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_get(
      &s_ctx.settings, key, s_ctx.output, sizeof(s_ctx.output), &length, &found));
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL_UINT(expected, length);
  if (expected != 0u) TEST_ASSERT_EQUAL_MEMORY(value, s_ctx.output, expected);
  TEST_ASSERT_EQUAL_HEX8(0xa5u, s_ctx.output[expected]);
}

static void expect_byte(const char *key, uint8_t expected) {
  expect_value(key, &expected, sizeof(expected));
}

static void expect_absent(const char *key) {
  size_t length = SIZE_MAX;
  bool found = true;
  memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_get(
      &s_ctx.settings, key, s_ctx.output, sizeof(s_ctx.output), &length, &found));
  TEST_ASSERT_FALSE(found);
  TEST_ASSERT_EQUAL_UINT(0u, length);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xa5u, s_ctx.output, sizeof(s_ctx.output));
}

static void set_byte(const char *key, uint8_t value) {
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, key, &value, sizeof(value)));
}

static void reopen_settings(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, detach_record_file());
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      attach_record_file());
  init_settings();
}

static void reset_faults_and_counts(void) {
  s_ctx.flash->fail_read_call = 0u;
  s_ctx.flash->fail_write_call = 0u;
  s_ctx.flash->fail_erase_call = 0u;
  s_ctx.flash->read_result = ESY_OK;
  s_ctx.flash->write_result = ESY_OK;
  s_ctx.flash->erase_result = ESY_OK;
  s_ctx.flash->read_count = 0u;
  s_ctx.flash->write_count = 0u;
  s_ctx.flash->erase_count = 0u;
}

static void persist_wire(size_t length) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_file_replace(&s_ctx.file, s_ctx.wire, length));
}

static size_t single_wire(uint32_t version, const void *key, uint32_t key_size,
                          const void *value, uint32_t value_size) {
  size_t length = TEST_SNAPSHOT_HEADER_BYTES + settings_fixture_entry(
      s_ctx.wire + TEST_SNAPSHOT_HEADER_BYTES, key, key_size, value, value_size);
  settings_fixture_header(s_ctx.wire, version, 1u, (uint32_t)length);
  return length;
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
  memset(s_ctx.work, 0xa5, sizeof(s_ctx.work));
  memset(s_ctx.scratch, 0x5a, sizeof(s_ctx.scratch));
  s_ctx.record_resources.work_buffer = s_ctx.record_work;
  s_ctx.record_resources.work_buffer_size = sizeof(s_ctx.record_work);
  s_ctx.record_config.static_resources = &s_ctx.record_resources;
  s_ctx.record_config.regions[0] = (esy_dual_flash_store_region_t){
      TEST_FLASH_BASE + TEST_REGION_OFFSET, TEST_REGION_BYTES};
  s_ctx.record_config.regions[1] = (esy_dual_flash_store_region_t){
      TEST_FLASH_BASE + TEST_REGION_OFFSET + TEST_REGION_BYTES, TEST_REGION_BYTES};
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      attach_record_file());
  s_ctx.resources.working_buffer = s_ctx.work;
  s_ctx.resources.scratch_buffer = s_ctx.scratch;
  s_ctx.resources.buffer_size = sizeof(s_ctx.work);
  s_ctx.config.file = &s_ctx.file;
  s_ctx.config.max_key_length = TEST_MAX_KEY_BYTES;
  s_ctx.config.static_resources = &s_ctx.resources;
}

void tearDown(void) {
  reset_faults_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, detach_record_file());
}

void test_settings_empty_media_loads_without_formatting_or_owning_flash(void) {
  init_settings();
  esy_settings_info_t info = settings_info();
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_CLEAN, info.state);
  TEST_ASSERT_FALSE(info.has_snapshot);
  TEST_ASSERT_EQUAL_UINT32(0u, info.entry_count);
  TEST_ASSERT_EQUAL_UINT(TEST_SNAPSHOT_HEADER_BYTES, info.used_bytes);
  TEST_ASSERT_EQUAL_UINT(TEST_BUFFER_BYTES, info.capacity);
  expect_absent("network/host");
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_remove(&s_ctx.settings, "absent"));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_reload(&s_ctx.settings));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->init_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->deinit_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->safe_execute_set_count);
}

void test_settings_multiple_keys_replace_delete_and_persist_as_one_snapshot(void) {
  init_settings();
  const uint8_t host[] = {192u, 168u, 1u, 23u};
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "network/host", host, sizeof(host)));
  set_byte("network/port", 42u);
  set_byte("sensor/offset", 7u);
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_DIRTY, settings_info().state);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(3u, settings_info().entry_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_CLEAN, settings_info().state);
  TEST_ASSERT_TRUE(settings_info().has_snapshot);
  reopen_settings();
  expect_value("network/host", host, sizeof(host));
  expect_byte("network/port", 42u);
  expect_byte("sensor/offset", 7u);
  set_byte("network/port", 53u);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_remove(&s_ctx.settings, "sensor/offset"));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  reopen_settings();
  expect_value("network/host", host, sizeof(host));
  expect_byte("network/port", 53u);
  expect_absent("sensor/offset");
  TEST_ASSERT_EQUAL_UINT32(2u, settings_info().entry_count);
}

void test_settings_copies_key_and_value_and_distinguishes_case_and_empty_blob(void) {
  init_settings();
  char key[] = "network/name";
  uint8_t value[] = {0u, 0xffu, 0u, 1u};
  const uint8_t expected[] = {0u, 0xffu, 0u, 1u};
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, key, value, sizeof(value)));
  memset(key, 'x', sizeof(key) - 1u);
  memset(value, 0x55, sizeof(value));
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "empty", NULL, 0u));
  set_byte("Network/name", 9u);
  expect_value("network/name", expected, sizeof(expected));
  expect_byte("Network/name", 9u);
  expect_value("empty", NULL, 0u);
  expect_absent("EMPTY");
  size_t length = SIZE_MAX;
  bool found = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_get(&s_ctx.settings, "empty", NULL, 0u, &length, &found));
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL_UINT(0u, length);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  reopen_settings();
  expect_value("empty", NULL, 0u);
  expect_value("network/name", expected, sizeof(expected));
}

void test_settings_size_query_and_short_output_preserve_destination(void) {
  init_settings();
  const uint8_t value[] = {1u, 2u, 3u};
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "value", value, sizeof(value)));
  size_t length = SIZE_MAX;
  bool found = false;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_settings_get(&s_ctx.settings, "value", NULL, 0u, &length, &found));
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL_UINT(sizeof(value), length);
  memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM, esy_settings_get(
      &s_ctx.settings, "value", s_ctx.output, sizeof(value) - 1u, &length, &found));
  TEST_ASSERT_EACH_EQUAL_HEX8(0xa5u, s_ctx.output, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_get(
      &s_ctx.settings, "value", s_ctx.output, sizeof(value), &length, &found));
  TEST_ASSERT_EQUAL_MEMORY(value, s_ctx.output, sizeof(value));
}

void test_settings_unchanged_set_absent_remove_and_clean_commit_do_not_write(void) {
  init_settings();
  set_byte("value", 3u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  reset_faults_and_counts();
  set_byte("value", 3u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_remove(&s_ctx.settings, "absent"));
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_CLEAN, settings_info().state);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->read_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
}

void test_settings_removing_last_key_commits_a_valid_empty_snapshot(void) {
  init_settings();
  set_byte("value", 3u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_remove(&s_ctx.settings, "value"));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  reopen_settings();
  TEST_ASSERT_TRUE(settings_info().has_snapshot);
  TEST_ASSERT_EQUAL_UINT32(0u, settings_info().entry_count);
  TEST_ASSERT_EQUAL_UINT(TEST_SNAPSHOT_HEADER_BYTES, settings_info().used_bytes);
  expect_absent("value");
}

void test_settings_reload_and_deinit_intentionally_discard_uncommitted_changes(void) {
  init_settings();
  set_byte("value", 3u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  set_byte("value", 4u);
  set_byte("unsaved", 5u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_reload(&s_ctx.settings));
  expect_byte("value", 3u);
  expect_absent("unsaved");
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_CLEAN, settings_info().state);
  set_byte("value", 6u);
  reset_faults_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->read_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  init_settings();
  expect_byte("value", 3u);
}

void test_settings_exact_capacity_and_failed_growth_preserve_all_working_bytes(void) {
  s_ctx.resources.buffer_size = 32u;
  init_settings();
  const uint8_t value[] = {0x31u, 0x31u, 0x31u, 0x31u, 0x31u, 0x31u};
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "key", value, 5u));
  TEST_ASSERT_EQUAL_UINT(32u, settings_info().used_bytes);
  memcpy(s_ctx.saved_work, s_ctx.work, sizeof(s_ctx.work));
  esy_settings_info_t info = settings_info();
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_settings_set(&s_ctx.settings, "key", value, 6u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_settings_set(&s_ctx.settings, "other", NULL, 0u));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_work, s_ctx.work, sizeof(s_ctx.work));
  expect_info_equal(info);
  expect_value("key", value, 5u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  reopen_settings();
  TEST_ASSERT_EQUAL_UINT(32u, settings_info().used_bytes);
}

void test_settings_replacement_shrink_and_remove_compact_the_working_snapshot(void) {
  s_ctx.resources.buffer_size = 53u;
  init_settings();
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "first", "abcdefghijk", 11u));
  set_byte("last", 7u);
  TEST_ASSERT_EQUAL_UINT(53u, settings_info().used_bytes);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "first", NULL, 0u));
  TEST_ASSERT_EQUAL_UINT(42u, settings_info().used_bytes);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "new", NULL, 0u));
  TEST_ASSERT_EQUAL_UINT(53u, settings_info().used_bytes);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_remove(&s_ctx.settings, "last"));
  TEST_ASSERT_EQUAL_UINT(40u, settings_info().used_bytes);
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "first", "abcdefghijklm", 13u));
  TEST_ASSERT_EQUAL_UINT(53u, settings_info().used_bytes);
  expect_value("first", "abcdefghijklm", 13u);
  expect_absent("last");
  expect_value("new", NULL, 0u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  reopen_settings();
  expect_value("first", "abcdefghijklm", 13u);
  expect_absent("last");
  expect_value("new", NULL, 0u);
}

void test_settings_limits_keys_and_accepts_the_maximum_without_truncation(void) {
  init_settings();
  char maximum[TEST_MAX_KEY_BYTES + 1u];
  char excessive[TEST_MAX_KEY_BYTES + 2u];
  memset(maximum, 'k', sizeof(maximum) - 1u);
  maximum[sizeof(maximum) - 1u] = '\0';
  memset(excessive, 'k', sizeof(excessive) - 1u);
  excessive[sizeof(excessive) - 1u] = '\0';
  set_byte(maximum, 4u);
  expect_byte(maximum, 4u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_set(&s_ctx.settings, excessive, NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_set(&s_ctx.settings, "", NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_remove(&s_ctx.settings, excessive));
  TEST_ASSERT_EQUAL_UINT32(1u, settings_info().entry_count);
}

void test_settings_copied_handles_are_rejected_without_harming_the_original(void) {
  init_settings();
  s_ctx.copied = s_ctx.settings;
  esy_settings_info_t info;
  size_t length = 9u;
  bool found = false;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_settings_get_info(&s_ctx.copied, &info));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_settings_get(&s_ctx.copied, "key", NULL, 0u, &length, &found));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_settings_set(&s_ctx.copied, "key", NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_settings_remove(&s_ctx.copied, "key"));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_settings_commit(&s_ctx.copied));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_settings_reload(&s_ctx.copied));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_settings_deinit(&s_ctx.copied));
  set_byte("key", 4u);
  expect_byte("key", 4u);
}

void test_settings_lifecycle_repeated_init_failure_retry_and_zero_deinit(void) {
  esy_settings_t zero = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_settings_reload(&s_ctx.settings));
  s_ctx.flash->read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.settings, sizeof(zero));
  s_ctx.flash->read_result = ESY_OK;
  init_settings();
  set_byte("unsaved", 1u);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  expect_byte("unsaved", 1u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.settings, sizeof(zero));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  init_settings();
  expect_absent("unsaved");
}

void test_settings_invalid_configuration_fails_without_publishing_a_handle(void) {
  esy_settings_t zero = {0};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_init(&s_ctx.settings, NULL));
  esy_settings_config_t original = s_ctx.config;
  s_ctx.config.static_resources = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.config = original;
  s_ctx.config.file = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.config = original;
  s_ctx.config.max_key_length = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.config = original;
  s_ctx.config.max_key_length = SIZE_MAX;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.config = original;
  s_ctx.resources.buffer_size = TEST_SNAPSHOT_HEADER_BYTES - 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.settings, sizeof(zero));
  s_ctx.resources.buffer_size = sizeof(s_ctx.work);
  TEST_ASSERT_EQUAL_INT(ESY_OK, detach_record_file());
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      attach_record_file());
  init_settings();
}

void test_settings_rejects_overlapping_resources_and_buffer_handle_aliases(void) {
  esy_settings_static_resources_t original = s_ctx.resources;
  s_ctx.resources.scratch_buffer = s_ctx.work + 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.resources = original;
  s_ctx.resources.working_buffer = (uint8_t *)&s_ctx.settings;
  s_ctx.resources.buffer_size = sizeof(s_ctx.settings);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.resources = original;
  s_ctx.resources.scratch_buffer = (uint8_t *)&s_ctx.file;
  s_ctx.resources.buffer_size = sizeof(s_ctx.file);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.resources = original;
  s_ctx.resources.working_buffer = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.resources = original;
  s_ctx.resources.scratch_buffer = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  s_ctx.resources = original;
  init_settings();
}

void test_settings_invalid_public_arguments_preserve_outputs_and_working_state(void) {
  init_settings();
  set_byte("value", 4u);
  size_t length = 71u;
  bool found = false;
  esy_settings_info_t info = settings_info();
  memcpy(s_ctx.saved_work, s_ctx.work, sizeof(s_ctx.work));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_get(NULL, "value", NULL, 0u, &length, &found));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_get(&s_ctx.settings, NULL, NULL, 0u, &length, &found));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_get(&s_ctx.settings, "", NULL, 0u, &length, &found));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_get(&s_ctx.settings, "value", NULL, 1u, &length, &found));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_get(&s_ctx.settings, "value", NULL, 0u, NULL, &found));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_get(&s_ctx.settings, "value", NULL, 0u, &length, NULL));
  TEST_ASSERT_EQUAL_UINT(71u, length);
  TEST_ASSERT_FALSE(found);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_set(NULL, "value", NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_set(&s_ctx.settings, NULL, NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_set(&s_ctx.settings, "value", NULL, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_set(&s_ctx.settings, "value", s_ctx.output, SIZE_MAX));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_settings_set(&s_ctx.settings, "value", s_ctx.output, TEST_BUFFER_BYTES + 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_remove(NULL, "value"));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_remove(&s_ctx.settings, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_commit(NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_reload(NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_deinit(NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_settings_get_info(&s_ctx.settings, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_get_info(NULL, &info));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_work, s_ctx.work, sizeof(s_ctx.work));
  expect_info_equal(info);
}

void test_settings_rejects_input_output_and_query_aliases(void) {
  init_settings();
  set_byte("value", 4u);
  size_t length = 71u;
  bool found = false;
  void *owned[] = {&s_ctx.settings, &s_ctx.file, s_ctx.work, s_ctx.scratch};
  for (size_t index = 0u; index < sizeof(owned) / sizeof(owned[0]); ++index) {
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_settings_set(&s_ctx.settings, "value", owned[index], 1u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_settings_get(&s_ctx.settings, "value", owned[index], 1u, &length, &found));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_get_info(
        &s_ctx.settings, (esy_settings_info_t *)owned[index]));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_get(
        &s_ctx.settings, "value", NULL, 0u, (size_t *)owned[index], &found));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_get(
        &s_ctx.settings, "value", NULL, 0u, &length, (bool *)owned[index]));
  }
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_get(
      &s_ctx.settings, "value", s_ctx.output, sizeof(size_t),
      (size_t *)s_ctx.output, &found));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_get(
      &s_ctx.settings, "value", s_ctx.output, sizeof(bool),
      &length, (bool *)s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_settings_get(
      &s_ctx.settings, "value", NULL, 0u, &length, (bool *)&length));
  TEST_ASSERT_EQUAL_UINT(71u, length);
  TEST_ASSERT_FALSE(found);
  expect_byte("value", 4u);
}

void test_settings_reads_independent_wire_and_preserves_unknown_keys(void) {
  const uint8_t future[] = {0x00u, 0x1fu, 0xffu, 0x88u};
  size_t length = single_wire(1u, "future/vendor/key", 17u, future, sizeof(future));
  persist_wire(length);
  init_settings();
  expect_value("future/vendor/key", future, sizeof(future));
  set_byte("network/port", 3u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  reopen_settings();
  expect_value("future/vendor/key", future, sizeof(future));
  expect_byte("network/port", 3u);
}

void test_settings_emits_documented_little_endian_snapshot(void) {
  init_settings();
  set_byte("a", 0x7fu);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_set(&s_ctx.settings, "empty", NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  size_t length = 0u;
  esy_file_info_t stored;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_get_info(&s_ctx.file, &stored));
  length = stored.length;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_read(&s_ctx.file, 0u, s_ctx.output, length));
  const uint8_t value = 0x7fu;
  size_t expected = 16u;
  expected += settings_fixture_entry(s_ctx.wire + expected, "a", 1u, &value, 1u);
  expected += settings_fixture_entry(s_ctx.wire + expected, "empty", 5u, NULL, 0u);
  settings_fixture_header(s_ctx.wire, 1u, 2u, (uint32_t)expected);
  TEST_ASSERT_EQUAL_UINT(expected, length);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.wire, s_ctx.output, expected);
}

void test_settings_unknown_magic_or_version_fail_without_overwriting_media(void) {
  for (uint32_t variant = 0u; variant < 2u; ++variant) {
    size_t length = single_wire(variant == 0u ? 2u : 1u, "key", 3u, "v", 1u);
    if (variant == 1u) settings_fixture_u32(s_ctx.wire, 0x12345678u);
    persist_wire(length);
    memcpy(s_ctx.saved_media, s_ctx.media, sizeof(s_ctx.media));
    reset_faults_and_counts();
    TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED,
        esy_settings_init(&s_ctx.settings, &s_ctx.config));
    esy_settings_t zero = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.settings, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_media, s_ctx.media, sizeof(s_ctx.media));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  }
}

void test_settings_rejects_malformed_snapshots_independently_of_the_record_crc(void) {
  const uint8_t embedded_nul[] = {'a', '\0', 'b'};
  for (uint32_t variant = 0u; variant < 13u; ++variant) {
    size_t length = single_wire(1u, "key", 3u, "v", 1u);
    switch (variant) {
      case 0u: length = 0u; break;
      case 1u: length = 15u; break;
      case 2u: settings_fixture_u32(s_ctx.wire + 12u, (uint32_t)length + 1u); break;
      case 3u: settings_fixture_u32(s_ctx.wire + 8u, 0u); break;
      case 4u: settings_fixture_u32(s_ctx.wire + 8u, 2u); break;
      case 5u: settings_fixture_u32(s_ctx.wire + 16u, 0u); break;
      case 6u: settings_fixture_u32(s_ctx.wire + 16u, UINT32_MAX); break;
      case 7u: settings_fixture_u32(s_ctx.wire + 20u, UINT32_MAX); break;
      case 8u:
        length = single_wire(1u, embedded_nul, sizeof(embedded_nul), "v", 1u);
        break;
      case 9u:
        length += settings_fixture_entry(s_ctx.wire + length, "key", 3u, "w", 1u);
        settings_fixture_header(s_ctx.wire, 1u, 2u, (uint32_t)length);
        break;
      case 10u:
        s_ctx.wire[length++] = 0u;
        settings_fixture_u32(s_ctx.wire + 12u, (uint32_t)length);
        break;
      case 11u:
        length = 20u;
        settings_fixture_u32(s_ctx.wire + 12u, (uint32_t)length);
        break;
      default:
        settings_fixture_u32(s_ctx.wire + 8u, UINT32_MAX);
        break;
    }
    persist_wire(length);
    reset_faults_and_counts();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ESY_ERR_CRC_FAIL,
        esy_settings_init(&s_ctx.settings, &s_ctx.config), "Malformed wire accepted");
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  }
}

void test_settings_existing_snapshot_larger_than_resources_fails_without_truncation(void) {
  memset(s_ctx.output, 0x12, sizeof(s_ctx.output));
  size_t length = single_wire(1u, "large", 5u, s_ctx.output, TEST_BUFFER_BYTES);
  persist_wire(length);
  reset_faults_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
}

void test_settings_loaded_key_over_budget_is_preserved_until_a_larger_limit_is_supplied(void) {
  size_t length = single_wire(1u, "future/key", 10u, "value", 5u);
  persist_wire(length);
  s_ctx.config.max_key_length = 9u;
  memcpy(s_ctx.saved_media, s_ctx.media, sizeof(s_ctx.media));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_media, s_ctx.media, sizeof(s_ctx.media));
  s_ctx.config.max_key_length = 10u;
  init_settings();
  expect_value("future/key", "value", 5u);
}

void test_settings_media_capacity_limits_working_capacity_and_new_entries(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, detach_record_file());
  s_ctx.record_config.regions[0].size_bytes = 256u;
  s_ctx.record_config.regions[1].size_bytes = 256u;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      attach_record_file());
  init_settings();
  TEST_ASSERT_EQUAL_UINT(192u, settings_info().capacity);
  memset(s_ctx.output, 0x21, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
      esy_settings_set(&s_ctx.settings, "key", s_ctx.output, 166u));
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_settings_set(&s_ctx.settings, "key", s_ctx.output, 165u));
  TEST_ASSERT_EQUAL_UINT(192u, settings_info().used_bytes);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  reopen_settings();
  TEST_ASSERT_EQUAL_UINT(192u, settings_info().used_bytes);
}

void test_settings_reload_rejects_unsupported_or_malformed_persistence_before_publication(void) {
  for (uint32_t variant = 0u; variant < 2u; ++variant) {
    size_t length = single_wire(variant == 0u ? 2u : 1u, "key", 3u, "v", 1u);
    if (variant == 1u) settings_fixture_u32(s_ctx.wire + 20u, UINT32_MAX);
    persist_wire(length);
    memcpy(s_ctx.saved_media, s_ctx.media, sizeof(s_ctx.media));
    size_t valid_length = single_wire(1u, "key", 3u, "v", 1u);
    persist_wire(valid_length);
    init_settings();
    set_byte("staged", 7u);
    memcpy(s_ctx.saved_work, s_ctx.work, sizeof(s_ctx.work));
    esy_settings_info_t info = settings_info();
    /* Replace fake media while idle to inject an independently encoded image. */
    memcpy(s_ctx.media, s_ctx.saved_media, sizeof(s_ctx.media));
    TEST_ASSERT_EQUAL_INT(variant == 0u ? ESY_ERR_NOT_SUPPORTED : ESY_ERR_CRC_FAIL,
        esy_settings_reload(&s_ctx.settings));
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_work, s_ctx.work, sizeof(s_ctx.work));
    expect_info_equal(info);
    expect_byte("staged", 7u);
    expect_value("key", "v", 1u);
  }
}

void test_settings_two_invalid_record_regions_report_no_snapshot_without_formatting(void) {
  memset(s_ctx.media + TEST_REGION_OFFSET, 0x23, 2u * TEST_REGION_BYTES);
  init_settings();
  TEST_ASSERT_FALSE(settings_info().has_snapshot);
  TEST_ASSERT_EQUAL_UINT32(0u, settings_info().entry_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  TEST_ASSERT_EACH_EQUAL_HEX8(0x23u, s_ctx.media + TEST_REGION_OFFSET,
      2u * TEST_REGION_BYTES);
}

static void stage_next_transaction(void) {
  set_byte("address", 5u);
  set_byte("port", 6u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_remove(&s_ctx.settings, "obsolete"));
  set_byte("added", 7u);
}

static void transaction_fixture(void) {
  init_settings();
  set_byte("address", 1u);
  set_byte("port", 2u);
  set_byte("obsolete", 3u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  set_byte("address", 4u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  memcpy(s_ctx.saved_media, s_ctx.media, sizeof(s_ctx.media));
  stage_next_transaction();
  reset_faults_and_counts();
}

static void restore_transaction(void) {
  reset_faults_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, detach_record_file());
  memcpy(s_ctx.media, s_ctx.saved_media, sizeof(s_ctx.media));
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      attach_record_file());
  init_settings();
  stage_next_transaction();
  reset_faults_and_counts();
}

static bool expect_atomic_recovery(void) {
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_UNCERTAIN, settings_info().state);
  TEST_ASSERT_TRUE(settings_info().has_snapshot);
  expect_byte("address", 5u);
  expect_byte("port", 6u);
  expect_absent("obsolete");
  expect_byte("added", 7u);
  uint8_t value = 0u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_settings_set(&s_ctx.settings, "address", &value, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_settings_remove(&s_ctx.settings, "absent"));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_settings_commit(&s_ctx.settings));
  reset_faults_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_reload(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_CLEAN, settings_info().state);
  size_t length = 0u;
  bool found = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_get(
      &s_ctx.settings, "address", &value, sizeof(value), &length, &found));
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL_UINT(1u, length);
  TEST_ASSERT_TRUE(value == 4u || value == 5u);
  const bool committed = value == 5u;
  expect_byte("port", committed ? 6u : 2u);
  if (committed) {
    expect_absent("obsolete");
    expect_byte("added", 7u);
  } else {
    expect_byte("obsolete", 3u);
    expect_absent("added");
  }
  TEST_ASSERT_EQUAL_UINT32(3u, settings_info().entry_count);
  /* The previously current region and all bytes outside the reservation survive. */
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_media + TEST_REGION_OFFSET + TEST_REGION_BYTES,
      s_ctx.media + TEST_REGION_OFFSET + TEST_REGION_BYTES, TEST_REGION_BYTES);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xffu, s_ctx.media, TEST_REGION_OFFSET);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xffu,
      s_ctx.media + TEST_REGION_OFFSET + 2u * TEST_REGION_BYTES,
      TEST_FLASH_BYTES - TEST_REGION_OFFSET - 2u * TEST_REGION_BYTES);
  reopen_settings();
  expect_byte("address", committed ? 5u : 4u);
  expect_byte("port", committed ? 6u : 2u);
  return committed;
}

void test_settings_each_program_failure_prefix_recovers_one_complete_snapshot(void) {
  transaction_fixture();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  uint32_t writes = s_ctx.flash->write_count;
  TEST_ASSERT_GREATER_THAN_UINT32(0u, writes);
  for (uint32_t call = 1u; call <= writes; ++call) {
    for (size_t prefix = 0u; prefix <= TEST_PROGRAM_BYTES; ++prefix) {
      restore_transaction();
      s_ctx.flash->fail_write_call = call;
      s_ctx.flash->write_fail_bytes = prefix;
      TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_commit(&s_ctx.settings));
      (void)expect_atomic_recovery();
    }
  }
  restore_transaction();
  s_ctx.flash->fail_write_call = writes;
  s_ctx.flash->write_fail_bytes = SIZE_MAX;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_TRUE(expect_atomic_recovery());
}

void test_settings_each_erase_failure_prefix_preserves_previous_snapshot(void) {
  transaction_fixture();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  uint32_t erases = s_ctx.flash->erase_count;
  TEST_ASSERT_GREATER_THAN_UINT32(0u, erases);
  const size_t prefixes[] = {0u, 1u, 31u, 255u, 256u, TEST_REGION_BYTES, SIZE_MAX};
  for (uint32_t call = 1u; call <= erases; ++call) {
    for (size_t index = 0u; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
      restore_transaction();
      s_ctx.flash->fail_erase_call = call;
      s_ctx.flash->erase_fail_bytes = prefixes[index];
      TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_commit(&s_ctx.settings));
      TEST_ASSERT_FALSE(expect_atomic_recovery());
    }
  }
}

void test_settings_each_commit_read_failure_recovers_and_final_failure_can_be_committed(void) {
  transaction_fixture();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  uint32_t reads = s_ctx.flash->read_count;
  TEST_ASSERT_GREATER_THAN_UINT32(0u, reads);
  const size_t prefixes[] = {0u, 1u, SIZE_MAX};
  for (uint32_t call = 1u; call <= reads; ++call) {
    for (size_t index = 0u; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
      restore_transaction();
      s_ctx.flash->fail_read_call = call;
      s_ctx.flash->read_fail_bytes = prefixes[index];
      TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_commit(&s_ctx.settings));
      (void)expect_atomic_recovery();
    }
  }
  restore_transaction();
  s_ctx.flash->fail_read_call = reads;
  s_ctx.flash->read_fail_bytes = SIZE_MAX;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_TRUE(expect_atomic_recovery());
}

void test_settings_every_reload_read_failure_preserves_working_bytes_and_query_state(void) {
  transaction_fixture();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_reload(&s_ctx.settings));
  uint32_t reads = s_ctx.flash->read_count;
  TEST_ASSERT_GREATER_THAN_UINT32(0u, reads);
  const size_t prefixes[] = {0u, 1u, SIZE_MAX};
  for (uint32_t call = 1u; call <= reads; ++call) {
    for (size_t index = 0u; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
      restore_transaction();
      memcpy(s_ctx.saved_work, s_ctx.work, sizeof(s_ctx.work));
      esy_settings_info_t info = settings_info();
      s_ctx.flash->fail_read_call = call;
      s_ctx.flash->read_fail_bytes = prefixes[index];
      TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_reload(&s_ctx.settings));
      TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_work, s_ctx.work, sizeof(s_ctx.work));
      expect_info_equal(info);
      expect_byte("address", 5u);
      expect_byte("port", 6u);
      expect_byte("added", 7u);
    }
  }
}

void test_settings_failed_reload_keeps_uncertain_and_deinit_never_saves_it(void) {
  transaction_fixture();
  s_ctx.flash->erase_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_commit(&s_ctx.settings));
  s_ctx.flash->erase_result = ESY_OK;
  s_ctx.flash->read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_reload(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_UNCERTAIN, settings_info().state);
  expect_byte("address", 5u);
  reset_faults_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->read_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
  init_settings();
  expect_byte("address", 4u);
}

void test_settings_first_save_error_keeps_last_confirmed_absence_until_reload(void) {
  init_settings();
  memcpy(s_ctx.saved_media, s_ctx.media, sizeof(s_ctx.media));
  set_byte("value", 9u);
  reset_faults_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  const uint32_t writes = s_ctx.flash->write_count;
  TEST_ASSERT_GREATER_THAN_UINT32(0u, writes);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, detach_record_file());
  memcpy(s_ctx.media, s_ctx.saved_media, sizeof(s_ctx.media));
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      attach_record_file());
  init_settings();
  set_byte("value", 9u);
  reset_faults_and_counts();
  s_ctx.flash->fail_write_call = writes;
  s_ctx.flash->write_fail_bytes = SIZE_MAX;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_UNCERTAIN, settings_info().state);
  TEST_ASSERT_FALSE(settings_info().has_snapshot);
  expect_byte("value", 9u);
  reset_faults_and_counts();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_reload(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_SETTINGS_CLEAN, settings_info().state);
  TEST_ASSERT_TRUE(settings_info().has_snapshot);
  expect_byte("value", 9u);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->write_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count);
}

void test_settings_snapshot_bytes_move_between_flash_and_ram_without_reencoding(void) {
  init_settings();
  set_byte("bus/address", 42u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_set(&s_ctx.settings, "empty", NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  esy_file_info_t info;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_get_info(&s_ctx.file, &info));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_read(&s_ctx.file, 0u, s_ctx.ram_data, info.length));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_deinit(&s_ctx.file));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_dual_flash_store_deinit(&s_ctx.file_adapter));
  const esy_file_ram_static_resources_t resources = {
      .data_buffer = s_ctx.ram_data, .capacity = sizeof(s_ctx.ram_data)};
  const esy_file_ram_config_t config = {
      .static_resources = &resources, .exists = true, .initial_length = info.length};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_ram_init(&s_ctx.ram_adapter, &config));
  esy_file_ops_t ops;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_ram_get_ops(&s_ctx.ram_adapter, &ops));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_init(&s_ctx.file, &ops));
  reset_faults_and_counts();
  init_settings();
  expect_byte("bus/address", 42u);
  expect_value("empty", NULL, 0u);
  set_byte("bus/address", 43u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_commit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_settings_deinit(&s_ctx.settings));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_get_info(&s_ctx.file, &info));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->read_count + s_ctx.flash->write_count + s_ctx.flash->erase_count);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_deinit(&s_ctx.file));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_ram_deinit(&s_ctx.ram_adapter));
  const esy_file_dual_flash_store_config_t binding = {.store = &s_ctx.dual_flash_store};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_dual_flash_store_init(&s_ctx.file_adapter, &binding));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_dual_flash_store_get_ops(&s_ctx.file_adapter, &ops));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_init(&s_ctx.file, &ops));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.ram_data, info.length));
  init_settings();
  expect_byte("bus/address", 43u);
  expect_value("empty", NULL, 0u);
}

void test_settings_requires_atomic_replacement_instead_of_assuming_storage_semantics(void) {
  esy_file_ops_t ops;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_dual_flash_store_get_ops(&s_ctx.file_adapter, &ops));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_deinit(&s_ctx.file));
  ops.replace = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_file_init(&s_ctx.file, &ops));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED,
      esy_settings_init(&s_ctx.settings, &s_ctx.config));
  const esy_settings_t zero = {0};
  TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.settings, sizeof(zero));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.flash->erase_count + s_ctx.flash->write_count);
}
