/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "unity.h"
#include "esy_middleware/file/esy_file.h"
#include "esy_middleware/file/esy_file_ram.h"
#include "esy_middleware/file/esy_file_flash.h"
#include "esy_middleware/file/esy_file_dual_flash_store.h"
#include "fake_esy_hal_flash.h"
#include <string.h>

#define TEST_MEDIA_BYTES 8192u
#define TEST_REGION_BYTES 4096u
#define TEST_BASE UINT32_C(0x10000)
#define TEST_PROGRAM_BYTES 16u
#define TEST_FOOTER_BYTES 32u
#define TEST_ALL_CAPS 15u

typedef struct {
    esy_file_t file;
    esy_file_ram_t ram;
    esy_file_flash_t flash;
    esy_file_dual_flash_store_t adapter;
    esy_dual_flash_store_t record;
    esy_file_ops_t ops;
    uint8_t bytes[256], output[256], media[TEST_MEDIA_BYTES], saved[TEST_MEDIA_BYTES];
    uint8_t program[TEST_PROGRAM_BYTES], record_work[TEST_PROGRAM_BYTES];
    uint8_t marks[TEST_MEDIA_BYTES];
    fake_esy_hal_flash_state_t *hal;
    esy_err_t result;
    unsigned info_calls, read_calls;
    bool reenter;
    esy_file_info_t snapshot;
} file_test_ctx_t;
static file_test_ctx_t s_ctx;

