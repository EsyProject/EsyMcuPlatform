/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"

#include <string.h>

#include "esy_common/defs/protocol/esy_proto_domain_defs.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_proto_bin.h"

#define TEST_BUFFER_BYTES 128u
#define TEST_REQUEST_ID 0x12345678u
#define TEST_STATUS_OFFSET 12u
#define TEST_FOUND_OFFSET 20u
#define TEST_LENGTH_OFFSET 21u

typedef struct {
    esy_remote_manager_t manager;
    uint8_t request[TEST_BUFFER_BYTES];
    uint8_t output[TEST_BUFFER_BYTES];
    size_t request_length;
    size_t output_length;
    unsigned authorization_calls;
    unsigned get_calls;
    unsigned set_calls;
    unsigned remove_calls;
    unsigned apply_calls;
    unsigned save_calls;
    esy_err_t authorization_result;
    esy_err_t operation_result;
    size_t get_length;
    bool get_found;
    bool partial_get_write;
    uint8_t get_value[8];
    esy_remote_manager_request_t authorized;
    char received_key[32];
    size_t received_key_length;
    uint8_t received_value[32];
    size_t received_value_length;
} test_ctx_t;

static test_ctx_t s_ctx;

static void write_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    test_ctx_t *ctx = context;
    ++ctx->authorization_calls;
    ctx->authorized = *request;
    return ctx->authorization_result;
}

static esy_err_t get_value(void *context, const char *key, size_t key_length,
                           uint8_t *output, size_t capacity, size_t *length,
                           bool *found) {
    test_ctx_t *ctx = context;
    ++ctx->get_calls;
    ctx->received_key_length = key_length;
    memcpy(ctx->received_key, key, key_length);
    *length = ctx->get_length;
    *found = ctx->get_found;
    if (ctx->partial_get_write && capacity != 0u) output[0] = 0xeeu;
    if (ctx->operation_result != ESY_OK) return ctx->operation_result;
    if (ctx->get_length > capacity) return ESY_ERR_NO_MEM;
    if (ctx->get_length != 0u) memcpy(output, ctx->get_value, ctx->get_length);
    return ESY_OK;
}

static esy_err_t set_value(void *context, const char *key, size_t key_length,
                           const uint8_t *value, size_t length) {
    test_ctx_t *ctx = context;
    ++ctx->set_calls;
    ctx->received_key_length = key_length;
    memcpy(ctx->received_key, key, key_length);
    ctx->received_value_length = length;
    if (length != 0u) memcpy(ctx->received_value, value, length);
    return ctx->operation_result;
}

static esy_err_t remove_value(void *context, const char *key, size_t key_length) {
    test_ctx_t *ctx = context;
    ++ctx->remove_calls;
    ctx->received_key_length = key_length;
    memcpy(ctx->received_key, key, key_length);
    return ctx->operation_result;
}

static esy_err_t apply_value(void *context) {
    test_ctx_t *ctx = context;
    ++ctx->apply_calls;
    return ctx->operation_result;
}

static esy_err_t save_value(void *context) {
    test_ctx_t *ctx = context;
    ++ctx->save_calls;
    return ctx->operation_result;
}

static void request_build(uint16_t group, uint16_t command,
                          const char *key, size_t key_length,
                          const uint8_t *value, size_t value_length) {
    memset(s_ctx.request, 0, sizeof(s_ctx.request));
    s_ctx.request[0] = ESY_PROTO_DOMAIN_REMOTE_MANAGER;
    s_ctx.request[1] = ESY_REMOTE_MANAGER_PROTO_BIN_REQUEST;
    s_ctx.request[2] = ESY_REMOTE_MANAGER_PROTO_BIN_VERSION;
    write_u16(s_ctx.request + 4u, group);
    write_u16(s_ctx.request + 6u, command);
    write_u32(s_ctx.request + 8u, TEST_REQUEST_ID);
    write_u16(s_ctx.request + 12u, (uint16_t)key_length);
    write_u32(s_ctx.request + 14u, (uint32_t)value_length);
    if (key_length != 0u) memcpy(s_ctx.request + 18u, key, key_length);
    if (value_length != 0u)
        memcpy(s_ctx.request + 18u + key_length, value, value_length);
    s_ctx.request_length = 18u + key_length + value_length;
    s_ctx.output_length = SIZE_MAX;
}

