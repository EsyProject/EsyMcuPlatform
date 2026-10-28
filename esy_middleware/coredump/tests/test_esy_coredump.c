/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <limits.h>
#include <string.h>

#include "esy_middleware/coredump/esy_coredump.h"
#include "coredump_fixture.h"

#define TEST_STACK_LIMIT 16u
#define TEST_BUFFER_BYTES (FIXTURE_HEADER_BYTES + ESY_FAULT_STACK_MAX_BYTES)
#define TEST_IMAGE_A UINT32_C(0x10030000)
#define TEST_IMAGE_B UINT32_C(0x10210000)

typedef struct {
  esy_coredump_t store, copied;
  esy_coredump_static_resources_t resources;
  esy_coredump_config_t config;
  esy_fault_snapshot_t snapshot;
  fixture_id_source_t id_source;
  esy_coredump_record_id_t record_id;
  _Alignas(uint32_t) uint8_t retained[TEST_BUFFER_BYTES];
  uint8_t saved[TEST_BUFFER_BYTES], wire[TEST_BUFFER_BYTES], output[TEST_BUFFER_BYTES];
  uint8_t stack[64];
} test_ctx_t;

static test_ctx_t s_ctx;

static void init_store(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_init(&s_ctx.store, &s_ctx.config));
}

static esy_coredump_info_t get_info(void) {
  esy_coredump_info_t info = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_get_info(&s_ctx.store, &info));
  return info;
}

static void capture(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_capture_fault(&s_ctx.snapshot, &s_ctx.store));
  s_ctx.record_id = get_info().record_id;
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  memset(s_ctx.retained, 0xa5, sizeof(s_ctx.retained));
  for (size_t index = 0u; index < sizeof(s_ctx.stack); ++index) s_ctx.stack[index] = (uint8_t)index;
  s_ctx.resources.retained_buffer = s_ctx.retained;
  s_ctx.resources.retained_size = sizeof(s_ctx.retained);
  s_ctx.config.static_resources = &s_ctx.resources;
  s_ctx.config.new_record_id = fixture_new_record_id;
  s_ctx.config.record_id_context = &s_ctx.id_source;
  s_ctx.config.image_base = TEST_IMAGE_A;
  s_ctx.config.firmware_version = 10001u;
  s_ctx.config.stack_limit = TEST_STACK_LIMIT;
  memset(s_ctx.config.build_id, 0x12, sizeof(s_ctx.config.build_id));
  s_ctx.snapshot.architecture = FIXTURE_ARCH_ARM;
  s_ctx.snapshot.reason = ESY_FAULT_REASON_SOFTWARE;
  s_ctx.snapshot.core_id = 1u;
  s_ctx.snapshot.flags = ESY_FAULT_FLAG_CORE_REGS_VALID | ESY_FAULT_FLAG_STACK_VALID;
  s_ctx.snapshot.stack = s_ctx.stack;
  s_ctx.snapshot.stack_address = UINT32_C(0x20001000);
  s_ctx.snapshot.stack_length = sizeof(s_ctx.stack);
  for (size_t index = 0u; index < ESY_FAULT_REGISTER_COUNT; ++index) {
    s_ctx.snapshot.r[index] = (uint32_t)(index + 100u);
  }
  s_ctx.snapshot.cfsr = 0x1234u;
  s_ctx.snapshot.detail0 = 0x4567u;
  s_ctx.snapshot.msplim = 0x89abu;
}

void tearDown(void) {
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_deinit(&s_ctx.store));
}

void test_coredump_lifecycle_never_initializes_or_clears_retained_bytes(void) {
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  init_store();
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  esy_coredump_info_t info;
  memset(&info, 0x5a, sizeof(info));
  esy_coredump_info_t untouched = info;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_get_info(&s_ctx.store, &info));
  TEST_ASSERT_EQUAL_MEMORY(&untouched, &info, sizeof(info));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_deinit(&s_ctx.store));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_deinit(&s_ctx.store));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  init_store();
}