static void bind_facade(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_init(&s_ctx.file, &s_ctx.ops));
}
static void bind_ram(void) {
    const esy_file_ram_static_resources_t resources = {s_ctx.bytes, sizeof(s_ctx.bytes)};
    const esy_file_ram_config_t config = {.static_resources = &resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_init(&s_ctx.ram, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_get_ops(&s_ctx.ram, &s_ctx.ops));
    bind_facade();
}
static esy_file_flash_config_t flash_config(esy_file_flash_static_resources_t *resources) {
    *resources = (esy_file_flash_static_resources_t){s_ctx.program, sizeof(s_ctx.program)};
    return (esy_file_flash_config_t){TEST_BASE, TEST_REGION_BYTES, resources};
}
static void bind_flash(void) {
    esy_file_flash_static_resources_t resources;
    const esy_file_flash_config_t config = flash_config(&resources);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_init(&s_ctx.flash, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_get_ops(&s_ctx.flash, &s_ctx.ops));
    bind_facade();
}
static void bind_flash_unit(uint32_t unit) {
    s_ctx.hal->program_size = unit;
    const esy_file_flash_static_resources_t resources = {s_ctx.bytes, sizeof(s_ctx.bytes)};
    const esy_file_flash_config_t config = {TEST_BASE, TEST_REGION_BYTES, &resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_init(&s_ctx.flash, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_get_ops(&s_ctx.flash, &s_ctx.ops));
    bind_facade();
}
static void bind_record(void) {
    const esy_dual_flash_store_static_resources_t resources = {s_ctx.record_work, sizeof(s_ctx.record_work)};
    const esy_dual_flash_store_config_t config = {
        .regions = {{TEST_BASE, TEST_REGION_BYTES}, {TEST_BASE + TEST_REGION_BYTES, TEST_REGION_BYTES}},
        .static_resources = &resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_dual_flash_store_init(&s_ctx.record, &config));
    const esy_file_dual_flash_store_config_t binding = {&s_ctx.record};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_init(&s_ctx.adapter, &binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_get_ops(&s_ctx.adapter, &s_ctx.ops));
    bind_facade();
}
static esy_file_info_t info(void) {
    esy_file_info_t snapshot;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_info(&s_ctx.file, &snapshot));
    return snapshot;
}
static void expect_bytes(const void *data, size_t length) {
    TEST_ASSERT_TRUE(info().exists);
    TEST_ASSERT_EQUAL_UINT32(length, info().length);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_read(&s_ctx.file, 0u, s_ctx.output, length));
    if (length != 0u) TEST_ASSERT_EQUAL_MEMORY(data, s_ctx.output, length);
}
static esy_err_t source_info(void *context, esy_file_info_t *snapshot) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
    ++s_ctx.info_calls;
    if (s_ctx.reenter) {
        uint32_t caps = 0xA5u;
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_file_get_capabilities(&s_ctx.file, &caps));
        TEST_ASSERT_EQUAL_HEX32(0xA5u, caps);
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_file_deinit(&s_ctx.file));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_file_init(&s_ctx.file, &s_ctx.ops));
    }
    *snapshot = s_ctx.snapshot;
    return s_ctx.result;
}
static esy_err_t source_read(void *context, uint32_t offset, void *buffer, size_t length) {
    (void)context; (void)offset; (void)buffer; (void)length;
    ++s_ctx.read_calls;
    return s_ctx.result;
}
static void bind_source(void) {
    s_ctx.ops = (esy_file_ops_t){.info = source_info, .read = source_read, .context = &s_ctx};
    bind_facade();
}
void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(s_ctx.media, UINT8_MAX, sizeof(s_ctx.media));
    fake_esy_hal_flash_reset();
    s_ctx.hal = fake_esy_hal_flash_get_state();
    s_ctx.hal->storage_base_addr = TEST_BASE;
    s_ctx.hal->size_bytes = sizeof(s_ctx.media);
    s_ctx.hal->sector_size = 256u;
    s_ctx.hal->program_size = TEST_PROGRAM_BYTES;
    s_ctx.hal->backing_storage = s_ctx.media;
    s_ctx.hal->backing_storage_size = sizeof(s_ctx.media);
    s_ctx.hal->programmed_units = s_ctx.marks;
    s_ctx.hal->programmed_units_size = sizeof(s_ctx.marks);
}
void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_deinit(&s_ctx.ram));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_deinit(&s_ctx.flash));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.record));
}
void test_facade_capabilities_do_not_query_or_initialize_the_backend(void) {
    bind_source();
    uint32_t caps = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_capabilities(&s_ctx.file, &caps));
    TEST_ASSERT_EQUAL_UINT32(ESY_FILE_CAP_READ, caps);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.info_calls);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_replace(&s_ctx.file, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_create(&s_ctx.file, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_remove(&s_ctx.file));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.info_calls);
}
void test_facade_rejects_reentry_and_does_not_expose_invalid_metadata(void) {
    bind_source();
    s_ctx.reenter = true;
    esy_file_info_t output = {.length = 0xA5u};
    s_ctx.snapshot = (esy_file_info_t){.length = 1u, .capacity = 1u};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_get_info(&s_ctx.file, &output));
    TEST_ASSERT_EQUAL_HEX32(0xA5u, output.length);
    s_ctx.snapshot = (esy_file_info_t){.exists = true, .length = 2u, .capacity = 1u};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_get_info(&s_ctx.file, &output));
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_file_get_info(&s_ctx.file, &output));
    s_ctx.result = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_get_info(&s_ctx.file, &output));
    TEST_ASSERT_EQUAL_HEX32(0xA5u, output.length);
}
void test_facade_validates_lifecycle_arguments_and_unused_provider_capabilities(void) {
    esy_file_ops_t missing = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_init(NULL, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_init(&s_ctx.file, &missing));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_read(&s_ctx.file, 0u, NULL, 0u));
    uint32_t caps = 0xA5u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_get_capabilities(&s_ctx.file, &caps));
    TEST_ASSERT_EQUAL_HEX32(0xA5u, caps);
    bind_source();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_init(&s_ctx.file, &s_ctx.ops));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_get_capabilities(&s_ctx.file, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_get_info(&s_ctx.file, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_read(&s_ctx.file, UINT32_MAX, s_ctx.output, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_read(&s_ctx.file, 0u, &s_ctx.file, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.info_calls);
}
void test_ram_empty_file_differs_from_absence_and_remove_preserves_content_bytes(void) {
    bind_ram();
    TEST_ASSERT_FALSE(info().exists);
    TEST_ASSERT_EQUAL_UINT32(TEST_ALL_CAPS, info().capabilities);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_read(&s_ctx.file, 0u, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, NULL, 0u));
    expect_bytes(NULL, 0u);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_file_create(&s_ctx.file, "new", 3u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, "saved", 5u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
    TEST_ASSERT_FALSE(info().exists);
    TEST_ASSERT_EQUAL_MEMORY("saved", s_ctx.bytes, 5u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
}
void test_ram_replacement_and_reads_are_bounded_and_reject_borrowed_storage_aliases(void) {
    bind_ram();
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, "abcdef", 6u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_read(&s_ctx.file, 2u, s_ctx.output, 3u));
    TEST_ASSERT_EQUAL_MEMORY("cde", s_ctx.output, 3u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_read(&s_ctx.file, 6u, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_read(&s_ctx.file, 7u, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_read(&s_ctx.file, 0u, s_ctx.bytes, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_replace(&s_ctx.file, s_ctx.bytes, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_replace(&s_ctx.file, NULL, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_file_replace(&s_ctx.file, s_ctx.output, sizeof(s_ctx.bytes) + 1u));
    expect_bytes("abcdef", 6u);
}
void test_ram_bindings_preserve_initial_bytes_and_detach_without_saving_or_erasing(void) {
    memcpy(s_ctx.bytes, "initial", 7u);
    const esy_file_ram_static_resources_t resources = {s_ctx.bytes, sizeof(s_ctx.bytes)};
    esy_file_ram_config_t config = {&resources, true, 7u};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_init(&s_ctx.ram, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_ram_init(&s_ctx.ram, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_get_ops(&s_ctx.ram, &s_ctx.ops));
    bind_facade();
    expect_bytes("initial", 7u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_deinit(&s_ctx.ram));
    TEST_ASSERT_EQUAL_MEMORY("initial", s_ctx.bytes, 7u);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.ops.info(s_ctx.ops.context, &s_ctx.snapshot));
    config.exists = false;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_ram_init(&s_ctx.ram, &config));
    config.exists = true;
    config.initial_length = sizeof(s_ctx.bytes) + 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_file_ram_init(&s_ctx.ram, &config));
}
void test_flash_init_and_capability_query_do_not_read_or_mutate_reserved_media(void) {
    bind_flash();
    uint32_t caps;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_capabilities(&s_ctx.file, &caps));
    TEST_ASSERT_EQUAL_UINT32(ESY_FILE_CAP_READ | ESY_FILE_CAP_CREATE | ESY_FILE_CAP_REMOVE, caps);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.hal->read_count + s_ctx.hal->write_count + s_ctx.hal->erase_count);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_replace(&s_ctx.file, NULL, 0u));
}
void test_flash_complete_create_has_exact_range_reads_padding_and_single_region_removal(void) {
    bind_flash();
    const uint8_t data[] = {1u, 2u, 3u, 4u, 5u};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, data, sizeof(data)));
    expect_bytes(data, sizeof(data));
    TEST_ASSERT_EQUAL_HEX8(UINT8_MAX, s_ctx.media[sizeof(data)]);
    const uint32_t erases = s_ctx.hal->erase_count;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_file_create(&s_ctx.file, "other", 5u));
    TEST_ASSERT_EQUAL_UINT32(erases, s_ctx.hal->erase_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
    for (size_t i = 0u; i < sizeof(s_ctx.media); ++i) TEST_ASSERT_EQUAL_HEX8(UINT8_MAX, s_ctx.media[i]);
    TEST_ASSERT_FALSE(info().exists);
}
void test_flash_empty_payload_is_a_present_file_with_zero_length(void) {
    bind_flash();
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, NULL, 0u));
    expect_bytes(NULL, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
}
void test_flash_errors_and_corrupt_committed_payload_fail_closed_without_erasing(void) {
    bind_flash();
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, "record", 6u));
    const uint32_t erases = s_ctx.hal->erase_count;
    s_ctx.media[0] ^= 1u;
    esy_file_info_t output = {.length = 0xA5u};
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_file_get_info(&s_ctx.file, &output));
    TEST_ASSERT_EQUAL_HEX32(0xA5u, output.length);
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_file_remove(&s_ctx.file));
    TEST_ASSERT_EQUAL_UINT32(erases, s_ctx.hal->erase_count);
    s_ctx.media[0] ^= 1u;
    s_ctx.hal->read_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_file_remove(&s_ctx.file));
    TEST_ASSERT_EQUAL_UINT32(erases, s_ctx.hal->erase_count);
}
static uint32_t fixture_crc(const uint8_t *bytes, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0u; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}
static void fixture_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0u; i < 4u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}
void test_flash_recognizable_unknown_committed_format_is_not_reclaimed(void) {
    bind_flash();
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, "x", 1u));
    uint8_t *footer = s_ctx.media + TEST_REGION_BYTES - TEST_FOOTER_BYTES - TEST_PROGRAM_BYTES;
    fixture_u32(footer + 4u, 0xffu);
    fixture_u32(footer + 16u, fixture_crc(footer, 16u));
    const uint32_t erases = s_ctx.hal->erase_count;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_remove(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_create(&s_ctx.file, NULL, 0u));
    TEST_ASSERT_EQUAL_UINT32(erases, s_ctx.hal->erase_count);
}
void test_flash_each_interrupted_program_exposes_only_absence_or_the_complete_file_after_reopen(void) {
    uint8_t data[37];
    for (size_t i = 0u; i < sizeof(data); ++i) data[i] = (uint8_t)(i + 1u);
    const uint32_t units[] = {1u, 4u, 16u, 64u, 256u};
    for (size_t unit_index = 0u; unit_index < sizeof(units) / sizeof(units[0]); ++unit_index) {
      const uint32_t unit = units[unit_index];
      memset(s_ctx.media, UINT8_MAX, sizeof(s_ctx.media));
      memset(s_ctx.marks, 0, sizeof(s_ctx.marks));
      bind_flash_unit(unit);
      const uint32_t before = s_ctx.hal->write_count;
      TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, data, sizeof(data)));
      const uint32_t writes = s_ctx.hal->write_count - before;
      TEST_ASSERT_NOT_EQUAL(0u, writes);
      TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
      TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_deinit(&s_ctx.flash));
      const size_t effects[] = {0u, unit > 1u ? unit / 2u : 1u, SIZE_MAX};
      for (uint32_t call = 1u; call <= writes; ++call) {
        for (size_t effect = 0u; effect < sizeof(effects) / sizeof(effects[0]); ++effect) {
            memset(s_ctx.media, UINT8_MAX, sizeof(s_ctx.media));
            memset(s_ctx.marks, 0, sizeof(s_ctx.marks));
            bind_flash_unit(unit);
            s_ctx.hal->fail_write_call = s_ctx.hal->write_count + call;
            s_ctx.hal->write_fail_bytes = effects[effect];
            TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_file_create(&s_ctx.file, data, sizeof(data)));
            s_ctx.hal->fail_write_call = 0u;
            TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
            TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_deinit(&s_ctx.flash));
            bind_flash_unit(unit);
            if (!info().exists) {
                TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, data, sizeof(data)));
            }
            expect_bytes(data, sizeof(data));
            TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
            TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_deinit(&s_ctx.flash));
        }
      }
    }
}
void test_flash_invalid_geometry_storage_and_aliases_are_rejected_before_mutation(void) {
    esy_file_flash_static_resources_t resources;
    esy_file_flash_config_t config = flash_config(&resources);
    resources.program_capacity = TEST_PROGRAM_BYTES - 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_file_flash_init(&s_ctx.flash, &config));
    resources.program_capacity = sizeof(s_ctx.program);
    ++config.address;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_flash_init(&s_ctx.flash, &config));
    config.address = TEST_BASE;
    --config.size;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_flash_init(&s_ctx.flash, &config));
    config.size = TEST_REGION_BYTES;
    s_ctx.hal->program_size = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_flash_init(&s_ctx.flash, &config));
    s_ctx.hal->program_size = TEST_PROGRAM_BYTES;
    bind_flash();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_create(&s_ctx.file, s_ctx.program, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.hal->erase_count + s_ctx.hal->write_count);
}
void test_dual_flash_store_adapter_reuses_atomic_replacement_and_logical_tombstones(void) {
    bind_record();
    TEST_ASSERT_EQUAL_UINT32(TEST_ALL_CAPS, info().capabilities);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, "original", 8u));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_file_create(&s_ctx.file, "wrong", 5u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, "replacement", 11u));
    expect_bytes("replacement", 11u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_read(&s_ctx.file, 4u, s_ctx.output, 3u));
    TEST_ASSERT_EQUAL_MEMORY("ace", s_ctx.output, 3u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
    TEST_ASSERT_FALSE(info().exists);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_deinit(&s_ctx.adapter));
    esy_dual_flash_store_info_t record;
    TEST_ASSERT_EQUAL(ESY_OK, esy_dual_flash_store_get_info(&s_ctx.record, &record));
    TEST_ASSERT_FALSE(record.has_record);
    TEST_ASSERT_NOT_EQUAL(0u, record.generation);
}
void test_dual_flash_store_failed_replacement_preserves_the_old_complete_file(void) {
    bind_record();
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, "old", 3u));
    s_ctx.hal->fail_write_call = s_ctx.hal->write_count + 2u;
    s_ctx.hal->write_fail_bytes = 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_file_replace(&s_ctx.file, "new-value", 9u));
    s_ctx.hal->fail_write_call = 0u;
    expect_bytes("old", 3u);
}

