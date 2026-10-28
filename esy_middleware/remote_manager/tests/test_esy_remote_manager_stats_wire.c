/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/remote_manager/esy_remote_manager_proto_bin.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_stats.h"
#include "unity.h"

#include <string.h>

#define TEST_REQUEST_BYTES 128u
#define TEST_RESPONSE_BYTES 128u
#define TEST_SUCCESS_BYTES 113u
#define TEST_NAME_BYTES 13u

typedef struct {
    esy_remote_manager_t manager;
    uint8_t request[TEST_REQUEST_BYTES];
    uint8_t response[TEST_RESPONSE_BYTES];
    esy_remote_manager_stats_snapshot_t stats;
    esy_err_t authorization_result;
    esy_err_t provider_result;
    unsigned authorizations;
    unsigned enumerations;
    unsigned reads;
    uint32_t requested_index;
} stats_wire_ctx_t;

static stats_wire_ctx_t s_ctx;
static const char k_name[] = "comm.rx_bytes";
static const uint8_t k_index[] = {1u, 0u, 0u, 0u};

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    stats_wire_ctx_t *ctx = context;
    ++ctx->authorizations;
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4u, request->request_id);
    TEST_ASSERT_EQUAL_UINT32(0u, request->value_length);
    return ctx->authorization_result;
}

static esy_err_t enumerate_stat(void *context, uint32_t index,
                                esy_remote_manager_stats_snapshot_t *stats) {
    stats_wire_ctx_t *ctx = context;
    ++ctx->enumerations;
    ctx->requested_index = index;
    if (ctx->provider_result != ESY_OK) return ctx->provider_result;
    if (index >= ctx->stats.count && !(index == 0u && ctx->stats.count == 0u))
        return ESY_ERR_INVALID_ARG;
    *stats = ctx->stats;
    stats->index = index;
    return ESY_OK;
}

static esy_err_t read_stat(void *context, const char *name, size_t length,
                           esy_remote_manager_stats_snapshot_t *stats) {
    stats_wire_ctx_t *ctx = context;
    ++ctx->reads;
    if (ctx->provider_result != ESY_OK) return ctx->provider_result;
    if (length != ctx->stats.name_length ||
        memcmp(name, ctx->stats.name, length) != 0) return ESY_ERR_NOT_SUPPORTED;
    *stats = ctx->stats;
    return ESY_OK;
}

static size_t make_request(uint16_t command, const char *key, size_t key_length,
                            const uint8_t *value, size_t value_length) {
    memset(s_ctx.request, 0, sizeof(s_ctx.request));
    const uint8_t header[] = {
        ESY_PROTO_DOMAIN_REMOTE_MANAGER, 1u, 1u, 0u, 5u, 0u,
        (uint8_t)command, (uint8_t)(command >> 8u),
        0xd4u, 0xc3u, 0xb2u, 0xa1u, (uint8_t)key_length, 0u,
        (uint8_t)value_length, 0u, 0u, 0u,
    };
    memcpy(s_ctx.request, header, sizeof(header));
    if (key_length != 0u) memcpy(s_ctx.request + sizeof(header), key, key_length);
    if (value_length != 0u)
        memcpy(s_ctx.request + sizeof(header) + key_length, value, value_length);
    return sizeof(header) + key_length + value_length;
}

static size_t enumerate_request(void) {
    return make_request(ESY_REMOTE_MANAGER_STATS_ENUMERATE, NULL, 0u,
                        k_index, sizeof(k_index));
}

static size_t read_request(void) {
    return make_request(ESY_REMOTE_MANAGER_STATS_READ, k_name,
                        TEST_NAME_BYTES, NULL, 0u);
}

static size_t process(size_t length, size_t capacity) {
    size_t output_length = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_proto_bin_process(
        &s_ctx.manager, s_ctx.request, length, s_ctx.response, capacity,
        &output_length));
    return output_length;
}

static int32_t response_status(void) {
    return (int32_t)((uint32_t)s_ctx.response[12] |
        ((uint32_t)s_ctx.response[13] << 8u) |
        ((uint32_t)s_ctx.response[14] << 16u) |
        ((uint32_t)s_ctx.response[15] << 24u));
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.stats = (esy_remote_manager_stats_snapshot_t){
        .count = 3u, .index = 1u, .name_length = TEST_NAME_BYTES,
        .kind = ESY_REMOTE_MANAGER_STATS_COUNTER,
        .unit = ESY_REMOTE_MANAGER_STATS_UNIT_BYTES,
        .sample = {.value = UINT64_C(0xfedcba9876543210),
            .epoch = UINT64_C(0x0123456789abcdef), .available = true},
    };
    memset(s_ctx.stats.name, 0xa5, sizeof(s_ctx.stats.name));
    memcpy(s_ctx.stats.name, k_name, sizeof(k_name));
    const esy_remote_manager_config_t config = {
        .max_key_length = ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES,
        .max_request_value_length = 4u, .max_response_value_length = 88u,
        .authorize = authorize, .authorize_context = &s_ctx,
        .stats_ops = {.enumerate = enumerate_stat, .read = read_stat,
            .context = &s_ctx},
    };
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}

