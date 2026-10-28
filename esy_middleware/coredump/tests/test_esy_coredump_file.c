/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/coredump/esy_coredump.h"
#include "esy_middleware/coredump/esy_coredump_file.h"
#include "esy_middleware/file/esy_file.h"
#include "esy_middleware/file/esy_file_ram.h"
#include "esy_middleware/file/esy_file_flash.h"
#include "fake_esy_hal_flash.h"
#include "coredump_fixture.h"

#include <string.h>

#define TEST_FILE_BYTES 2048u
#define TEST_FLASH_BASE UINT32_C(0x10000)
#define TEST_FLASH_BYTES 12288u
#define TEST_FLASH_OFFSET 1024u
#define TEST_FLASH_REGION_BYTES 8192u
#define TEST_PROGRAM_BYTES 16u
#define TEST_SECTOR_BYTES 256u
#define TEST_IMAGE_A UINT32_C(0x10030000)
#define TEST_IMAGE_B UINT32_C(0x10210000)

typedef struct {
  esy_coredump_t store;
  esy_coredump_static_resources_t core_resources;
  esy_coredump_config_t core_config;
  fixture_id_source_t ids;
  esy_coredump_file_t adapter, copied;
  esy_coredump_file_config_t config;
  esy_coredump_file_static_resources_t resources;
  esy_file_t file;
  esy_file_ops_t backend_ops;
  esy_file_ram_t ram;
  esy_file_ram_static_resources_t ram_resources;
  esy_file_flash_t flash;
  esy_file_flash_static_resources_t flash_resources;
  fake_esy_hal_flash_state_t *hal;
  esy_err_t info_result, read_result, create_result, remove_result;
  bool commit_create_error, commit_remove_error, no_create, no_remove;
  unsigned infos, reads, creates, removes, fail_read_call;
  _Alignas(uint32_t) uint8_t retained[ESY_COREDUMP_MAX_RECORD_BYTES];
  uint8_t workspace[ESY_COREDUMP_MAX_RECORD_BYTES];
  uint8_t wire[ESY_COREDUMP_MAX_RECORD_BYTES];
  uint8_t saved_ram[ESY_COREDUMP_MAX_RECORD_BYTES];
  uint8_t data[TEST_FILE_BYTES], saved_data[TEST_FILE_BYTES];
  uint8_t media[TEST_FLASH_BYTES], marks[TEST_FLASH_BYTES];
  uint8_t program[TEST_SECTOR_BYTES];
} coredump_file_test_ctx_t;

static coredump_file_test_ctx_t s_ctx;

static esy_err_t provider_info(void *context, esy_file_info_t *info) {
  coredump_file_test_ctx_t *ctx = context;
  ++ctx->infos;
  return ctx->info_result != ESY_OK ? ctx->info_result
      : ctx->backend_ops.info(ctx->backend_ops.context, info);
}

static esy_err_t provider_read(void *context, uint32_t offset, void *buffer, size_t length) {
  coredump_file_test_ctx_t *ctx = context;
  ++ctx->reads;
  if (ctx->read_result != ESY_OK) return ctx->read_result;
  if (ctx->fail_read_call == ctx->reads) return ESY_ERR_IO;
  return ctx->backend_ops.read(ctx->backend_ops.context, offset, buffer, length);
}

static esy_err_t provider_create(void *context, const void *data, size_t length) {
  coredump_file_test_ctx_t *ctx = context;
  ++ctx->creates;
  if (ctx->create_result != ESY_OK && !ctx->commit_create_error) return ctx->create_result;
  const esy_err_t result = ctx->backend_ops.create(ctx->backend_ops.context, data, length);
  return result == ESY_OK ? ctx->create_result : result;
}

static esy_err_t provider_remove(void *context) {
  coredump_file_test_ctx_t *ctx = context;
  ++ctx->removes;
  if (ctx->remove_result != ESY_OK && !ctx->commit_remove_error) return ctx->remove_result;
  const esy_err_t result = ctx->backend_ops.remove(ctx->backend_ops.context);
  return result == ESY_OK ? ctx->remove_result : result;
}