void test_coredump_capture_is_bounded_and_commits_interoperable_wire_record(void) {
  init_store();
  capture();
  esy_coredump_info_t info = get_info();
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_HEADER_BYTES + TEST_STACK_LIMIT, info.record_size);
  TEST_ASSERT_EQUAL_UINT32(TEST_STACK_LIMIT, info.stack_length);
  TEST_ASSERT_BITS_HIGH(ESY_COREDUMP_FLAG_STACK_TRUNCATED, info.flags);
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_A, info.image_base);
  TEST_ASSERT_EQUAL_UINT32(10001u, info.firmware_version);
  TEST_ASSERT_EQUAL_UINT32(1u, info.core_id);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.config.build_id, info.build_id, sizeof(info.build_id));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_read(&s_ctx.store, &s_ctx.record_id, 0u, s_ctx.output, info.record_size));
  TEST_ASSERT_EQUAL_HEX32(FIXTURE_MAGIC, fixture_read_u32(s_ctx.output));
  TEST_ASSERT_EQUAL_HEX32(fixture_record_crc(s_ctx.output, info.record_size), info.record_crc);
  TEST_ASSERT_EQUAL_HEX32(115u, fixture_read_u32(s_ctx.output + 156u));
  TEST_ASSERT_EQUAL_HEX32(0x1234u, fixture_read_u32(s_ctx.output + 224u));
  TEST_ASSERT_EQUAL_HEX32(0x4567u, fixture_read_u32(s_ctx.output + 252u));
  TEST_ASSERT_EQUAL_HEX32(0x89abu, fixture_read_u32(s_ctx.output + 260u));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.stack, s_ctx.output + FIXTURE_HEADER_BYTES, TEST_STACK_LIMIT);
  TEST_ASSERT_EQUAL_HEX8(0xa5u, s_ctx.retained[info.record_size]);
}

void test_coredump_first_record_survives_repeated_capture_and_reinitialization_in_other_slot(void) {
  init_store();
  capture();
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.snapshot.r[15] = UINT32_C(0xdeadbeef);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_coredump_capture_fault(&s_ctx.snapshot, &s_ctx.store));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_deinit(&s_ctx.store));
  s_ctx.config.image_base = TEST_IMAGE_B;
  s_ctx.config.firmware_version = 20002u;
  memset(s_ctx.config.build_id, 0x34, sizeof(s_ctx.config.build_id));
  init_store();
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_A, get_info().image_base);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_coredump_capture_fault(&s_ctx.snapshot, &s_ctx.store));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
}

void test_coredump_explicit_clear_allows_a_new_record_without_waiting_for_clock_wrap(void) {
  init_store();
  capture();
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_clear(&s_ctx.store, &s_ctx.record_id));
  esy_coredump_info_t info = {0};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_get_info(&s_ctx.store, &info));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_clear(&s_ctx.store, &s_ctx.record_id));
  s_ctx.snapshot.reason = ESY_FAULT_REASON_ASSERT;
  capture();
  TEST_ASSERT_EQUAL_UINT32(ESY_FAULT_REASON_ASSERT, get_info().reason);
}

void test_coredump_restore_prior_persistent_record_is_authoritative_across_ab_images(void) {
  init_store();
  capture();
  size_t length = fixture_record(s_ctx.wire, TEST_IMAGE_B, 0x34u, s_ctx.stack, 4u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_restore(&s_ctx.store, s_ctx.wire, length));
  esy_coredump_info_t info = get_info();
  s_ctx.record_id = info.record_id;
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_B, info.image_base);
  TEST_ASSERT_EQUAL_HEX8(0x34u, info.build_id[0]);
  TEST_ASSERT_EQUAL_UINT32(4u, info.stack_length);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_read(&s_ctx.store, &s_ctx.record_id, 0u, s_ctx.output, length));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.wire, s_ctx.output, length);
}

