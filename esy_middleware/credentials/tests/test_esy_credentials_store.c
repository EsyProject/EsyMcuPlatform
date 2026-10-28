/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_credentials.h"
#include "esy_credentials_store.h"
#include "esy_middleware/file/esy_file_ram.h"
#include "esy_middleware/file/esy_file_dual_flash_store.h"
#include "fake_esy_hal_flash.h"

#include <limits.h>
#include <string.h>

#define TEST_BUFFER_BYTES 1024u
#define TEST_FILE_BYTES 2048u
#define TEST_FLASH_BASE UINT32_C(0x10000)
#define TEST_FLASH_BYTES 12288u
#define TEST_REGION_BYTES 4096u
#define TEST_REGION_OFFSET 1024u
#define TEST_PROGRAM_BYTES 16u
#define TEST_WIRE_HEADER_BYTES 96u
#define TEST_WIRE_CRC_BYTES 4u

static const uint8_t k_first_ca[] = {0x30u, 0x01u, 0x11u};
static const uint8_t k_first_certificate[] = {0x30u, 0x02u, 0x22u, 0x23u};
static const uint8_t k_first_key[] = {0x30u, 0x03u, 0x33u, 0x34u, 0x35u};
static const uint8_t k_second_ca[] = "test candidate CA";
static const uint8_t k_second_certificate[] = "test candidate certificate";
static const uint8_t k_second_key[] = "test candidate software key bytes";

typedef struct {
    esy_credentials_store_t store;
    esy_credentials_store_config_t config;
    esy_credentials_store_static_resources_t resources;
    esy_file_t file;
    esy_file_ops_t inner_ops;
    esy_file_ram_t ram;
    esy_file_dual_flash_store_t adapter;
    esy_dual_flash_store_t records;
    esy_dual_flash_store_config_t record_config;
    esy_dual_flash_store_static_resources_t record_resources;
    fake_esy_hal_flash_state_t *flash;
    esy_err_t info_error;
    esy_err_t read_error;
    esy_err_t replace_error;
    bool commit_before_error;
    bool reenter;
    unsigned reentry_checks;
    unsigned replaces;
    uint8_t working[TEST_BUFFER_BYTES];
    uint8_t scratch[TEST_BUFFER_BYTES];
    uint8_t ram_bytes[TEST_FILE_BYTES];
    uint8_t loaded[TEST_BUFFER_BYTES];
    uint8_t retained[TEST_BUFFER_BYTES];
    uint8_t saved_working[TEST_BUFFER_BYTES];
    uint8_t media[TEST_FLASH_BYTES];
    uint8_t saved_media[TEST_FLASH_BYTES];
    uint8_t record_work[TEST_PROGRAM_BYTES];
} store_test_ctx_t;

static store_test_ctx_t s_ctx;

static void wire_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0u; i < 4u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}

static void wire_u64(uint8_t *bytes, uint64_t value) {
    for (unsigned i = 0u; i < 8u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}

static void wire_crc(uint8_t *bytes, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0u; i < length - TEST_WIRE_CRC_BYTES; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ ((crc & 1u) != 0u ? UINT32_C(0xedb88320) : 0u);
    }
    wire_u32(bytes + length - TEST_WIRE_CRC_BYTES, ~crc);
}

/* Independent published-format fixture; does not include private store state. */
static size_t wire_snapshot(uint64_t transaction, uint64_t last_issued,
                             uint64_t confirmed_id, uint64_t candidate_id) {
    memset(s_ctx.loaded, 0, sizeof(s_ctx.loaded));
    wire_u32(s_ctx.loaded, UINT32_C(0x53524345));
    wire_u32(s_ctx.loaded + 4u, 1u);
    wire_u64(s_ctx.loaded + 16u, transaction);
    wire_u64(s_ctx.loaded + 24u, last_issued);
    wire_u64(s_ctx.loaded + 32u, confirmed_id);
    wire_u64(s_ctx.loaded + 40u, candidate_id);
    esy_credentials_config_t confirmed = {
        {k_first_ca, sizeof(k_first_ca), ESY_CREDENTIALS_FORMAT_DER},
        {k_first_certificate, sizeof(k_first_certificate), ESY_CREDENTIALS_FORMAT_DER},
        {k_first_key, sizeof(k_first_key), ESY_CREDENTIALS_FORMAT_DER}};
    esy_credentials_config_t candidate = {
        {k_second_ca, sizeof(k_second_ca), ESY_CREDENTIALS_FORMAT_PEM},
        {k_second_certificate, sizeof(k_second_certificate), ESY_CREDENTIALS_FORMAT_PEM},
        {k_second_key, sizeof(k_second_key), ESY_CREDENTIALS_FORMAT_PEM}};
    const esy_credentials_config_t bundles[] = {confirmed, candidate};
    const uint64_t ids[] = {confirmed_id, candidate_id};
    size_t offset = TEST_WIRE_HEADER_BYTES;
    for (unsigned slot = 0u; slot < 2u; ++slot) {
        if (ids[slot] == 0u) continue;
        const esy_credentials_blob_t blobs[] = {
            bundles[slot].trust_anchors, bundles[slot].client_certificate,
            bundles[slot].private_key};
        for (unsigned field = 0u; field < 3u; ++field) {
            size_t descriptor = 48u + slot * 24u + field * 8u;
            wire_u32(s_ctx.loaded + descriptor, (uint32_t)blobs[field].size);
            wire_u32(s_ctx.loaded + descriptor + 4u, (uint32_t)blobs[field].format);
            memcpy(s_ctx.loaded + offset, blobs[field].data, blobs[field].size);
            offset += blobs[field].size;
        }
    }
    size_t length = offset + TEST_WIRE_CRC_BYTES;
    wire_u32(s_ctx.loaded + 8u, (uint32_t)length);
    wire_crc(s_ctx.loaded, length);
    return length;
}