static void bind_facade(void) {
  const esy_file_ops_t ops = {
      .info = provider_info, .read = provider_read,
      .create = s_ctx.no_create ? NULL : provider_create,
      .remove = s_ctx.no_remove ? NULL : provider_remove, .context = &s_ctx};
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_init(&s_ctx.file, &ops));
}

static void bind_ram(void) {
  s_ctx.ram_resources = (esy_file_ram_static_resources_t){s_ctx.data, sizeof(s_ctx.data)};
  const esy_file_ram_config_t config = {.static_resources = &s_ctx.ram_resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_init(&s_ctx.ram, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_get_ops(&s_ctx.ram, &s_ctx.backend_ops));
  bind_facade();
}

static void bind_flash(void) {
  s_ctx.flash_resources = (esy_file_flash_static_resources_t){s_ctx.program, sizeof(s_ctx.program)};
  const esy_file_flash_config_t config = {
      .address = TEST_FLASH_BASE + TEST_FLASH_OFFSET, .size = TEST_FLASH_REGION_BYTES,
      .static_resources = &s_ctx.flash_resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_init(&s_ctx.flash, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_get_ops(&s_ctx.flash, &s_ctx.backend_ops));
  bind_facade();
}

static void initialize(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_init(&s_ctx.adapter, &s_ctx.config));
}

static esy_coredump_info_t get_info(void) {
  esy_coredump_info_t info;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_get_info(&s_ctx.store, &info));
  return info;
}

static esy_coredump_info_t capture(void) {
  const esy_fault_snapshot_t snapshot = {
      .architecture = ESY_FAULT_ARCH_ARM_M, .reason = ESY_FAULT_REASON_SOFTWARE};
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_capture_fault(&snapshot, &s_ctx.store));
  return get_info();
}

static esy_coredump_record_id_t seed_file(uint8_t identity) {
  const size_t length = fixture_record(s_ctx.wire, TEST_IMAGE_A, identity, NULL, 0u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, s_ctx.wire, length));
  esy_coredump_record_id_t id;
  memcpy(id.bytes, s_ctx.wire + FIXTURE_RECORD_ID_OFFSET, sizeof(id.bytes));
  return id;
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  memset(s_ctx.retained, 0xa5, sizeof(s_ctx.retained));
  memset(s_ctx.media, 0xff, sizeof(s_ctx.media));
  fake_esy_hal_flash_reset();
  s_ctx.hal = fake_esy_hal_flash_get_state();
  s_ctx.hal->storage_base_addr = TEST_FLASH_BASE;
  s_ctx.hal->size_bytes = TEST_FLASH_BYTES;
  s_ctx.hal->sector_size = TEST_SECTOR_BYTES;
  s_ctx.hal->program_size = TEST_PROGRAM_BYTES;
  s_ctx.hal->backing_storage = s_ctx.media;
  s_ctx.hal->backing_storage_size = sizeof(s_ctx.media);
  s_ctx.hal->programmed_units = s_ctx.marks;
  s_ctx.hal->programmed_units_size = sizeof(s_ctx.marks);
  s_ctx.core_resources = (esy_coredump_static_resources_t){s_ctx.retained, sizeof(s_ctx.retained)};
  s_ctx.core_config = (esy_coredump_config_t){
      .static_resources = &s_ctx.core_resources, .image_base = TEST_IMAGE_B,
      .stack_limit = ESY_FAULT_STACK_MAX_BYTES, .new_record_id = fixture_new_record_id,
      .record_id_context = &s_ctx.ids};
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_init(&s_ctx.store, &s_ctx.core_config));
  s_ctx.resources = (esy_coredump_file_static_resources_t){s_ctx.workspace, sizeof(s_ctx.workspace)};
  s_ctx.config = (esy_coredump_file_config_t){
      .store = &s_ctx.store, .file = &s_ctx.file, .static_resources = &s_ctx.resources};
}

