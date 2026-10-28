/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_coredump_store.h"
#include "esy_middleware/coredump/esy_coredump_file.h"
#include "esy_middleware/file/esy_file_ram.h"

#include <string.h>

#define TEST_STACK_BYTES 4u
#define TEST_RECORD_BYTES (ESY_COREDUMP_HEADER_BYTES + TEST_STACK_BYTES)

typedef struct {
    esy_coredump_t store;
    esy_coredump_file_t persistence;
    esy_file_t file;
    esy_file_ram_t ram_file;
    esy_remote_manager_coredump_store_t adapter;
    esy_remote_manager_coredump_store_config_t config;
    esy_remote_manager_coredump_ops_t ops;
    esy_remote_manager_t manager;
    _Alignas(uint32_t) uint8_t retained[TEST_RECORD_BYTES];
    uint8_t stack[TEST_STACK_BYTES];
    uint8_t saved[TEST_RECORD_BYTES];
    uint8_t file_data[ESY_COREDUMP_MAX_RECORD_BYTES];
    uint8_t file_workspace[ESY_COREDUMP_MAX_RECORD_BYTES];
    unsigned clear_calls;
    esy_err_t clear_result;
    bool persistent_record;
    bool authorized;
    unsigned next_id;
    esy_coredump_record_id_t record_id;
} fixture_ctx_t;

static fixture_ctx_t s_ctx;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    fixture_ctx_t *ctx = context;
    (void)request;
    return ctx->authorized ? ESY_OK : ESY_ERR_FAIL;
}

static esy_err_t new_record_id(void *context, esy_coredump_record_id_t *record_id) {
    fixture_ctx_t *ctx = context;
    *record_id = (esy_coredump_record_id_t){0};
    record_id->bytes[0] = (uint8_t)++ctx->next_id;
    return ESY_OK;
}

static esy_err_t clear_authoritative_sources(void *context,
                                             const esy_coredump_record_id_t *record_id) {
    fixture_ctx_t *ctx = context;
    ctx->clear_calls++;
    if (ctx->clear_result != ESY_OK) return ctx->clear_result;
    const esy_err_t result = esy_coredump_file_clear(&ctx->persistence, record_id);
    if (result == ESY_OK) ctx->persistent_record = false;
    return result;
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    const esy_coredump_static_resources_t resources = {
        .retained_buffer = s_ctx.retained,
        .retained_size = sizeof(s_ctx.retained),
    };
    esy_coredump_config_t config = {
        .static_resources = &resources, .stack_limit = TEST_STACK_BYTES,
        .image_base = UINT32_C(0x10000000), .firmware_version = 3,
        .new_record_id = new_record_id, .record_id_context = &s_ctx,
    };
    memset(config.build_id, 0xa5, sizeof(config.build_id));
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_init(&s_ctx.store, &config));
    const esy_file_ram_static_resources_t file_resources = {
        .data_buffer = s_ctx.file_data, .capacity = sizeof(s_ctx.file_data)};
    const esy_file_ram_config_t file_config = {.static_resources = &file_resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_init(&s_ctx.ram_file, &file_config));
    esy_file_ops_t file_ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_get_ops(&s_ctx.ram_file, &file_ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_init(&s_ctx.file, &file_ops));
    const esy_coredump_file_static_resources_t persistence_resources = {
        .record_buffer = s_ctx.file_workspace, .record_capacity = sizeof(s_ctx.file_workspace)};
    const esy_coredump_file_config_t persistence = {
        .store = &s_ctx.store, .file = &s_ctx.file, .static_resources = &persistence_resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_init(&s_ctx.persistence, &persistence));
    s_ctx.record_id.bytes[0] = 1u;
    s_ctx.config.store = &s_ctx.store;
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_coredump_store_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_deinit(&s_ctx.persistence));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_ram_deinit(&s_ctx.ram_file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_deinit(&s_ctx.store));
}

static void initialize(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_coredump_store_init(
        &s_ctx.adapter, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_coredump_store_get_ops(
        &s_ctx.adapter, &s_ctx.ops));
}