void test_facade_overrides_provider_capabilities_and_propagates_read_failures_without_retries(void) {
    bind_source();
    s_ctx.snapshot = (esy_file_info_t){.exists = true, .length = 1u, .capacity = 1u,
        .capabilities = UINT32_MAX};
    TEST_ASSERT_EQUAL_UINT32(ESY_FILE_CAP_READ, info().capabilities);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_read(&s_ctx.file, 0u, s_ctx.output, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.read_calls);
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_file_read(&s_ctx.file, 0u, s_ctx.output, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.read_calls);
    TEST_ASSERT_FALSE(s_ctx.file.busy);
}

void test_ram_rejects_overlapping_initial_storage_and_raw_callback_outputs(void) {
    const esy_file_ram_static_resources_t bad = {(uint8_t *)(void *)&s_ctx.ram, sizeof(s_ctx.ram)};
    esy_file_ram_config_t config = {&bad, false, 0u};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_ram_init(&s_ctx.ram, &config));
    bind_ram();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, s_ctx.ops.info(s_ctx.ops.context,
        (esy_file_info_t *)(void *)s_ctx.bytes));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_ram_get_ops(&s_ctx.ram,
        (esy_file_ops_t *)(void *)s_ctx.bytes));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, s_ctx.ops.read(s_ctx.ops.context, 0u, NULL, 1u));
}