static esy_err_t process(size_t capacity) {
    return esy_remote_manager_proto_bin_process(
        &s_ctx.manager, s_ctx.request, s_ctx.request_length,
        s_ctx.output, capacity, &s_ctx.output_length);
}

static void expect_status(esy_err_t status) {
    TEST_ASSERT_EQUAL_HEX32((uint32_t)status,
                            read_u32(s_ctx.output + TEST_STATUS_OFFSET));
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
    s_ctx.get_length = 3u;
    s_ctx.get_found = true;
    s_ctx.get_value[0] = 0u;
    s_ctx.get_value[1] = 0xffu;
    s_ctx.get_value[2] = 0x7fu;
    const esy_remote_manager_config_t config = {
        .max_key_length = sizeof(s_ctx.received_key),
        .max_request_value_length = SIZE_MAX, .max_response_value_length = SIZE_MAX,
        .authorize = authorize,
        .authorize_context = &s_ctx,
        .config_ops = {
            .get = get_value,
            .set = set_value,
            .remove = remove_value,
            .apply = apply_value,
            .save = save_value,
            .context = &s_ctx,
        },
    };
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
}

void tearDown(void) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}

void test_discovery_response_matches_golden_bytes(void) {
    static const uint8_t k_expected[] = {
        0x02u, 0x02u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u,
        0x78u, 0x56u, 0x34u, 0x12u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x3fu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    request_build(ESY_REMOTE_MANAGER_GROUP_SYSTEM,
                   ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, NULL, 0u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(sizeof(s_ctx.output)));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_expected), s_ctx.output_length);
    TEST_ASSERT_EQUAL_MEMORY(k_expected, s_ctx.output, sizeof(k_expected));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_HEX32(TEST_REQUEST_ID, s_ctx.authorized.request_id);
    TEST_ASSERT_EQUAL_HEX8(0xa5u, s_ctx.output[s_ctx.output_length]);
}

void test_binary_get_response_matches_golden_bytes_and_borrows_tail(void) {
    static const uint8_t k_expected[] = {
        0x02u, 0x02u, 0x01u, 0x00u, 0x01u, 0x00u, 0x01u, 0x00u,
        0x78u, 0x56u, 0x34u, 0x12u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x03u, 0x00u, 0x00u, 0x00u,
        0x00u, 0xffu, 0x7fu,
    };
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_GET, "mode", 4u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(sizeof(k_expected)));
    TEST_ASSERT_EQUAL_UINT(sizeof(k_expected), s_ctx.output_length);
    TEST_ASSERT_EQUAL_MEMORY(k_expected, s_ctx.output, sizeof(k_expected));
    TEST_ASSERT_EQUAL_UINT(4u, s_ctx.received_key_length);
    TEST_ASSERT_EQUAL_MEMORY("mode", s_ctx.received_key, 4u);
    TEST_ASSERT_EQUAL_HEX8(0xa5u, s_ctx.output[s_ctx.output_length]);
}

void test_binary_set_preserves_bytes_and_returns_only_header(void) {
    static const uint8_t k_value[] = {0u, 0xffu, 0x7fu};
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_SET, "mode", 4u,
                   k_value, sizeof(k_value));
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_OK);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT(sizeof(k_value), s_ctx.received_value_length);
    TEST_ASSERT_EQUAL_MEMORY(k_value, s_ctx.received_value, sizeof(k_value));
    TEST_ASSERT_EQUAL_UINT32(0u, read_u32(s_ctx.output + TEST_LENGTH_OFFSET));
    TEST_ASSERT_EQUAL_HEX8(0xa5u, s_ctx.output[25u]);
}

void test_empty_set_is_not_missing_and_executes_once(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_SET, "mode", 4u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_OK);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.received_value_length);
    TEST_ASSERT_NULL(s_ctx.authorized.value);
}

void test_empty_and_missing_get_have_distinct_found_flags(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_GET, "mode", 4u, NULL, 0u);
    s_ctx.get_length = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_OK);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT8(1u, s_ctx.output[TEST_FOUND_OFFSET]);
    s_ctx.get_found = false;
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_OK);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.output[TEST_FOUND_OFFSET]);
    TEST_ASSERT_EQUAL_UINT32(0u, read_u32(s_ctx.output + TEST_LENGTH_OFFSET));
}