static void capture(void) {
    const esy_fault_snapshot_t snapshot = {
        .architecture = ESY_FAULT_ARCH_ARM_M,
        .reason = ESY_FAULT_REASON_SOFTWARE,
        .flags = ESY_FAULT_FLAG_STACK_VALID,
        .stack = s_ctx.stack, .stack_length = sizeof(s_ctx.stack),
        .stack_address = UINT32_C(0x20001000),
    };
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_capture_fault(&snapshot, &s_ctx.store));
    esy_coredump_info_t info;
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_get_info(&s_ctx.store, &info));
    s_ctx.record_id = info.record_id;
    if (s_ctx.persistent_record)
        TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_file_persist(&s_ctx.persistence));
    memcpy(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
}

void test_coredump_adapter_validates_lifecycle_without_owning_store(void) {
    esy_remote_manager_coredump_ops_t zero_ops = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_coredump_store_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_coredump_store_init(&s_ctx.adapter, NULL));
    s_ctx.config.store = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_coredump_store_init(&s_ctx.adapter, &s_ctx.config));
    s_ctx.config.store = &s_ctx.store;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_coredump_store_get_ops(NULL, &s_ctx.ops));
    TEST_ASSERT_EQUAL_MEMORY(&zero_ops, &s_ctx.ops, sizeof(zero_ops));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_coredump_store_get_ops(&s_ctx.adapter, &s_ctx.ops));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_coredump_store_get_ops(&s_ctx.adapter, NULL));
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_coredump_store_init(&s_ctx.adapter, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_coredump_store_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_coredump_store_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_coredump_store_deinit(NULL));
    capture();
    initialize();
}

void test_coredump_adapter_empty_record_is_successfully_reported_unavailable(void) {
    esy_remote_manager_coredump_info_t info = {
        .available = true, .record_size = 10, .record_crc = 20,
    };
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.info(s_ctx.ops.context, &info));
    TEST_ASSERT_FALSE(info.available);
    TEST_ASSERT_EQUAL_UINT32(0, info.record_size);
    TEST_ASSERT_EQUAL_UINT32(0, info.record_crc);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        s_ctx.ops.read(s_ctx.ops.context, &s_ctx.record_id, 0, NULL, 0));
    TEST_ASSERT_NULL(s_ctx.ops.clear);
}

void test_coredump_adapter_info_and_range_read_preserve_record_and_crc(void) {
    esy_remote_manager_coredump_info_t info;
    esy_coredump_info_t original;
    uint8_t output[7];
    capture();
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_get_info(&s_ctx.store, &original));
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.info(s_ctx.ops.context, &info));
    TEST_ASSERT_TRUE(info.available);
    TEST_ASSERT_EQUAL_UINT32(original.record_size, info.record_size);
    TEST_ASSERT_EQUAL_UINT32(original.record_crc, info.record_crc);
    TEST_ASSERT_EQUAL_MEMORY(original.record_id.bytes, info.record_id.bytes, ESY_COREDUMP_RECORD_ID_BYTES);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.read(s_ctx.ops.context, &s_ctx.record_id, 3,
                                           output, sizeof(output)));
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.retained + 3, output, sizeof(output));
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.read(s_ctx.ops.context, &s_ctx.record_id,
                                           original.record_size, NULL, 0));
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.clear_calls);
}

void test_coredump_adapter_propagates_invalid_ranges_and_crc_errors_without_clearing(void) {
    uint8_t output = 0xa5;
    esy_remote_manager_coredump_info_t info;
    capture();
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.read(s_ctx.ops.context, &s_ctx.record_id, TEST_RECORD_BYTES, &output, 1));
    TEST_ASSERT_EQUAL_HEX8(0xa5, output);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.read(s_ctx.ops.context, &s_ctx.record_id, 0, NULL, 1));
    s_ctx.retained[TEST_RECORD_BYTES - 1u] ^= 1;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, s_ctx.ops.info(s_ctx.ops.context, &info));
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL,
        s_ctx.ops.read(s_ctx.ops.context, &s_ctx.record_id, 0, &output, 1));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.clear_calls);
}

void test_coredump_adapter_only_exposes_explicit_owner_clear_and_propagates_failure(void) {
    esy_remote_manager_coredump_info_t info;
    s_ctx.config.clear = clear_authoritative_sources;
    s_ctx.config.clear_context = &s_ctx;
    s_ctx.persistent_record = true;
    capture();
    initialize();
    TEST_ASSERT_NOT_NULL(s_ctx.ops.clear);
    s_ctx.clear_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.ops.clear(s_ctx.ops.context, &s_ctx.record_id));
    TEST_ASSERT_TRUE(s_ctx.persistent_record);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.clear_calls);
    s_ctx.clear_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.clear(s_ctx.ops.context, &s_ctx.record_id));
    TEST_ASSERT_FALSE(s_ctx.persistent_record);
    TEST_ASSERT_EQUAL_UINT(2, s_ctx.clear_calls);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.info(s_ctx.ops.context, &info));
    TEST_ASSERT_FALSE(info.available);
}