static esy_credentials_config_t first_bundle(void) {
    return (esy_credentials_config_t){
        {k_first_ca, sizeof(k_first_ca), ESY_CREDENTIALS_FORMAT_DER},
        {k_first_certificate, sizeof(k_first_certificate), ESY_CREDENTIALS_FORMAT_DER},
        {k_first_key, sizeof(k_first_key), ESY_CREDENTIALS_FORMAT_DER}};
}

static esy_credentials_config_t second_bundle(void) {
    return (esy_credentials_config_t){
        {k_second_ca, sizeof(k_second_ca), ESY_CREDENTIALS_FORMAT_PEM},
        {k_second_certificate, sizeof(k_second_certificate), ESY_CREDENTIALS_FORMAT_PEM},
        {k_second_key, sizeof(k_second_key), ESY_CREDENTIALS_FORMAT_PEM}};
}

static void exercise_reentry(store_test_ctx_t *ctx) {
    if (!ctx->reenter) return;
    esy_credentials_store_info_t info;
    esy_credentials_config_t bundle = first_bundle(), output = {0};
    uint64_t id = 0u;
    const esy_credentials_store_load_resources_t resources = {
        ctx->loaded, sizeof(ctx->loaded)};
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_store_init(&ctx->store, &ctx->config));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_store_get_info(&ctx->store, &info));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_store_load(&ctx->store,
        ESY_CREDENTIALS_STORE_CONFIRMED, &resources, &output, &id));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_store_stage(&ctx->store, &bundle, &id));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_store_confirm(&ctx->store, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_store_discard(&ctx->store, 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_store_reload(&ctx->store));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_store_deinit(&ctx->store));
    ++ctx->reentry_checks;
}

static esy_err_t file_info(void *context, esy_file_info_t *info) {
    store_test_ctx_t *ctx = context;
    exercise_reentry(ctx);
    if (ctx->info_error != ESY_OK) return ctx->info_error;
    return ctx->inner_ops.info(ctx->inner_ops.context, info);
}

static esy_err_t file_read(void *context, uint32_t offset, void *buffer, size_t length) {
    store_test_ctx_t *ctx = context;
    exercise_reentry(ctx);
    if (ctx->read_error != ESY_OK) return ctx->read_error;
    return ctx->inner_ops.read(ctx->inner_ops.context, offset, buffer, length);
}

static esy_err_t file_replace(void *context, const void *bytes, size_t length) {
    store_test_ctx_t *ctx = context;
    exercise_reentry(ctx);
    ++ctx->replaces;
    if (ctx->replace_error != ESY_OK && !ctx->commit_before_error)
        return ctx->replace_error;
    esy_err_t result = ctx->inner_ops.replace(ctx->inner_ops.context, bytes, length);
    return result == ESY_OK ? ctx->replace_error : result;
}

static void bind_file(void) {
    const esy_file_ops_t ops = {
        .info = file_info, .read = file_read, .replace = file_replace,
        .context = &s_ctx};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_init(&s_ctx.file, &ops));
}

static void detach_file(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_deinit(&s_ctx.ram));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.records));
}

static void attach_ram(void) {
    const esy_file_ram_static_resources_t resources = {
        s_ctx.ram_bytes, sizeof(s_ctx.ram_bytes)};
    const esy_file_ram_config_t config = {&resources, false, 0u};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_init(&s_ctx.ram, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_get_ops(&s_ctx.ram, &s_ctx.inner_ops));
    bind_file();
}

static void attach_flash(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_dual_flash_store_init(&s_ctx.records, &s_ctx.record_config));
    const esy_file_dual_flash_store_config_t config = {&s_ctx.records};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_init(&s_ctx.adapter, &config));
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_file_dual_flash_store_get_ops(&s_ctx.adapter, &s_ctx.inner_ops));
    bind_file();
}

static void initialize(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_init(&s_ctx.store, &s_ctx.config));
}

static esy_credentials_store_info_t information(void) {
    esy_credentials_store_info_t info = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_get_info(&s_ctx.store, &info));
    return info;
}

static uint64_t stage(const esy_credentials_config_t *bundle) {
    uint64_t id = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_stage(&s_ctx.store, bundle, &id));
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, id);
    return id;
}

static uint64_t provision(void) {
    esy_credentials_config_t bundle = first_bundle();
    uint64_t id = stage(&bundle);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_confirm(&s_ctx.store, id));
    return id;
}

static void compare_blob(esy_credentials_blob_t expected, esy_credentials_blob_t actual) {
    TEST_ASSERT_EQUAL(expected.format, actual.format);
    TEST_ASSERT_EQUAL_UINT(expected.size, actual.size);
    if (expected.size != 0u) {
        TEST_ASSERT_EQUAL_MEMORY(expected.data, actual.data, expected.size);
        TEST_ASSERT_TRUE((uintptr_t)actual.data >= (uintptr_t)s_ctx.loaded);
        TEST_ASSERT_TRUE((uintptr_t)actual.data + actual.size <=
                         (uintptr_t)s_ctx.loaded + sizeof(s_ctx.loaded));
    }
}