void test_get_no_mem_reports_required_bytes_without_appending_value(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_GET, "mode", 4u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(27u));
    expect_status(ESY_ERR_NO_MEM);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT8(1u, s_ctx.output[TEST_FOUND_OFFSET]);
    TEST_ASSERT_EQUAL_UINT32(3u, read_u32(s_ctx.output + TEST_LENGTH_OFFSET));
    TEST_ASSERT_EQUAL_HEX8(0xa5u, s_ctx.output[25u]);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.get_calls);
}

void test_undersized_response_prevents_authorization_and_mutation(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_SET, "mode", 4u, NULL, 0u);
    for (size_t capacity = 0u; capacity < 25u; ++capacity) {
        s_ctx.output_length = SIZE_MAX;
        TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM, process(capacity));
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.set_calls);
    TEST_ASSERT_EACH_EQUAL_HEX8(0xa5u, s_ctx.output, sizeof(s_ctx.output));
}

void test_undersized_response_prevents_remove_apply_and_save(void) {
    static const uint16_t k_commands[] = {
        ESY_REMOTE_MANAGER_CONFIG_REMOVE,
        ESY_REMOTE_MANAGER_CONFIG_APPLY,
        ESY_REMOTE_MANAGER_CONFIG_SAVE,
    };
    for (size_t i = 0u; i < sizeof(k_commands) / sizeof(k_commands[0]); ++i) {
        const bool keyed = k_commands[i] == ESY_REMOTE_MANAGER_CONFIG_REMOVE;
        request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG, k_commands[i],
                       keyed ? "mode" : NULL, keyed ? 4u : 0u, NULL, 0u);
        TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM, process(24u));
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.remove_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.apply_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.save_calls);
}

void test_authorization_denial_is_encoded_and_does_not_call_provider(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_SET, "mode", 4u, NULL, 0u);
    s_ctx.authorization_result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_ERR_BUSY);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, read_u32(s_ctx.output + TEST_LENGTH_OFFSET));
}

void test_provider_error_is_encoded_without_adapter_retry(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_SET, "mode", 4u, NULL, 0u);
    s_ctx.operation_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_ERR_IO);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.set_calls);
}

void test_failed_get_does_not_export_partial_provider_output(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_GET, "mode", 4u, NULL, 0u);
    s_ctx.operation_result = ESY_ERR_IO;
    s_ctx.partial_get_write = true;
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(sizeof(s_ctx.output)));
    expect_status(ESY_ERR_IO);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT32(0u, read_u32(s_ctx.output + TEST_LENGTH_OFFSET));
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.output[TEST_FOUND_OFFSET]);
    TEST_ASSERT_EQUAL_HEX8(0xeeu, s_ctx.output[25u]);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.get_calls);
}

void test_unknown_group_and_command_preserve_request_identity(void) {
    static const uint16_t k_groups[] = {3u, 0x1234u};
    static const uint16_t k_commands[] = {1u, 0x5678u};
    for (size_t i = 0u; i < sizeof(k_groups) / sizeof(k_groups[0]); ++i) {
        request_build(k_groups[i], k_commands[i], NULL, 0u, NULL, 0u);
        write_u32(s_ctx.request + 8u, UINT32_MAX);
        TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
        expect_status(ESY_ERR_NOT_SUPPORTED);
        TEST_ASSERT_EQUAL_MEMORY(s_ctx.request + 4u, s_ctx.output + 4u, 8u);
        TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, s_ctx.authorized.request_id);
    }
}

void test_every_truncated_header_is_rejected_without_dispatch(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_SYSTEM,
                   ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, NULL, 0u, NULL, 0u);
    for (size_t length = 0u; length < 18u; ++length) {
        s_ctx.request_length = length;
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, process(25u));
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
    TEST_ASSERT_EACH_EQUAL_HEX8(0xa5u, s_ctx.output, sizeof(s_ctx.output));
}

void test_truncated_key_value_and_trailing_bytes_are_rejected(void) {
    static const uint8_t k_value[] = {1u, 2u};
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_SET, "mode", 4u,
                   k_value, sizeof(k_value));
    const size_t complete_length = s_ctx.request_length;
    for (size_t length = 18u; length <= complete_length + 1u; ++length) {
        if (length == complete_length) continue;
        s_ctx.request_length = length;
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, process(25u));
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.set_calls);
}