void test_stats_wire_has_exact_v1_snapshot_and_preserves_full_u64_precision(void) {
    const size_t length = enumerate_request();
    memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    const uint8_t expected[TEST_SUCCESS_BYTES] = {
        2u, 2u, 1u, 0u, 5u, 0u, 1u, 0u, 0xd4u, 0xc3u, 0xb2u, 0xa1u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 88u, 0u, 0u, 0u,
        3u, 0u, 0u, 0u, 1u, 0u, 0u, 0u,
        1u, 0u, 0u, 0u, 2u, 0u, 0u, 0u,
        1u, 0u, 0u, 0u, 13u, 0u, 0u, 0u,
        0xefu, 0xcdu, 0xabu, 0x89u, 0x67u, 0x45u, 0x23u, 0x01u,
        0x10u, 0x32u, 0x54u, 0x76u, 0x98u, 0xbau, 0xdcu, 0xfeu,
        'c', 'o', 'm', 'm', '.', 'r', 'x', '_', 'b', 'y', 't', 'e', 's',
    };
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.response, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[TEST_SUCCESS_BYTES]);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.requested_index);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.enumerations);
}

void test_stats_read_uses_the_name_key_and_returns_the_same_named_sample(void) {
    const size_t length = read_request();
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    TEST_ASSERT_EQUAL(ESY_OK, response_status());
    TEST_ASSERT_EQUAL_MEMORY(k_name, s_ctx.response + 65u, TEST_NAME_BYTES);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.reads);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumerations);
    TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.response[6]);
}

void test_stats_enumeration_preserves_all_index_bits_without_a_fixed_catalogue_limit(void) {
    s_ctx.stats.count = 0x01020305u;
    const uint8_t index[] = {4u, 3u, 2u, 1u};
    const size_t length = make_request(ESY_REMOTE_MANAGER_STATS_ENUMERATE,
        NULL, 0u, index, sizeof(index));
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    TEST_ASSERT_EQUAL_HEX32(0x01020304u, s_ctx.requested_index);
    TEST_ASSERT_EQUAL_MEMORY(index, s_ctx.response + 29u, sizeof(index));
}

void test_stats_wire_distinguishes_an_unavailable_sample_from_an_available_zero(void) {
    s_ctx.stats.sample.value = 0u;
    s_ctx.stats.sample.available = false;
    size_t length = read_request();
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.response[41]);
    const uint8_t zero[8] = {0};
    TEST_ASSERT_EQUAL_MEMORY(zero, s_ctx.response + 57u, sizeof(zero));
    const uint8_t epoch[] = {0xefu, 0xcdu, 0xabu, 0x89u, 0x67u, 0x45u, 0x23u, 0x01u};
    TEST_ASSERT_EQUAL_MEMORY(epoch, s_ctx.response + 49u, sizeof(epoch));
    s_ctx.stats.sample.available = true;
    length = read_request();
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(1u, s_ctx.response[41]);
    TEST_ASSERT_EQUAL_MEMORY(zero, s_ctx.response + 57u, sizeof(zero));
}

void test_stats_empty_inventory_has_an_explicit_zero_snapshot(void) {
    s_ctx.stats = (esy_remote_manager_stats_snapshot_t){0};
    const uint8_t first[4] = {0};
    const size_t length = make_request(ESY_REMOTE_MANAGER_STATS_ENUMERATE,
        NULL, 0u, first, sizeof(first));
    const uint8_t empty[ESY_REMOTE_MANAGER_PROTO_BIN_STATS_BYTES] = {0};
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    TEST_ASSERT_EQUAL_MEMORY(empty, s_ctx.response + 25u, sizeof(empty));
}

void test_stats_commands_preflight_the_complete_fixed_snapshot_before_callbacks(void) {
    for (uint16_t command = ESY_REMOTE_MANAGER_STATS_ENUMERATE;
         command <= ESY_REMOTE_MANAGER_STATS_READ; ++command) {
        const size_t length = command == ESY_REMOTE_MANAGER_STATS_ENUMERATE
            ? enumerate_request() : read_request();
        memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
        TEST_ASSERT_EQUAL_UINT32(25u, process(length, TEST_SUCCESS_BYTES - 1u));
        TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, response_status());
        TEST_ASSERT_EQUAL_UINT8(88u, s_ctx.response[21]);
        TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[25]);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumerations + s_ctx.reads);
}

void test_stats_configured_response_budget_is_enforced_before_authorization(void) {
    const size_t length = read_request();
    s_ctx.manager.config.max_response_value_length = 87u;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, response_status());
    TEST_ASSERT_EQUAL_UINT8(88u, s_ctx.response[21]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.reads);
    s_ctx.manager.config.max_response_value_length = 88u;
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.reads);
}