static void expect_loaded(esy_credentials_store_slot_t slot, uint64_t expected_id,
                           esy_credentials_config_t expected) {
    esy_credentials_config_t actual = {0};
    uint64_t id = UINT64_MAX;
    memset(s_ctx.loaded, 0xa5, sizeof(s_ctx.loaded));
    const esy_credentials_store_load_resources_t resources = {
        s_ctx.loaded, sizeof(s_ctx.loaded)};
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_credentials_store_load(&s_ctx.store, slot, &resources, &actual, &id));
    TEST_ASSERT_EQUAL_UINT64(expected_id, id);
    compare_blob(expected.trust_anchors, actual.trust_anchors);
    compare_blob(expected.client_certificate, actual.client_certificate);
    compare_blob(expected.private_key, actual.private_key);
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.resources = (esy_credentials_store_static_resources_t){
        s_ctx.working, s_ctx.scratch, sizeof(s_ctx.working)};
    s_ctx.config = (esy_credentials_store_config_t){&s_ctx.file, &s_ctx.resources};
    fake_esy_hal_flash_reset();
    s_ctx.flash = fake_esy_hal_flash_get_state();
    s_ctx.flash->storage_base_addr = TEST_FLASH_BASE;
    s_ctx.flash->size_bytes = sizeof(s_ctx.media);
    s_ctx.flash->sector_size = 256u;
    s_ctx.flash->program_size = TEST_PROGRAM_BYTES;
    s_ctx.flash->backing_storage = s_ctx.media;
    s_ctx.flash->backing_storage_size = sizeof(s_ctx.media);
    memset(s_ctx.media, 0xff, sizeof(s_ctx.media));
    s_ctx.record_resources = (esy_dual_flash_store_static_resources_t){
        s_ctx.record_work, sizeof(s_ctx.record_work)};
    s_ctx.record_config.static_resources = &s_ctx.record_resources;
    s_ctx.record_config.regions[0] = (esy_dual_flash_store_region_t){
        TEST_FLASH_BASE + TEST_REGION_OFFSET, TEST_REGION_BYTES};
    s_ctx.record_config.regions[1] = (esy_dual_flash_store_region_t){
        TEST_FLASH_BASE + TEST_REGION_OFFSET + TEST_REGION_BYTES, TEST_REGION_BYTES};
    attach_ram();
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    detach_file();
}

void test_store_first_provisioning_is_explicit_and_survives_reinitialization(void) {
    initialize();
    esy_credentials_store_info_t empty = information();
    TEST_ASSERT_FALSE(empty.has_snapshot);
    TEST_ASSERT_EQUAL_UINT64(0u, empty.transaction_id);
    TEST_ASSERT_EQUAL_UINT64(0u, empty.last_issued_id);
    TEST_ASSERT_EQUAL_UINT64(0u, empty.confirmed_id);
    TEST_ASSERT_EQUAL_UINT64(0u, empty.candidate_id);
    TEST_ASSERT_EQUAL_UINT(0u, empty.used_bytes);
    TEST_ASSERT_EQUAL_UINT(TEST_BUFFER_BYTES, empty.capacity);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.replaces);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_credentials_store_init(&s_ctx.store, &s_ctx.config));
    uint64_t id = provision();
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, id, first_bundle());
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    initialize();
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, id, first_bundle());
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.replaces);
}

void test_store_null_invalid_resources_and_read_only_backend_fail_without_writes(void) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_init(&s_ctx.store, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_deinit(NULL));
    esy_credentials_store_config_t config = s_ctx.config;
    config.file = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_init(&s_ctx.store, &config));
    config = s_ctx.config;
    config.static_resources = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_init(&s_ctx.store, &config));
    config = s_ctx.config;
    esy_credentials_store_static_resources_t resources = s_ctx.resources;
    config.static_resources = &resources;
    resources.scratch_buffer = resources.working_buffer;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_init(&s_ctx.store, &config));
    resources = s_ctx.resources;
    resources.working_buffer = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_init(&s_ctx.store, &config));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.replaces);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    const esy_file_ops_t read_only = {.info = file_info, .read = file_read, .context = &s_ctx};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_init(&s_ctx.file, &read_only));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED,
        esy_credentials_store_init(&s_ctx.store, &s_ctx.config));
}

void test_store_provider_query_and_initial_read_errors_are_not_treated_as_absence(void) {
    s_ctx.info_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_credentials_store_init(&s_ctx.store, &s_ctx.config));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.replaces);
    s_ctx.info_error = ESY_OK;
    size_t length = wire_snapshot(2u, 1u, 1u, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
    s_ctx.read_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_credentials_store_init(&s_ctx.store, &s_ctx.config));
    s_ctx.read_error = ESY_OK;
    initialize();
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, 1u, first_bundle());
}