void test_exaggerated_wire_lengths_cannot_wrap_into_valid_request(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_SET, NULL, 0u, NULL, 0u);
    write_u16(s_ctx.request + 12u, UINT16_MAX);
    write_u32(s_ctx.request + 14u, UINT32_MAX);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, process(25u));
    write_u16(s_ctx.request + 12u, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, process(25u));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
}

void test_other_domain_version_kind_and_reserved_are_rejected(void) {
    static const struct {
        size_t offset;
        uint8_t value;
        esy_err_t expected;
    } k_cases[] = {
        {0u, 0x7fu, ESY_ERR_NOT_SUPPORTED},
        {2u, 0xffu, ESY_ERR_NOT_SUPPORTED},
        {1u, ESY_REMOTE_MANAGER_PROTO_BIN_RESPONSE, ESY_ERR_INVALID_ARG},
        {1u, 0u, ESY_ERR_INVALID_ARG},
        {3u, 1u, ESY_ERR_INVALID_ARG},
    };
    for (size_t i = 0u; i < sizeof(k_cases) / sizeof(k_cases[0]); ++i) {
        request_build(ESY_REMOTE_MANAGER_GROUP_SYSTEM,
                       ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, NULL, 0u, NULL, 0u);
        s_ctx.request[k_cases[i].offset] = k_cases[i].value;
        TEST_ASSERT_EQUAL_INT(k_cases[i].expected, process(25u));
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
}

void test_semantically_invalid_key_is_encoded_without_authorization(void) {
    static const char k_key[] = {'m', '\0', 'd'};
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_GET, k_key, sizeof(k_key), NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_ERR_INVALID_ARG);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.get_calls);
}

void test_null_arguments_are_rejected_without_dispatch(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_SYSTEM,
                   ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, NULL, 0u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(NULL, s_ctx.request,
            s_ctx.request_length, s_ctx.output, 25u, &s_ctx.output_length));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager, NULL,
            s_ctx.request_length, s_ctx.output, 25u, &s_ctx.output_length));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
            s_ctx.request_length, NULL, 25u, &s_ctx.output_length));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_NO_MEM,
        esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
            s_ctx.request_length, NULL, 0u, &s_ctx.output_length));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
            s_ctx.request_length, s_ctx.output, 25u, NULL));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
}

void test_overlapping_input_output_or_manager_is_rejected(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_SET, "mode", 4u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
            s_ctx.request_length, s_ctx.request, 25u, &s_ctx.output_length));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
            s_ctx.request_length, (uint8_t *)&s_ctx.manager, 25u,
            &s_ctx.output_length));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager,
            (const uint8_t *)&s_ctx.manager, 18u, s_ctx.output, 25u,
            &s_ctx.output_length));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.set_calls);
}

void test_overlapping_output_length_and_wrapping_ranges_are_rejected(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_SYSTEM,
                   ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, NULL, 0u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager,
            (const uint8_t *)&s_ctx.output_length, sizeof(s_ctx.output_length),
            s_ctx.output, 25u, &s_ctx.output_length));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
            s_ctx.request_length, (uint8_t *)&s_ctx.output_length,
            sizeof(s_ctx.output_length), &s_ctx.output_length));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
            SIZE_MAX, s_ctx.output, 25u, &s_ctx.output_length));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, process(SIZE_MAX));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
}

void test_uninitialized_manager_error_is_encoded_as_business_response(void) {
    request_build(ESY_REMOTE_MANAGER_GROUP_SYSTEM,
                   ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, NULL, 0u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_ERR_STATE);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorization_calls);
}

void test_required_length_above_wire_limit_is_not_truncated(void) {
#if SIZE_MAX > UINT32_MAX
    request_build(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                   ESY_REMOTE_MANAGER_CONFIG_GET, "mode", 4u, NULL, 0u);
    s_ctx.get_length = (size_t)UINT32_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, process(25u));
    expect_status(ESY_ERR_STATE);
    TEST_ASSERT_EQUAL_UINT(25u, s_ctx.output_length);
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.output[TEST_FOUND_OFFSET]);
    TEST_ASSERT_EQUAL_UINT32(0u, read_u32(s_ctx.output + TEST_LENGTH_OFFSET));
#else
    TEST_IGNORE_MESSAGE("size_t cannot represent lengths beyond the wire limit");
#endif
}