void test_coredump_adapter_core_authorization_controls_explicit_discard(void) {
    esy_remote_manager_response_t response;
    s_ctx.config.clear = clear_authoritative_sources;
    s_ctx.config.clear_context = &s_ctx;
    capture();
    initialize();
    const esy_remote_manager_config_t config = {
        .max_key_length = 1, .max_request_value_length = TEST_RECORD_BYTES, .max_response_value_length = TEST_RECORD_BYTES,
        .authorize = authorize, .authorize_context = &s_ctx,
        .coredump_ops = s_ctx.ops,
    };
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
    const esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_COREDUMP,
        .command = ESY_REMOTE_MANAGER_COREDUMP_CLEAR,
        .record_id = s_ctx.record_id,
    };
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.clear_calls);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
    s_ctx.authorized = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.clear_calls);
}

void test_coredump_adapter_detach_does_not_clear_or_deinitialize_the_store(void) {
    esy_coredump_info_t info;
    capture();
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_coredump_store_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_get_info(&s_ctx.store, &info));
    TEST_ASSERT_EQUAL_UINT32(TEST_RECORD_BYTES, info.record_size);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
}

void test_coredump_adapter_old_record_requests_cannot_read_or_clear_an_identical_new_fault(void) {
    s_ctx.config.clear = clear_authoritative_sources;
    s_ctx.config.clear_context = &s_ctx;
    capture();
    initialize();
    const esy_coredump_record_id_t old_id = s_ctx.record_id;
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.clear(s_ctx.ops.context, &old_id));
    capture();
    TEST_ASSERT_NOT_EQUAL(old_id.bytes[0], s_ctx.record_id.bytes[0]);
    uint8_t output[3] = {0xa5u, 0xa5u, 0xa5u};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        s_ctx.ops.read(s_ctx.ops.context, &old_id, 0u, output, sizeof(output)));
    TEST_ASSERT_EACH_EQUAL_UINT8(0xa5u, output, sizeof(output));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        s_ctx.ops.read(s_ctx.ops.context, &old_id, 0u, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.ops.clear(s_ctx.ops.context, &old_id));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.clear_calls);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
    esy_coredump_record_id_t last_byte_wrong = s_ctx.record_id;
    last_byte_wrong.bytes[ESY_COREDUMP_RECORD_ID_BYTES - 1u] ^= 0x80u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.ops.clear(s_ctx.ops.context, &last_byte_wrong));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.clear_calls);
    TEST_ASSERT_EQUAL(ESY_OK,
        s_ctx.ops.read(s_ctx.ops.context, &s_ctx.record_id, 0u, output, sizeof(output)));
}

void test_coredump_adapter_owner_must_match_persistent_authority_before_discard(void) {
    s_ctx.config.clear = clear_authoritative_sources;
    s_ctx.config.clear_context = &s_ctx;
    s_ctx.persistent_record = true;
    capture();
    initialize();
    const esy_coredump_record_id_t authoritative_id = s_ctx.record_id;
    TEST_ASSERT_EQUAL(ESY_OK, esy_coredump_clear(&s_ctx.store, &authoritative_id));
    s_ctx.persistent_record = false;
    capture();
    s_ctx.persistent_record = true;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        s_ctx.ops.clear(s_ctx.ops.context, &s_ctx.record_id));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.clear_calls);
    TEST_ASSERT_TRUE(s_ctx.persistent_record);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.saved, s_ctx.retained, sizeof(s_ctx.saved));
}

void test_coredump_adapter_zero_ids_are_invalid_without_owner_discard(void) {
    s_ctx.config.clear = clear_authoritative_sources;
    s_ctx.config.clear_context = &s_ctx;
    capture();
    initialize();
    const esy_coredump_record_id_t zero = {0};
    uint8_t output = 0xa5u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, s_ctx.ops.clear(s_ctx.ops.context, &zero));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, s_ctx.ops.clear(s_ctx.ops.context, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.read(s_ctx.ops.context, &zero, 0u, &output, sizeof(output)));
    TEST_ASSERT_EQUAL_UINT8(0xa5u, output);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.clear_calls);
}