void test_store_published_wire_format_loads_complete_bundles_and_does_not_auto_confirm(void) {
    size_t length = wire_snapshot(3u, 2u, 1u, 2u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
    initialize();
    esy_credentials_store_info_t info = information();
    TEST_ASSERT_EQUAL(ESY_CREDENTIALS_STORE_READY, info.state);
    TEST_ASSERT_TRUE(info.has_snapshot);
    TEST_ASSERT_EQUAL_UINT64(3u, info.transaction_id);
    TEST_ASSERT_EQUAL_UINT64(2u, info.last_issued_id);
    TEST_ASSERT_EQUAL_UINT64(1u, info.confirmed_id);
    TEST_ASSERT_EQUAL_UINT64(2u, info.candidate_id);
    TEST_ASSERT_EQUAL_UINT(sizeof(k_first_ca) + sizeof(k_first_certificate) + sizeof(k_first_key),
                            info.confirmed_size);
    TEST_ASSERT_EQUAL_UINT(sizeof(k_second_ca) + sizeof(k_second_certificate) + sizeof(k_second_key),
                            info.candidate_size);
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, 1u, first_bundle());
    expect_loaded(ESY_CREDENTIALS_STORE_CANDIDATE, 2u, second_bundle());
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.replaces);
}

void test_store_bootstrap_candidate_can_be_discarded_without_losing_identifier_history(void) {
    initialize();
    esy_credentials_config_t bundle = first_bundle();
    uint64_t id = stage(&bundle);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_discard(&s_ctx.store, id));
    esy_credentials_store_info_t info = information();
    TEST_ASSERT_TRUE(info.has_snapshot);
    TEST_ASSERT_EQUAL_UINT64(0u, info.confirmed_id);
    TEST_ASSERT_EQUAL_UINT64(0u, info.candidate_id);
    TEST_ASSERT_EQUAL_UINT64(id, info.last_issued_id);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    TEST_ASSERT_TRUE(stage(&bundle) > id);
}

void test_store_optional_client_pair_and_independent_capacity_are_preserved(void) {
    initialize();
    esy_credentials_config_t bundle = first_bundle();
    bundle.client_certificate = (esy_credentials_blob_t){0};
    bundle.private_key = (esy_credentials_blob_t){0};
    uint64_t id = stage(&bundle);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_confirm(&s_ctx.store, id));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, id, bundle);
    TEST_ASSERT_EQUAL_UINT(sizeof(k_first_ca), information().confirmed_size);
    esy_credentials_store_t copy = s_ctx.store;
    esy_credentials_store_info_t info;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_get_info(&copy, &info));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_reload(&copy));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_deinit(&copy));
}

void test_store_candidate_is_complete_separate_and_requires_matching_confirmation(void) {
    initialize();
    uint64_t confirmed = provision();
    esy_credentials_config_t bundle = second_bundle();
    uint64_t candidate = stage(&bundle);
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, confirmed, first_bundle());
    expect_loaded(ESY_CREDENTIALS_STORE_CANDIDATE, candidate, bundle);
    uint64_t unchanged = UINT64_MAX;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY,
        esy_credentials_store_stage(&s_ctx.store, &bundle, &unchanged));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, unchanged);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_confirm(&s_ctx.store, confirmed));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_discard(&s_ctx.store, confirmed));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    initialize();
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, confirmed, first_bundle());
    expect_loaded(ESY_CREDENTIALS_STORE_CANDIDATE, candidate, bundle);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_confirm(&s_ctx.store, candidate));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, candidate, bundle);
}

void test_store_discard_preserves_confirmed_and_never_reuses_candidate_identity(void) {
    initialize();
    uint64_t confirmed = provision();
    esy_credentials_config_t bundle = second_bundle();
    uint64_t discarded = stage(&bundle);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_discard(&s_ctx.store, discarded));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, confirmed, first_bundle());
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_confirm(&s_ctx.store, discarded));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_discard(&s_ctx.store, discarded));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    initialize();
    TEST_ASSERT_TRUE(stage(&bundle) > discarded);
}

void test_store_loaded_credentials_keep_their_lease_bytes_during_rotation_and_reload(void) {
    initialize();
    uint64_t confirmed = provision();
    esy_credentials_config_t loaded = {0}, borrowed = {0};
    uint64_t loaded_id = 0u;
    const esy_credentials_store_load_resources_t resources = {
        s_ctx.retained, sizeof(s_ctx.retained)};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_load(&s_ctx.store,
        ESY_CREDENTIALS_STORE_CONFIRMED, &resources, &loaded, &loaded_id));
    TEST_ASSERT_EQUAL_UINT64(confirmed, loaded_id);
    esy_credentials_t credentials = {0};
    esy_credentials_lease_t lease = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_init(&credentials, &loaded));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_acquire(&credentials, &lease, &borrowed));
    esy_credentials_config_t bundle = second_bundle();
    uint64_t candidate = stage(&bundle);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_confirm(&s_ctx.store, candidate));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    TEST_ASSERT_EQUAL_MEMORY(k_first_ca, borrowed.trust_anchors.data, sizeof(k_first_ca));
    TEST_ASSERT_EQUAL_MEMORY(k_first_certificate, borrowed.client_certificate.data,
                             sizeof(k_first_certificate));
    TEST_ASSERT_EQUAL_MEMORY(k_first_key, borrowed.private_key.data, sizeof(k_first_key));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_credentials_replace(&credentials, &bundle));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_release(&lease));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_deinit(&credentials));
}