void test_coredump_committed_unknown_version_is_protected_from_new_faults(void) {
  size_t length = fixture_record(s_ctx.retained, TEST_IMAGE_B, 0x34u, s_ctx.stack, 0u);
  fixture_u32(s_ctx.retained + 4u, 2u);
  fixture_u32(s_ctx.retained + 12u, fixture_record_crc(s_ctx.retained, length));
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  init_store();
  esy_coredump_info_t info = {0};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, esy_coredump_get_info(&s_ctx.store, &info));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, esy_coredump_capture_fault(&s_ctx.snapshot, &s_ctx.store));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
}

void test_coredump_corruption_and_invalid_restore_leave_existing_record_and_outputs_unchanged(void) {
  init_store();
  capture();
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  size_t length = fixture_record(s_ctx.wire, TEST_IMAGE_B, 0x34u, s_ctx.stack, 4u);
  s_ctx.wire[FIXTURE_HEADER_BYTES] ^= 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL, esy_coredump_restore(&s_ctx.store, s_ctx.wire, length));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.retained[96u] ^= 1u;
  esy_coredump_info_t info;
  memset(&info, 0x5a, sizeof(info));
  esy_coredump_info_t untouched = info;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL, esy_coredump_get_info(&s_ctx.store, &info));
  TEST_ASSERT_EQUAL_MEMORY(&untouched, &info, sizeof(info));
  memset(s_ctx.output, 0x5a, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL, esy_coredump_read(&s_ctx.store, &s_ctx.record_id, 0u, s_ctx.output, 1u));
  TEST_ASSERT_EQUAL_HEX8(0x5au, s_ctx.output[0]);
}

void test_coredump_corrupt_committed_record_can_be_replaced_by_a_valid_new_fault(void) {
  init_store();
  capture();
  s_ctx.retained[96u] ^= 1u;
  s_ctx.snapshot.reason = ESY_FAULT_REASON_ASSERT;
  capture();
  TEST_ASSERT_EQUAL_UINT32(ESY_FAULT_REASON_ASSERT, get_info().reason);
}

void test_coredump_raw_validation_checks_exact_size_crc_and_optional_metadata(void) {
  size_t length = fixture_record(s_ctx.wire, TEST_IMAGE_B, 0x34u, s_ctx.stack, 4u);
  esy_coredump_info_t info = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_validate(s_ctx.wire, length, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_validate(s_ctx.wire, length, &info));
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_B, info.image_base);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_validate(NULL, length, &info));
  TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_coredump_validate(s_ctx.wire, length - 1u, &info));
  TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_coredump_validate(s_ctx.wire, length + 1u, &info));
  s_ctx.wire[length - 1u] ^= 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL, esy_coredump_validate(s_ctx.wire, length, &info));
}

void test_coredump_read_rejects_overflow_ranges_aliases_and_invalid_arguments(void) {
  init_store();
  capture();
  size_t total = get_info().record_size;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_read(&s_ctx.store, &s_ctx.record_id, total, NULL, 0u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_read(&s_ctx.store, &s_ctx.record_id, total, s_ctx.output, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_read(&s_ctx.store, &s_ctx.record_id, SIZE_MAX, s_ctx.output, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_read(&s_ctx.store, &s_ctx.record_id, 0u, NULL, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_read(&s_ctx.store, &s_ctx.record_id, 0u, s_ctx.retained, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_get_info(&s_ctx.store, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_capture_fault(NULL, &s_ctx.store));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_capture_fault(&s_ctx.snapshot, NULL));
}

void test_coredump_invalid_configuration_is_reusable_and_live_handle_cannot_be_copied(void) {
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_init(&s_ctx.store, NULL));
  s_ctx.resources.retained_buffer++;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  s_ctx.resources.retained_buffer--;
  size_t capacity = s_ctx.resources.retained_size;
  s_ctx.resources.retained_size = FIXTURE_HEADER_BYTES - 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  s_ctx.resources.retained_size = capacity;
  s_ctx.config.stack_limit = ESY_FAULT_STACK_MAX_BYTES + 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  s_ctx.config.stack_limit = TEST_STACK_LIMIT;
  init_store();
  s_ctx.copied = s_ctx.store;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_deinit(&s_ctx.copied));
  capture();
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_A, get_info().image_base);
}

void test_coredump_zero_stack_limit_records_registers_without_stack_bytes(void) {
  s_ctx.config.stack_limit = 0u;
  init_store();
  capture();
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_HEADER_BYTES, get_info().record_size);
  TEST_ASSERT_EQUAL_UINT32(0u, get_info().stack_length);
}