void test_flash_payload_verification_failure_never_publishes_a_commit(void) {
    bind_flash();
    s_ctx.hal->fail_read_call = s_ctx.hal->read_count + 3u;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_file_create(&s_ctx.file, "record", 6u));
    s_ctx.hal->fail_read_call = 0u;
    TEST_ASSERT_FALSE(info().exists);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.hal->write_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, "record", 6u));
    expect_bytes("record", 6u);
}

void test_flash_remove_readback_error_is_reported_even_when_the_file_is_already_gone(void) {
    bind_flash();
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, "record", 6u));
    s_ctx.hal->fail_read_call = s_ctx.hal->read_count + 4u;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.ops.remove(s_ctx.ops.context));
    s_ctx.hal->fail_read_call = 0u;
    TEST_ASSERT_FALSE(info().exists);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
}

void test_flash_program_units_larger_than_the_footer_do_not_overlap_payload_or_neighbours(void) {
    s_ctx.hal->program_size = 64u;
    esy_file_flash_static_resources_t resources = {s_ctx.output, sizeof(s_ctx.output)};
    const esy_file_flash_config_t config = {TEST_BASE, TEST_REGION_BYTES, &resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_init(&s_ctx.flash, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_flash_get_ops(&s_ctx.flash, &s_ctx.ops));
    bind_facade();
    for (size_t i = 0u; i < 65u; ++i) s_ctx.bytes[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL_UINT32(TEST_REGION_BYTES - 128u, info().capacity);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, s_ctx.bytes, 65u));
    uint8_t output[65];
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_read(&s_ctx.file, 0u, output, sizeof(output)));
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.bytes, output, sizeof(output));
    for (size_t i = TEST_REGION_BYTES; i < sizeof(s_ctx.media); ++i)
        TEST_ASSERT_EQUAL_HEX8(UINT8_MAX, s_ctx.media[i]);
}

void test_flash_committed_metadata_corruption_never_turns_into_absence_or_reclaimable_storage(void) {
    bind_flash();
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, "record", 6u));
    uint8_t *metadata = s_ctx.media + TEST_REGION_BYTES - TEST_FOOTER_BYTES - TEST_PROGRAM_BYTES;
    const uint32_t erases = s_ctx.hal->erase_count;
    metadata[16] ^= 1u;
    memcpy(s_ctx.saved, s_ctx.media, sizeof(s_ctx.saved));
    esy_file_info_t output = {.length = 0xA5u};
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_file_get_info(&s_ctx.file, &output));
    TEST_ASSERT_EQUAL_HEX32(0xA5u, output.length);
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_file_create(&s_ctx.file, "new", 3u));
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_file_remove(&s_ctx.file));
    TEST_ASSERT_EQUAL_UINT32(erases, s_ctx.hal->erase_count);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.media, sizeof(s_ctx.saved));
}