void test_store_invalid_candidate_and_capacity_errors_do_not_publish_or_write(void) {
    initialize();
    uint64_t confirmed = provision();
    unsigned replaces = s_ctx.replaces;
    uint64_t id = UINT64_MAX;
    esy_credentials_config_t invalid = first_bundle();
    invalid.private_key = (esy_credentials_blob_t){0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_credentials_store_stage(&s_ctx.store, &invalid, &id));
    static const uint8_t k_invalid_pem[] = {'a', 0u, 'b', 0u};
    invalid = first_bundle();
    invalid.trust_anchors = (esy_credentials_blob_t){
        k_invalid_pem, sizeof(k_invalid_pem), ESY_CREDENTIALS_FORMAT_PEM};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_credentials_store_stage(&s_ctx.store, &invalid, &id));
    invalid = first_bundle();
    invalid.trust_anchors.format = (esy_credentials_format_t)99;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_credentials_store_stage(&s_ctx.store, &invalid, &id));
    uint8_t oversized[TEST_BUFFER_BYTES];
    memset(oversized, 0x30, sizeof(oversized));
    invalid = (esy_credentials_config_t){
        {oversized, sizeof(oversized), ESY_CREDENTIALS_FORMAT_DER}, {0}, {0}};
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,
        esy_credentials_store_stage(&s_ctx.store, &invalid, &id));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, id);
    TEST_ASSERT_EQUAL_UINT(replaces, s_ctx.replaces);
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, confirmed, first_bundle());
}

void test_store_load_validates_outputs_and_preserves_them_on_absence_or_short_capacity(void) {
    initialize();
    esy_credentials_config_t actual;
    memset(&actual, 0xa5, sizeof(actual));
    esy_credentials_config_t unchanged = actual;
    uint64_t id = UINT64_MAX;
    uint8_t short_buffer[] = {0xa5u};
    const esy_credentials_store_load_resources_t short_resources = {
        short_buffer, sizeof(short_buffer)};
    const esy_credentials_store_load_resources_t resources = {
        s_ctx.loaded, sizeof(s_ctx.loaded)};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_load(&s_ctx.store,
        ESY_CREDENTIALS_STORE_CONFIRMED, &resources, &actual, &id));
    (void)provision();
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_credentials_store_load(&s_ctx.store,
        ESY_CREDENTIALS_STORE_CONFIRMED, &short_resources, &actual, &id));
    TEST_ASSERT_EQUAL_MEMORY(&unchanged, &actual, sizeof(actual));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, id);
    TEST_ASSERT_EQUAL_HEX8(0xa5u, short_buffer[0]);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_load(&s_ctx.store,
        (esy_credentials_store_slot_t)99, &resources, &actual, &id));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_load(&s_ctx.store,
        ESY_CREDENTIALS_STORE_CONFIRMED, NULL, &actual, &id));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_load(&s_ctx.store,
        ESY_CREDENTIALS_STORE_CONFIRMED, &resources, NULL, &id));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_load(&s_ctx.store,
        ESY_CREDENTIALS_STORE_CONFIRMED, &resources, &actual, NULL));
}

void test_store_failed_stage_before_commit_requires_reconciliation_and_preserves_old_bundle(void) {
    initialize();
    uint64_t confirmed = provision();
    esy_credentials_config_t bundle = second_bundle();
    uint64_t id = UINT64_MAX;
    s_ctx.replace_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO,
        esy_credentials_store_stage(&s_ctx.store, &bundle, &id));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, id);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_credentials_store_stage(&s_ctx.store, &bundle, &id));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_credentials_store_confirm(&s_ctx.store, confirmed));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_credentials_store_discard(&s_ctx.store, confirmed));
    const esy_credentials_store_load_resources_t resources = {
        s_ctx.loaded, sizeof(s_ctx.loaded)};
    esy_credentials_config_t loaded = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_load(&s_ctx.store,
        ESY_CREDENTIALS_STORE_CONFIRMED, &resources, &loaded, &id));
    s_ctx.replace_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, confirmed, first_bundle());
    uint64_t candidate = stage(&bundle);
    TEST_ASSERT_TRUE(candidate > confirmed);
}

void test_store_failed_stage_after_commit_reloads_the_persisted_candidate(void) {
    initialize();
    uint64_t confirmed = provision();
    esy_credentials_config_t bundle = second_bundle();
    uint64_t id = UINT64_MAX;
    s_ctx.replace_error = ESY_ERR_IO;
    s_ctx.commit_before_error = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO,
        esy_credentials_store_stage(&s_ctx.store, &bundle, &id));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, id);
    s_ctx.replace_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, confirmed, first_bundle());
    expect_loaded(ESY_CREDENTIALS_STORE_CANDIDATE, confirmed + 1u, bundle);
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_credentials_store_confirm(&s_ctx.store, confirmed + 1u));
}

void test_store_failed_confirmation_after_commit_reloads_new_confirmed_identity(void) {
    initialize();
    (void)provision();
    esy_credentials_config_t bundle = second_bundle();
    uint64_t id = stage(&bundle);
    s_ctx.replace_error = ESY_ERR_IO;
    s_ctx.commit_before_error = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_credentials_store_confirm(&s_ctx.store, id));
    s_ctx.replace_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, id, bundle);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_confirm(&s_ctx.store, id));
}

void test_store_failed_discard_after_commit_reloads_old_confirmed_without_candidate(void) {
    initialize();
    uint64_t confirmed = provision();
    esy_credentials_config_t bundle = second_bundle();
    uint64_t id = stage(&bundle);
    s_ctx.replace_error = ESY_ERR_IO;
    s_ctx.commit_before_error = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_credentials_store_discard(&s_ctx.store, id));
    s_ctx.replace_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, confirmed, first_bundle());
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_discard(&s_ctx.store, id));
}