void test_stats_malformed_enumeration_value_produces_no_response(void) {
    const uint8_t argument[5] = {0};
    const size_t invalid_lengths[] = {0u, 3u, 5u};
    for (size_t i = 0u; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); ++i) {
        const size_t length = make_request(ESY_REMOTE_MANAGER_STATS_ENUMERATE,
            NULL, 0u, argument, invalid_lengths[i]);
        size_t output_length = 99u;
        memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
            esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
                length, s_ctx.response, sizeof(s_ctx.response), &output_length));
        TEST_ASSERT_EQUAL_UINT32(0u, output_length);
        TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[0]);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumerations);
}

void test_stats_unused_arguments_and_invalid_names_reject_before_authorization(void) {
    size_t length = make_request(ESY_REMOTE_MANAGER_STATS_ENUMERATE,
        "x", 1u, k_index, sizeof(k_index));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response_status());
    length = make_request(ESY_REMOTE_MANAGER_STATS_READ, k_name,
        TEST_NAME_BYTES, k_index, 1u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response_status());
    const char invalid[] = {'a', '\0', 'b'};
    const char *names[] = {"", "bad@name", invalid};
    const size_t lengths[] = {0u, 8u, sizeof(invalid)};
    for (size_t i = 0u; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        length = make_request(ESY_REMOTE_MANAGER_STATS_READ, names[i], lengths[i], NULL, 0u);
        TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response_status());
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumerations + s_ctx.reads);
}

void test_stats_name_and_enumeration_budgets_apply_to_decoded_arguments(void) {
    s_ctx.manager.config.max_request_value_length = 3u;
    size_t length = enumerate_request();
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response_status());
    s_ctx.manager.config.max_key_length = TEST_NAME_BYTES - 1u;
    length = read_request();
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response_status());
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    s_ctx.manager.config.max_key_length = TEST_NAME_BYTES;
    s_ctx.manager.config.max_request_value_length = 0u;
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.reads);
}

void test_stats_authorization_denial_does_not_query_any_provider(void) {
    s_ctx.authorization_result = ESY_ERR_STATE;
    for (uint16_t command = ESY_REMOTE_MANAGER_STATS_ENUMERATE;
         command <= ESY_REMOTE_MANAGER_STATS_READ; ++command) {
        const size_t length = command == ESY_REMOTE_MANAGER_STATS_ENUMERATE
            ? enumerate_request() : read_request();
        TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
        TEST_ASSERT_EQUAL(ESY_ERR_STATE, response_status());
        TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.response[21]);
    }
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumerations + s_ctx.reads);
}

void test_stats_provider_errors_have_no_partial_snapshot(void) {
    s_ctx.provider_result = ESY_ERR_IO;
    for (uint16_t command = ESY_REMOTE_MANAGER_STATS_ENUMERATE;
         command <= ESY_REMOTE_MANAGER_STATS_READ; ++command) {
        const size_t length = command == ESY_REMOTE_MANAGER_STATS_ENUMERATE
            ? enumerate_request() : read_request();
        memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
        TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
        TEST_ASSERT_EQUAL(ESY_ERR_IO, response_status());
        TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.response[21]);
        TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[25]);
    }
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.enumerations);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.reads);
}

void test_stats_unknown_commands_missing_providers_and_unknown_names_are_unsupported(void) {
    size_t length = make_request(UINT16_MAX, NULL, 0u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, 25u));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, response_status());
    length = make_request(ESY_REMOTE_MANAGER_STATS_READ, "unknown", 7u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, response_status());
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.reads);
    s_ctx.manager.config.stats_ops.read = NULL;
    length = read_request();
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, response_status());
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.reads);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumerations);
}

void test_stats_maximum_name_and_u64_values_fit_without_nul_or_integer_truncation(void) {
    memset(s_ctx.stats.name, 'a', ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES);
    s_ctx.stats.name[ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES] = '\0';
    s_ctx.stats.name_length = ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES;
    s_ctx.stats.sample.value = UINT64_MAX;
    s_ctx.stats.sample.epoch = UINT64_MAX;
    const size_t length = make_request(ESY_REMOTE_MANAGER_STATS_READ,
        s_ctx.stats.name, s_ctx.stats.name_length, NULL, 0u);
    memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    for (size_t i = 49u; i < 65u; ++i) TEST_ASSERT_EQUAL_UINT8(0xffu, s_ctx.response[i]);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.stats.name, s_ctx.response + 65u,
                              ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[TEST_SUCCESS_BYTES]);
}

void test_stats_discovery_advertises_only_installed_statistics_operations(void) {
    const size_t length = make_request(ESY_REMOTE_MANAGER_SYSTEM_DISCOVER,
        NULL, 0u, NULL, 0u);
    s_ctx.request[4] = ESY_REMOTE_MANAGER_GROUP_SYSTEM;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    const uint8_t expected[] = {1u, 0u, 48u, 0u};
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.response + 16u, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumerations + s_ctx.reads);
}