void tearDown(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_deinit(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_deinit(&s_ctx.ram));
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_deinit(&s_ctx.flash));
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_deinit(&s_ctx.store));
}

void test_coredump_file_init_and_detach_never_query_or_mutate_file_contents(void) {
  bind_ram();
  initialize();
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.infos + s_ctx.reads + s_ctx.creates + s_ctx.removes);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_init(&s_ctx.adapter, &s_ctx.config));
  s_ctx.copied = s_ctx.adapter;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_deinit(&s_ctx.copied));
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_deinit(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_deinit(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.infos + s_ctx.reads + s_ctx.creates + s_ctx.removes);
  initialize();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_persist(&s_ctx.adapter));
}

void test_coredump_file_ram_persistence_is_repeatable_and_never_uses_hal(void) {
  bind_ram();
  initialize();
  const esy_coredump_info_t original = capture();
  memcpy(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.creates);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.retained, s_ctx.data, original.record_size);
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.creates);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.hal->read_count + s_ctx.hal->write_count + s_ctx.hal->erase_count);
}

void test_coredump_file_first_persisted_record_wins_and_restore_preserves_its_identity(void) {
  bind_ram();
  const esy_coredump_record_id_t older = seed_file(0x12u);
  initialize();
  capture();
  const unsigned allocations = s_ctx.ids.calls;
  memcpy(s_ctx.saved_data, s_ctx.data, sizeof(s_ctx.saved_data));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_data, s_ctx.data, sizeof(s_ctx.saved_data));
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_restore(&s_ctx.adapter));
  const esy_coredump_info_t restored = get_info();
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_A, restored.image_base);
  TEST_ASSERT_EQUAL_MEMORY(older.bytes, restored.record_id.bytes, sizeof(older.bytes));
  TEST_ASSERT_EQUAL_UINT(allocations, s_ctx.ids.calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_clear(&s_ctx.adapter, &older));
}

void test_coredump_file_identity_guard_protects_both_authoritative_sources(void) {
  bind_ram();
  const esy_coredump_record_id_t older = seed_file(0x12u);
  initialize();
  const esy_coredump_record_id_t newer = capture().record_id;
  memcpy(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  memcpy(s_ctx.saved_data, s_ctx.data, sizeof(s_ctx.saved_data));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_clear(&s_ctx.adapter, &newer));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_clear(&s_ctx.adapter, &older));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.removes);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_data, s_ctx.data, sizeof(s_ctx.saved_data));
}

void test_coredump_file_clear_retains_the_expected_identity_when_workspace_is_reused(void) {
  struct {
    uint8_t prefix[FIXTURE_RECORD_ID_OFFSET];
    esy_coredump_record_id_t id;
    uint8_t suffix[ESY_COREDUMP_MAX_RECORD_BYTES - FIXTURE_RECORD_ID_OFFSET - ESY_COREDUMP_RECORD_ID_BYTES];
  } workspace = {0};
  bind_ram();
  seed_file(0x12u);
  s_ctx.resources.record_buffer = (uint8_t *)(void *)&workspace;
  s_ctx.resources.record_capacity = sizeof(workspace);
  initialize();
  workspace.id = capture().record_id;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_clear(&s_ctx.adapter, &workspace.id));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.removes);
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_deinit(&s_ctx.adapter));
}

void test_coredump_file_create_error_after_commit_reconciles_without_replacing_the_record(void) {
  bind_ram();
  initialize();
  capture();
  s_ctx.create_result = ESY_ERR_IO;
  s_ctx.commit_create_error = true;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_persist(&s_ctx.adapter));
  s_ctx.create_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.creates);
}

void test_coredump_file_failed_remove_preserves_ram_and_retry_accepts_an_absent_file(void) {
  bind_ram();
  initialize();
  const esy_coredump_record_id_t id = capture().record_id;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  memcpy(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  s_ctx.remove_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_clear(&s_ctx.adapter, &id));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  s_ctx.commit_remove_error = true;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_clear(&s_ctx.adapter, &id));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  s_ctx.remove_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_clear(&s_ctx.adapter, &id));
  esy_coredump_info_t info;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_get_info(&s_ctx.store, &info));
}