void test_store_wire_corruption_and_unknown_versions_fail_closed_without_formatting(void) {
    for (unsigned scenario = 0u; scenario < 19u; ++scenario) {
        size_t length = wire_snapshot(3u, 2u, 1u, 2u);
        esy_err_t expected = ESY_ERR_CRC_FAIL;
        switch (scenario) {
            case 0u: wire_u32(s_ctx.loaded, UINT32_C(0x12345678)); expected = ESY_ERR_NOT_SUPPORTED; break;
            case 1u: wire_u32(s_ctx.loaded + 4u, 2u); expected = ESY_ERR_NOT_SUPPORTED; break;
            case 2u: wire_u32(s_ctx.loaded + 8u, (uint32_t)length + 1u); break;
            case 3u: wire_u32(s_ctx.loaded + 12u, 1u); break;
            case 4u: wire_u64(s_ctx.loaded + 16u, 0u); break;
            case 5u: wire_u64(s_ctx.loaded + 24u, 0u); break;
            case 6u: wire_u64(s_ctx.loaded + 24u, 4u); break;
            case 7u: wire_u64(s_ctx.loaded + 32u, 3u); break;
            case 8u: wire_u64(s_ctx.loaded + 40u, 1u); break;
            case 9u:
                wire_u64(s_ctx.loaded + 16u, 4u);
                wire_u64(s_ctx.loaded + 24u, 3u);
                break;
            case 10u: wire_u64(s_ctx.loaded + 32u, 0u); break;
            case 11u: wire_u32(s_ctx.loaded + 52u, 99u); break;
            case 12u: wire_u32(s_ctx.loaded + 48u, 0u); break;
            case 13u: wire_u32(s_ctx.loaded + 56u, 0u); break;
            case 14u: wire_u32(s_ctx.loaded + 48u, UINT32_MAX); break;
            case 15u: s_ctx.loaded[length - TEST_WIRE_CRC_BYTES - 1u] = 'x'; break;
            case 16u:
                s_ctx.loaded[TEST_WIRE_HEADER_BYTES + sizeof(k_first_ca) +
                             sizeof(k_first_certificate) + sizeof(k_first_key)] = 0u;
                break;
            case 17u:
                ++length;
                wire_u32(s_ctx.loaded + 8u, (uint32_t)length);
                break;
            default: break;
        }
        wire_crc(s_ctx.loaded, length);
        if (scenario == 18u) s_ctx.loaded[length - 1u] ^= 1u;
        TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
        unsigned writes = s_ctx.replaces;
        TEST_ASSERT_EQUAL(expected, esy_credentials_store_init(&s_ctx.store, &s_ctx.config));
        TEST_ASSERT_EQUAL_UINT(writes, s_ctx.replaces);
        TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    }
}

void test_store_exhausted_transaction_and_candidate_identifiers_never_wrap(void) {
    size_t length = wire_snapshot(UINT64_MAX, UINT64_MAX, UINT64_MAX, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
    initialize();
    esy_credentials_config_t bundle = second_bundle();
    uint64_t id = 7u;
    unsigned writes = s_ctx.replaces;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_credentials_store_stage(&s_ctx.store, &bundle, &id));
    TEST_ASSERT_EQUAL_UINT64(7u, id);
    TEST_ASSERT_EQUAL_UINT(writes, s_ctx.replaces);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    length = wire_snapshot(UINT64_MAX, 2u, 1u, 2u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
    initialize();
    writes = s_ctx.replaces;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_confirm(&s_ctx.store, 2u));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_discard(&s_ctx.store, 2u));
    TEST_ASSERT_EQUAL_UINT(writes, s_ctx.replaces);
}

void test_store_stage_reserves_the_final_transaction_for_confirmation_or_discard(void) {
    size_t length = wire_snapshot(UINT64_MAX - 1u, 1u, 1u, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
    initialize();
    esy_credentials_config_t bundle = second_bundle();
    uint64_t id = UINT64_MAX;
    unsigned writes = s_ctx.replaces;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_credentials_store_stage(&s_ctx.store, &bundle, &id));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, id);
    TEST_ASSERT_EQUAL_UINT(writes, s_ctx.replaces);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX - 1u, information().transaction_id);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));

    for (unsigned confirm = 0u; confirm < 2u; ++confirm) {
        length = wire_snapshot(UINT64_MAX - 2u, 1u, 1u, 0u);
        TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
        initialize();
        id = stage(&bundle);
        TEST_ASSERT_EQUAL_UINT64(2u, id);
        TEST_ASSERT_EQUAL_UINT64(UINT64_MAX - 1u, information().transaction_id);
        TEST_ASSERT_EQUAL(ESY_OK, confirm != 0u
            ? esy_credentials_store_confirm(&s_ctx.store, id)
            : esy_credentials_store_discard(&s_ctx.store, id));
        esy_credentials_store_info_t info = information();
        TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, info.transaction_id);
        TEST_ASSERT_EQUAL_UINT64(0u, info.candidate_id);
        expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED,
            confirm != 0u ? id : 1u, confirm != 0u ? bundle : first_bundle());
        writes = s_ctx.replaces;
        TEST_ASSERT_EQUAL(ESY_ERR_STATE,
            esy_credentials_store_stage(&s_ctx.store, &bundle, &id));
        TEST_ASSERT_EQUAL_UINT(writes, s_ctx.replaces);
        TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    }
}