void test_coredump_identity_is_prepared_only_in_normal_context_and_survives_reinitialization(void) {
  init_store();
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.id_source.calls);
  capture();
  const esy_coredump_record_id_t original = get_info().record_id;
  TEST_ASSERT_EQUAL_MEMORY(original.bytes,
      s_ctx.retained + FIXTURE_RECORD_ID_OFFSET, ESY_COREDUMP_RECORD_ID_BYTES);
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.id_source.calls);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY,
      esy_coredump_capture_fault(&s_ctx.snapshot, &s_ctx.store));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.id_source.calls);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_read(&s_ctx.store, &original,
      0u, s_ctx.output, get_info().record_size));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.id_source.calls);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_deinit(&s_ctx.store));
  s_ctx.config.image_base = TEST_IMAGE_B;
  init_store();
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.id_source.calls);
  const esy_coredump_info_t info = get_info();
  TEST_ASSERT_EQUAL_MEMORY(original.bytes, info.record_id.bytes, sizeof(original.bytes));
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_A, info.image_base);
}

void test_coredump_stale_or_zero_identity_never_reads_clears_or_allocates(void) {
  init_store();
  capture();
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  esy_coredump_record_id_t stale = s_ctx.record_id;
  stale.bytes[ESY_COREDUMP_RECORD_ID_BYTES - 1u] ^= 1u;
  memset(s_ctx.output, 0x5a, sizeof(s_ctx.output));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE,
      esy_coredump_read(&s_ctx.store, &stale, 0u, s_ctx.output, 1u));
  TEST_ASSERT_EQUAL_HEX8(0x5au, s_ctx.output[0]);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_clear(&s_ctx.store, &stale));
  const esy_coredump_record_id_t zero = {0};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_coredump_read(&s_ctx.store, &zero, 0u, s_ctx.output, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_clear(&s_ctx.store, &zero));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_coredump_read(&s_ctx.store, NULL, 0u, s_ctx.output, 1u));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_clear(&s_ctx.store, NULL));
  TEST_ASSERT_EQUAL_UINT(1u, s_ctx.id_source.calls);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
}

void test_coredump_identical_faults_after_clear_have_distinct_occurrence_identities(void) {
  init_store();
  capture();
  const esy_coredump_record_id_t old_id = s_ctx.record_id;
  const uint32_t old_crc = get_info().record_crc;
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_clear(&s_ctx.store, &old_id));
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.id_source.calls);
  capture();
  TEST_ASSERT_NOT_EQUAL(0, memcmp(old_id.bytes, s_ctx.record_id.bytes, sizeof(old_id.bytes)));
  TEST_ASSERT_NOT_EQUAL(old_crc, get_info().record_crc);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved + 16u, s_ctx.retained + 16u,
                           FIXTURE_RECORD_ID_OFFSET - 16u);
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved + FIXTURE_RECORD_ID_OFFSET + ESY_COREDUMP_RECORD_ID_BYTES,
      s_ctx.retained + FIXTURE_RECORD_ID_OFFSET + ESY_COREDUMP_RECORD_ID_BYTES,
      get_info().record_size - FIXTURE_RECORD_ID_OFFSET - ESY_COREDUMP_RECORD_ID_BYTES);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_clear(&s_ctx.store, &old_id));
  TEST_ASSERT_EQUAL_UINT(2u, s_ctx.id_source.calls);
}