void test_coredump_file_failed_successor_allocation_keeps_ram_after_removing_the_file(void) {
  bind_ram();
  initialize();
  const esy_coredump_record_id_t id = capture().record_id;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  memcpy(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  s_ctx.ids.result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_clear(&s_ctx.adapter, &id));
  esy_file_info_t info;
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_info(&s_ctx.file, &info));
  TEST_ASSERT_FALSE(info.exists);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  s_ctx.ids.result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_clear(&s_ctx.adapter, &id));
}

void test_coredump_file_info_and_read_errors_never_authorize_create_remove_or_ram_restore(void) {
  bind_ram();
  seed_file(0x12u);
  initialize();
  const esy_coredump_record_id_t id = capture().record_id;
  memcpy(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  s_ctx.info_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_clear(&s_ctx.adapter, &id));
  s_ctx.info_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_clear(&s_ctx.adapter, &id));
  s_ctx.info_result = ESY_OK;
  s_ctx.read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_clear(&s_ctx.adapter, &id));
  s_ctx.read_result = ESY_OK;
  s_ctx.fail_read_call = s_ctx.reads + 2u;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.creates);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.removes);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
}

void test_coredump_file_malformed_and_unknown_records_are_not_overwritten_or_removed(void) {
  bind_ram();
  initialize();
  const esy_coredump_record_id_t id = capture().record_id;
  const uint8_t tiny[] = {1u, 2u, 3u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, tiny, sizeof(tiny)));
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_coredump_file_clear(&s_ctx.adapter, &id));
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
  const uint8_t unknown[] = {'E', 'C', 'D', '1', 0xffu, 0u, 0u, 0u};
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, unknown, sizeof(unknown)));
  const unsigned removes = s_ctx.removes;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_coredump_file_clear(&s_ctx.adapter, &id));
  TEST_ASSERT_EQUAL_UINT(removes, s_ctx.removes);
}

void test_coredump_file_corrupt_complete_record_leaves_both_sources_untouched(void) {
  bind_ram();
  seed_file(0x12u);
  initialize();
  const esy_coredump_record_id_t id = capture().record_id;
  s_ctx.data[FIXTURE_RECORD_ID_OFFSET] ^= 1u;
  memcpy(s_ctx.saved_data, s_ctx.data, sizeof(s_ctx.saved_data));
  memcpy(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  const unsigned creates = s_ctx.creates;
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_coredump_file_clear(&s_ctx.adapter, &id));
  TEST_ASSERT_EQUAL_UINT(creates, s_ctx.creates);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.removes);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_data, s_ctx.data, sizeof(s_ctx.saved_data));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
}

void test_coredump_file_recognizes_unknown_versions_before_workspace_size_bounds(void) {
  bind_ram();
  initialize();
  const esy_coredump_record_id_t id = capture().record_id;
  memset(s_ctx.saved_data, 0x5a, sizeof(s_ctx.saved_data));
  fixture_u32(s_ctx.saved_data, FIXTURE_MAGIC);
  fixture_u32(s_ctx.saved_data + 4u, 0xffu);
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, s_ctx.saved_data, sizeof(s_ctx.saved_data)));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_coredump_file_clear(&s_ctx.adapter, &id));
  fixture_u32(s_ctx.data + 4u, 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.removes);
}

void test_coredump_file_capacity_failure_occurs_at_persist_without_mutating_ram_or_file(void) {
  s_ctx.ram_resources = (esy_file_ram_static_resources_t){s_ctx.data, ESY_COREDUMP_HEADER_BYTES - 1u};
  const esy_file_ram_config_t config = {.static_resources = &s_ctx.ram_resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_init(&s_ctx.ram, &config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_get_ops(&s_ctx.ram, &s_ctx.backend_ops));
  bind_facade();
  initialize();
  capture();
  memcpy(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_coredump_file_persist(&s_ctx.adapter));
  esy_file_info_t info;
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_info(&s_ctx.file, &info));
  TEST_ASSERT_FALSE(info.exists);
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.creates);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
}