void test_store_reload_failures_block_loaded_data_until_authoritative_state_is_read(void) {
    initialize();
    uint64_t confirmed = provision();
    s_ctx.read_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_credentials_store_reload(&s_ctx.store));
    esy_credentials_store_info_t info = information();
    TEST_ASSERT_EQUAL(ESY_CREDENTIALS_STORE_UNCERTAIN, info.state);
    TEST_ASSERT_EQUAL_UINT64(confirmed, info.confirmed_id);
    s_ctx.read_error = ESY_OK;
    s_ctx.info_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_credentials_store_reload(&s_ctx.store));
    s_ctx.info_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, confirmed, first_bundle());
}

void test_store_known_snapshot_cannot_silently_disappear_or_rewind_while_live(void) {
    initialize();
    (void)provision();
    size_t length = s_ctx.ram.length;
    memcpy(s_ctx.saved_working, s_ctx.ram_bytes, length);
    /* Model backend retention loss, without using an absent file as defaults. */
    s_ctx.ram.exists = false;
    s_ctx.ram.length = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_reload(&s_ctx.store));
    TEST_ASSERT_EQUAL(ESY_CREDENTIALS_STORE_UNCERTAIN, information().state);
    memcpy(s_ctx.ram_bytes, s_ctx.saved_working, length);
    s_ctx.ram.length = (uint32_t)length;
    s_ctx.ram.exists = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    esy_credentials_config_t bundle = second_bundle();
    (void)stage(&bundle);
    memcpy(s_ctx.ram_bytes, s_ctx.saved_working, length);
    s_ctx.ram.length = (uint32_t)length;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_reload(&s_ctx.store));
}

void test_store_workspace_and_output_aliases_are_rejected_without_modifying_live_snapshot(void) {
    initialize();
    (void)provision();
    esy_credentials_config_t bundle = second_bundle();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_credentials_store_stage(&s_ctx.store, &bundle, (uint64_t *)s_ctx.working));
    const esy_credentials_store_load_resources_t alias = {
        s_ctx.working, sizeof(s_ctx.working)};
    esy_credentials_config_t loaded;
    uint64_t id = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_credentials_store_load(&s_ctx.store,
        ESY_CREDENTIALS_STORE_CONFIRMED, &alias, &loaded, &id));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_credentials_store_get_info(&s_ctx.store, (esy_credentials_store_info_t *)s_ctx.scratch));
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, 1u, first_bundle());
}

void test_store_deinit_clears_private_workspaces_but_leaves_loaded_consumer_copy_intact(void) {
    initialize();
    (void)provision();
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, 1u, first_bundle());
    uint8_t saved[sizeof(k_first_ca) + sizeof(k_first_certificate) + sizeof(k_first_key)];
    memcpy(saved, s_ctx.loaded, sizeof(saved));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    TEST_ASSERT_EACH_EQUAL_HEX8(0u, s_ctx.working, sizeof(s_ctx.working));
    TEST_ASSERT_EACH_EQUAL_HEX8(0u, s_ctx.scratch, sizeof(s_ctx.scratch));
    TEST_ASSERT_EQUAL_MEMORY(saved, s_ctx.loaded, sizeof(saved));
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
}

void test_store_provider_callback_reentry_is_rejected_through_mutation_and_reload(void) {
    initialize();
    (void)provision();
    s_ctx.reenter = true;
    esy_credentials_config_t bundle = second_bundle();
    uint64_t id = stage(&bundle);
    TEST_ASSERT_TRUE(s_ctx.reentry_checks > 0u);
    unsigned checks = s_ctx.reentry_checks;
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_reload(&s_ctx.store));
    TEST_ASSERT_TRUE(s_ctx.reentry_checks > checks);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_confirm(&s_ctx.store, id));
    s_ctx.reenter = false;
}

void test_store_same_transaction_with_different_crc_valid_format_bytes_is_rejected(void) {
    size_t length = wire_snapshot(2u, 1u, 1u, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
    initialize();
    /* Simulate inconsistent retained bytes with a fresh valid CRC and same ID. */
    s_ctx.ram_bytes[TEST_WIRE_HEADER_BYTES] ^= 1u;
    wire_crc(s_ctx.ram_bytes, length);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_credentials_store_reload(&s_ctx.store));
    TEST_ASSERT_EQUAL(ESY_CREDENTIALS_STORE_UNCERTAIN, information().state);
}

void test_store_first_init_protects_handle_and_workspaces_from_provider_callback_reentry(void) {
    size_t length = wire_snapshot(2u, 1u, 1u, 0u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, s_ctx.loaded, length));
    s_ctx.reenter = true;
    initialize();
    TEST_ASSERT_TRUE(s_ctx.reentry_checks >= 2u);
    s_ctx.reenter = false;
    expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, 1u, first_bundle());
}

static void clear_flash_faults(void) {
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

static void prepare_flash_transaction(unsigned scenario) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    detach_file();
    memset(s_ctx.media, 0xff, sizeof(s_ctx.media));
    clear_flash_faults();
    attach_flash();
    initialize();
    if (scenario != 3u) (void)provision();
    if (scenario == 1u || scenario == 2u) {
        esy_credentials_config_t bundle = second_bundle();
        TEST_ASSERT_EQUAL_UINT64(2u, stage(&bundle));
    }
    memcpy(s_ctx.saved_media, s_ctx.media, sizeof(s_ctx.saved_media));
    clear_flash_faults();
}