void test_coredump_failed_zero_or_reused_successor_identity_preserves_the_current_record(void) {
  init_store();
  capture();
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.id_source.result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_coredump_clear(&s_ctx.store, &s_ctx.record_id));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.id_source.result = ESY_OK;
  s_ctx.id_source.zero = true;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_clear(&s_ctx.store, &s_ctx.record_id));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.id_source.zero = false;
  s_ctx.id_source.repeat = true;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_clear(&s_ctx.store, &s_ctx.record_id));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.id_source.repeat = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_clear(&s_ctx.store, &s_ctx.record_id));
  capture();
}

void test_coredump_clear_snapshots_an_identity_aliasing_mutable_allocator_context(void) {
  init_store();
  capture();
  const esy_coredump_record_id_t first = s_ctx.id_source.last;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_coredump_clear(&s_ctx.store, &s_ctx.id_source.last));
  capture();
  TEST_ASSERT_NOT_EQUAL(0,
      memcmp(first.bytes, s_ctx.record_id.bytes, sizeof(first.bytes)));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.id_source.last.bytes, s_ctx.record_id.bytes,
                           sizeof(s_ctx.record_id.bytes));
}

void test_coredump_restore_preserves_persistent_identity_without_allocating_a_new_one(void) {
  init_store();
  capture();
  const unsigned calls = s_ctx.id_source.calls;
  const size_t length = fixture_record(s_ctx.wire, TEST_IMAGE_B, 0x34u, s_ctx.stack, 4u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_restore(&s_ctx.store, s_ctx.wire, length));
  const esy_coredump_info_t info = get_info();
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.wire + FIXTURE_RECORD_ID_OFFSET,
                           info.record_id.bytes, ESY_COREDUMP_RECORD_ID_BYTES);
  TEST_ASSERT_EQUAL_UINT(calls, s_ctx.id_source.calls);
  s_ctx.id_source.repeat = true;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_clear(&s_ctx.store, &info.record_id));
  s_ctx.id_source.repeat = false;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_coredump_clear(&s_ctx.store, &info.record_id));
}

void test_coredump_identity_bytes_are_crc_protected_and_zero_identity_is_invalid(void) {
  const size_t length = fixture_record(s_ctx.wire, TEST_IMAGE_B, 0x34u, s_ctx.stack, 4u);
  s_ctx.wire[FIXTURE_RECORD_ID_OFFSET + ESY_COREDUMP_RECORD_ID_BYTES - 1u] ^= 1u;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL, esy_coredump_validate(s_ctx.wire, length, NULL));
  memset(s_ctx.wire + FIXTURE_RECORD_ID_OFFSET, 0, ESY_COREDUMP_RECORD_ID_BYTES);
  fixture_u32(s_ctx.wire + 12u, fixture_record_crc(s_ctx.wire, length));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_CRC_FAIL, esy_coredump_validate(s_ctx.wire, length, NULL));
}

void test_coredump_invalid_identity_source_leaves_zero_handle_and_retained_bytes_reusable(void) {
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.config.new_record_id = NULL;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  s_ctx.config.new_record_id = fixture_new_record_id;
  s_ctx.id_source.result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  s_ctx.id_source.result = ESY_OK;
  s_ctx.id_source.zero = true;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.id_source.zero = false;
  init_store();
}

void test_coredump_init_rejects_reusing_an_existing_valid_record_identity(void) {
  fixture_record(s_ctx.retained, TEST_IMAGE_A, 0x12u, s_ctx.stack, 4u);
  memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  memcpy(s_ctx.id_source.last.bytes, s_ctx.retained + FIXTURE_RECORD_ID_OFFSET,
         ESY_COREDUMP_RECORD_ID_BYTES);
  s_ctx.id_source.repeat = true;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_coredump_init(&s_ctx.store, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
  s_ctx.id_source.repeat = false;
  init_store();
}