void test_coredump_file_init_requires_create_and_clear_requires_explicit_remove_capability(void) {
  s_ctx.no_create = true;
  bind_ram();
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_coredump_file_init(&s_ctx.adapter, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
  s_ctx.no_create = false;
  s_ctx.no_remove = true;
  bind_facade();
  initialize();
  const esy_coredump_record_id_t id = capture().record_id;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_coredump_file_clear(&s_ctx.adapter, &id));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.removes);
}

void test_coredump_file_invalid_storage_is_rejected_before_file_callbacks(void) {
  bind_ram();
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_coredump_file_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_coredump_file_init(&s_ctx.adapter, NULL));
  s_ctx.resources.record_capacity = ESY_COREDUMP_MAX_RECORD_BYTES - 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_coredump_file_init(&s_ctx.adapter, &s_ctx.config));
  s_ctx.resources.record_capacity = sizeof(s_ctx.workspace);
  s_ctx.resources.record_buffer = s_ctx.retained;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_coredump_file_init(&s_ctx.adapter, &s_ctx.config));
  s_ctx.resources.record_buffer = (uint8_t *)(void *)&s_ctx.file;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_coredump_file_init(&s_ctx.adapter, &s_ctx.config));
  s_ctx.resources.record_buffer = s_ctx.workspace;
  initialize();
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.infos + s_ctx.reads + s_ctx.creates + s_ctx.removes);
}

void test_coredump_file_stale_and_zero_clear_ids_have_no_provider_effects(void) {
  bind_ram();
  initialize();
  esy_coredump_record_id_t stale = capture().record_id;
  stale.bytes[0] ^= 1u;
  const esy_coredump_record_id_t zero = {0};
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_clear(&s_ctx.adapter, &stale));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_coredump_file_clear(&s_ctx.adapter, &zero));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_coredump_file_clear(&s_ctx.adapter, NULL));
  TEST_ASSERT_EQUAL_UINT(0u, s_ctx.infos + s_ctx.reads + s_ctx.creates + s_ctx.removes);
}

void test_coredump_file_real_flash_backend_persists_restores_and_clears_one_reserved_region(void) {
  bind_flash();
  initialize();
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.hal->read_count + s_ctx.hal->write_count + s_ctx.hal->erase_count);
  const esy_coredump_record_id_t id = capture().record_id;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  const uint32_t writes = s_ctx.hal->write_count;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  TEST_ASSERT_EQUAL_UINT32(writes, s_ctx.hal->write_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_restore(&s_ctx.adapter));
  const esy_coredump_record_id_t restored = get_info().record_id;
  TEST_ASSERT_EQUAL_MEMORY(id.bytes, restored.bytes, sizeof(id.bytes));
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_clear(&s_ctx.adapter, &id));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_coredump_file_restore(&s_ctx.adapter));
  for (size_t i = 0u; i < TEST_FLASH_OFFSET; ++i) TEST_ASSERT_EQUAL_HEX8(0xffu, s_ctx.media[i]);
  for (size_t i = TEST_FLASH_OFFSET + TEST_FLASH_REGION_BYTES; i < sizeof(s_ctx.media); ++i)
    TEST_ASSERT_EQUAL_HEX8(0xffu, s_ctx.media[i]);
}

void test_coredump_file_flash_errors_never_clear_retained_record(void) {
  bind_flash();
  initialize();
  const esy_coredump_record_id_t id = capture().record_id;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.adapter));
  memcpy(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  s_ctx.hal->read_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_restore(&s_ctx.adapter));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_clear(&s_ctx.adapter, &id));
  s_ctx.hal->read_result = ESY_OK;
  s_ctx.hal->erase_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_coredump_file_clear(&s_ctx.adapter, &id));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved_ram, s_ctx.retained, sizeof(s_ctx.saved_ram));
  s_ctx.hal->erase_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_clear(&s_ctx.adapter, &id));
}