static void restore_flash_transaction(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    detach_file();
    memcpy(s_ctx.media, s_ctx.saved_media, sizeof(s_ctx.media));
    clear_flash_faults();
    attach_flash();
    initialize();
    clear_flash_faults();
}

static esy_err_t perform_flash_transaction(unsigned scenario) {
    if (scenario == 1u) return esy_credentials_store_confirm(&s_ctx.store, 2u);
    if (scenario == 2u) return esy_credentials_store_discard(&s_ctx.store, 2u);
    esy_credentials_config_t bundle = scenario == 3u ? first_bundle() : second_bundle();
    uint64_t id = 0u;
    return esy_credentials_store_stage(&s_ctx.store, &bundle, &id);
}

static void expect_atomic_flash_recovery(unsigned scenario) {
    TEST_ASSERT_EQUAL(ESY_CREDENTIALS_STORE_UNCERTAIN, information().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_credentials_store_deinit(&s_ctx.store));
    detach_file();
    clear_flash_faults();
    attach_flash();
    initialize();
    esy_credentials_store_info_t info = information();
    TEST_ASSERT_EQUAL(ESY_CREDENTIALS_STORE_READY, info.state);
    if (scenario == 3u) {
        TEST_ASSERT_EQUAL_UINT64(0u, info.confirmed_id);
        TEST_ASSERT_TRUE(info.candidate_id == 0u || info.candidate_id == 1u);
        if (info.candidate_id != 0u)
            expect_loaded(ESY_CREDENTIALS_STORE_CANDIDATE, 1u, first_bundle());
    } else if (scenario == 1u) {
        TEST_ASSERT_TRUE(info.confirmed_id == 1u || info.confirmed_id == 2u);
        if (info.confirmed_id == 1u) {
            expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, 1u, first_bundle());
            TEST_ASSERT_EQUAL_UINT64(2u, info.candidate_id);
            expect_loaded(ESY_CREDENTIALS_STORE_CANDIDATE, 2u, second_bundle());
        } else {
            expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, 2u, second_bundle());
            TEST_ASSERT_EQUAL_UINT64(0u, info.candidate_id);
        }
    } else {
        TEST_ASSERT_EQUAL_UINT64(1u, info.confirmed_id);
        expect_loaded(ESY_CREDENTIALS_STORE_CONFIRMED, 1u, first_bundle());
        TEST_ASSERT_TRUE(info.candidate_id == 0u || info.candidate_id == 2u);
        if (info.candidate_id != 0u)
            expect_loaded(ESY_CREDENTIALS_STORE_CANDIDATE, 2u, second_bundle());
    }
    TEST_ASSERT_EACH_EQUAL_HEX8(0xffu, s_ctx.media, TEST_REGION_OFFSET);
    TEST_ASSERT_EACH_EQUAL_HEX8(0xffu,
        s_ctx.media + TEST_REGION_OFFSET + 2u * TEST_REGION_BYTES,
        TEST_FLASH_BYTES - TEST_REGION_OFFSET - 2u * TEST_REGION_BYTES);
}

void test_store_real_record_provider_recovers_complete_snapshots_at_every_program_prefix(void) {
    for (unsigned scenario = 0u; scenario < 4u; ++scenario) {
        prepare_flash_transaction(scenario);
        TEST_ASSERT_EQUAL(ESY_OK, perform_flash_transaction(scenario));
        uint32_t programs = s_ctx.flash->write_count;
        TEST_ASSERT_TRUE(programs > 0u);
        for (uint32_t call = 1u; call <= programs; ++call) {
            for (size_t prefix = 0u; prefix <= TEST_PROGRAM_BYTES; ++prefix) {
                restore_flash_transaction();
                s_ctx.flash->fail_write_call = call;
                s_ctx.flash->write_fail_bytes = prefix;
                TEST_ASSERT_EQUAL(ESY_ERR_IO, perform_flash_transaction(scenario));
                expect_atomic_flash_recovery(scenario);
            }
        }
    }
}

void test_store_real_record_provider_preserves_last_snapshot_through_partial_erase(void) {
    const size_t prefixes[] = {0u, 1u, 255u, 256u, TEST_REGION_BYTES, SIZE_MAX};
    for (unsigned scenario = 0u; scenario < 4u; ++scenario) {
        prepare_flash_transaction(scenario);
        for (size_t i = 0u; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
            restore_flash_transaction();
            s_ctx.flash->fail_erase_call = 1u;
            s_ctx.flash->erase_fail_bytes = prefixes[i];
            TEST_ASSERT_EQUAL(ESY_ERR_IO, perform_flash_transaction(scenario));
            expect_atomic_flash_recovery(scenario);
        }
    }
}

void test_store_real_record_read_errors_reconcile_commits_instead_of_assuming_rollback(void) {
    for (unsigned scenario = 0u; scenario < 4u; ++scenario) {
        prepare_flash_transaction(scenario);
        TEST_ASSERT_EQUAL(ESY_OK, perform_flash_transaction(scenario));
        uint32_t reads = s_ctx.flash->read_count;
        for (uint32_t call = 1u; call <= reads; ++call) {
            restore_flash_transaction();
            s_ctx.flash->fail_read_call = call;
            s_ctx.flash->read_fail_bytes = SIZE_MAX;
            TEST_ASSERT_EQUAL(ESY_ERR_IO, perform_flash_transaction(scenario));
            expect_atomic_flash_recovery(scenario);
        }
    }
}
