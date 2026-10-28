/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_stats.h"

#include <string.h>

typedef struct {
    esy_remote_manager_t manager;
    esy_remote_manager_config_t config;
    esy_remote_manager_stats_snapshot_t snapshot;
    esy_err_t authorization;
    esy_err_t provider_result;
    uint32_t last_index;
    char last_name[ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES + 1u];
    size_t last_name_length;
    unsigned authorization_calls;
    unsigned enumeration_calls;
    unsigned read_calls;
} remote_manager_stats_test_ctx_t;

static remote_manager_stats_test_ctx_t s_ctx;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    remote_manager_stats_test_ctx_t *ctx = context;
    (void)request;
    ++ctx->authorization_calls;
    return ctx->authorization;
}

static esy_err_t enumerate(void *context, uint32_t index,
                           esy_remote_manager_stats_snapshot_t *snapshot) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
    remote_manager_stats_test_ctx_t *ctx = context;
    ++ctx->enumeration_calls;
    ctx->last_index = index;
    *snapshot = ctx->snapshot;
    return ctx->provider_result;
}

static esy_err_t read_stat(void *context, const char *name, size_t length,
                           esy_remote_manager_stats_snapshot_t *snapshot) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
    remote_manager_stats_test_ctx_t *ctx = context;
    ++ctx->read_calls;
    TEST_ASSERT_LESS_OR_EQUAL_UINT(ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES, length);
    memcpy(ctx->last_name, name, length);
    ctx->last_name[length] = '\0';
    ctx->last_name_length = length;
    *snapshot = ctx->snapshot;
    return ctx->provider_result;
}

static void initialize(void) {
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_remote_manager_init(&s_ctx.manager, &s_ctx.config));
}

static esy_err_t dispatch(const esy_remote_manager_request_t *request,
                           esy_remote_manager_response_t *response) {
    const esy_err_t result = esy_remote_manager_dispatch(
        &s_ctx.manager, request, NULL, 0u, response);
    TEST_ASSERT_EQUAL(result, response->status);
    return result;
}

static esy_remote_manager_request_t request_for(uint16_t command) {
    esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_STATS, .command = command};
    if (command == ESY_REMOTE_MANAGER_STATS_ENUMERATE)
        request.stat_index = s_ctx.snapshot.index;
    else if (command == ESY_REMOTE_MANAGER_STATS_READ) {
        request.key = s_ctx.snapshot.name;
        request.key_length = s_ctx.snapshot.name_length;
    }
    return request;
}

static void assert_empty_stats(const esy_remote_manager_response_t *response) {
    TEST_ASSERT_EQUAL_UINT32(0u, response->stats.count);
    TEST_ASSERT_EQUAL_UINT32(0u, response->stats.index);
    TEST_ASSERT_EQUAL_UINT(0u, response->stats.name_length);
    TEST_ASSERT_EQUAL_UINT8(0u, response->stats.name[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, response->stats.kind);
    TEST_ASSERT_EQUAL_UINT32(0u, response->stats.unit);
    TEST_ASSERT_EQUAL_UINT64(0u, response->stats.sample.value);
    TEST_ASSERT_EQUAL_UINT64(0u, response->stats.sample.epoch);
    TEST_ASSERT_FALSE(response->stats.sample.available);
    TEST_ASSERT_EQUAL_UINT(0u, response->value_length);
}

static void expect_invalid_request(const esy_remote_manager_request_t *request) {
    const unsigned authorization_calls = s_ctx.authorization_calls;
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(request, &response));
    TEST_ASSERT_EQUAL_UINT(authorization_calls, s_ctx.authorization_calls);
    assert_empty_stats(&response);
}

static void expect_invalid_snapshot(const esy_remote_manager_request_t *request) {
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(request, &response));
    assert_empty_stats(&response);
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.snapshot = (esy_remote_manager_stats_snapshot_t){
        .count = 4u, .index = 1u, .name = "comms.rx_bytes",
        .name_length = sizeof("comms.rx_bytes") - 1u,
        .kind = ESY_REMOTE_MANAGER_STATS_COUNTER,
        .unit = ESY_REMOTE_MANAGER_STATS_UNIT_BYTES,
        .sample = {.value = UINT64_C(0xfedcba9876543210),
                   .epoch = UINT64_C(0x1020304050607080), .available = true}};
    s_ctx.config = (esy_remote_manager_config_t){
        .max_key_length = ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES,
        .max_request_value_length = ESY_REMOTE_MANAGER_STATS_INDEX_BYTES,
        .max_response_value_length = 0u,
        .authorize = authorize, .authorize_context = &s_ctx,
        .stats_ops = {.enumerate = enumerate, .read = read_stat, .context = &s_ctx}};
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}

void test_stats_discovery_reports_each_installed_operation_without_sampling(void) {
    initialize();
    const esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_REMOTE_MANAGER_CAP_DISCOVER |
        ESY_REMOTE_MANAGER_CAP_STATS_ENUMERATE |
        ESY_REMOTE_MANAGER_CAP_STATS_READ, response.capabilities);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumeration_calls + s_ctx.read_calls);
}

void test_stats_discovery_does_not_advertise_a_missing_read_callback(void) {
    s_ctx.config.stats_ops.read = NULL;
    initialize();
    const esy_remote_manager_request_t discover = {
        .group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&discover, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_REMOTE_MANAGER_CAP_DISCOVER |
        ESY_REMOTE_MANAGER_CAP_STATS_ENUMERATE, response.capabilities);
    const esy_remote_manager_request_t read = request_for(ESY_REMOTE_MANAGER_STATS_READ);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&read, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumeration_calls + s_ctx.read_calls);
}

void test_stats_enumeration_preserves_64_bit_samples_without_output_storage(void) {
    initialize();
    const esy_remote_manager_request_t request =
        request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(4u, response.stats.count);
    TEST_ASSERT_EQUAL_UINT32(1u, response.stats.index);
    TEST_ASSERT_EQUAL_STRING("comms.rx_bytes", response.stats.name);
    TEST_ASSERT_EQUAL_UINT(sizeof("comms.rx_bytes") - 1u, response.stats.name_length);
    TEST_ASSERT_EQUAL(ESY_REMOTE_MANAGER_STATS_COUNTER, response.stats.kind);
    TEST_ASSERT_EQUAL(ESY_REMOTE_MANAGER_STATS_UNIT_BYTES, response.stats.unit);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0xfedcba9876543210), response.stats.sample.value);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x1020304050607080), response.stats.sample.epoch);
    TEST_ASSERT_TRUE(response.stats.sample.available);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.last_index);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.enumeration_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.read_calls);
    TEST_ASSERT_EQUAL_UINT(0u, response.value_length);
}

void test_stats_read_uses_the_exact_bounded_name_and_returns_a_typed_gauge(void) {
    s_ctx.config.max_request_value_length = 0u;
    s_ctx.snapshot.kind = ESY_REMOTE_MANAGER_STATS_GAUGE;
    s_ctx.snapshot.unit = ESY_REMOTE_MANAGER_STATS_UNIT_WORDS;
    s_ctx.snapshot.sample.value = UINT64_MAX;
    s_ctx.snapshot.sample.epoch = 0u;
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_STATS_READ);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_STRING("comms.rx_bytes", s_ctx.last_name);
    TEST_ASSERT_EQUAL_UINT(request.key_length, s_ctx.last_name_length);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, response.stats.sample.value);
    TEST_ASSERT_EQUAL_UINT64(0u, response.stats.sample.epoch);
    TEST_ASSERT_EQUAL(ESY_REMOTE_MANAGER_STATS_GAUGE, response.stats.kind);
    TEST_ASSERT_EQUAL(ESY_REMOTE_MANAGER_STATS_UNIT_WORDS, response.stats.unit);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.read_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumeration_calls);
}

void test_stats_names_support_all_canonical_punctuation_and_the_exact_maximum_length(void) {
    memset(s_ctx.snapshot.name, 'a', ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES);
    s_ctx.snapshot.name[0] = 'Z';
    s_ctx.snapshot.name[1] = '0';
    s_ctx.snapshot.name[2] = '_';
    s_ctx.snapshot.name[3] = '.';
    s_ctx.snapshot.name[4] = '-';
    s_ctx.snapshot.name[5] = '/';
    s_ctx.snapshot.name[ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES] = '\0';
    s_ctx.snapshot.name_length = ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES;
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_STATS_READ);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES, response.stats.name_length);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.snapshot.name, response.stats.name,
                             ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES + 1u);
}

void test_stats_empty_inventory_is_an_all_zero_enumeration_result_at_index_zero(void) {
    s_ctx.snapshot = (esy_remote_manager_stats_snapshot_t){0};
    initialize();
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    assert_empty_stats(&response);
    request.stat_index = 1u;
    expect_invalid_snapshot(&request);
    request.stat_index = 0u;
    s_ctx.snapshot.sample.epoch = 1u;
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = (esy_remote_manager_stats_snapshot_t){0};
    s_ctx.snapshot.name[0] = 'x';
    expect_invalid_snapshot(&request);
}

void test_stats_unavailable_samples_are_distinct_from_an_available_zero_value(void) {
    s_ctx.snapshot.sample.value = 0u;
    s_ctx.snapshot.sample.available = false;
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_FALSE(response.stats.sample.available);
    TEST_ASSERT_EQUAL_UINT64(s_ctx.snapshot.sample.epoch, response.stats.sample.epoch);
    s_ctx.snapshot.sample.available = true;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_TRUE(response.stats.sample.available);
    s_ctx.snapshot.sample.available = false;
    s_ctx.snapshot.sample.value = 1u;
    expect_invalid_snapshot(&request);
}

void test_stats_percent_values_have_explicit_bounds_and_trailing_name_storage_is_ignored(void) {
    s_ctx.snapshot.kind = ESY_REMOTE_MANAGER_STATS_GAUGE;
    s_ctx.snapshot.unit = ESY_REMOTE_MANAGER_STATS_UNIT_PERCENT;
    s_ctx.snapshot.sample.value = 100u;
    s_ctx.snapshot.name[ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES] = 'x';
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT64(100u, response.stats.sample.value);
    s_ctx.snapshot.sample.value = 101u;
    expect_invalid_snapshot(&request);
    s_ctx.snapshot.sample.value = 0u;
    s_ctx.snapshot.sample.available = false;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_FALSE(response.stats.sample.available);
}

void test_stats_authorization_precedes_each_sampling_callback(void) {
    s_ctx.authorization = ESY_ERR_FAIL;
    initialize();
    esy_remote_manager_response_t response;
    for (uint16_t command = ESY_REMOTE_MANAGER_STATS_ENUMERATE;
         command <= ESY_REMOTE_MANAGER_STATS_READ; ++command) {
        const esy_remote_manager_request_t request = request_for(command);
        TEST_ASSERT_EQUAL(ESY_ERR_FAIL, dispatch(&request, &response));
        assert_empty_stats(&response);
    }
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumeration_calls + s_ctx.read_calls);
}

void test_stats_enumeration_requires_its_full_normalized_index_budget(void) {
    s_ctx.config.max_request_value_length = ESY_REMOTE_MANAGER_STATS_INDEX_BYTES - 1u;
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    expect_invalid_request(&request);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumeration_calls);
}

void test_stats_reject_unused_fields_and_invalid_names_before_authorization(void) {
    initialize();
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    request.key = "x";
    request.key_length = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_READ);
    request.stat_index = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_READ);
    request.key_length = 0u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_READ);
    request.key = "space here";
    request.key_length = sizeof("space here") - 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_READ);
    request.key = "embedded\0nul";
    request.key_length = sizeof("embedded\0nul") - 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_READ);
    request.value = (const uint8_t *)"x";
    request.value_length = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    request.session_id = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    request.offset = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    request.length = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    request.slot_index = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    request.image.slot = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    request.manifest.version = 1u;
    expect_invalid_request(&request);
    request = (esy_remote_manager_request_t){
        .group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, .stat_index = 1u};
    expect_invalid_request(&request);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumeration_calls + s_ctx.read_calls);
}

void test_stats_provider_errors_clear_the_result_and_are_never_retried(void) {
    s_ctx.provider_result = ESY_ERR_NOT_SUPPORTED;
    initialize();
    esy_remote_manager_response_t response;
    for (uint16_t command = ESY_REMOTE_MANAGER_STATS_ENUMERATE;
         command <= ESY_REMOTE_MANAGER_STATS_READ; ++command) {
        const esy_remote_manager_request_t request = request_for(command);
        memset(&response, 0xa5, sizeof(response));
        TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
        assert_empty_stats(&response);
    }
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.enumeration_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.read_calls);
}

void test_stats_reject_inconsistent_enumeration_names_bounds_and_kinds(void) {
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_STATS_ENUMERATE);
    const esy_remote_manager_stats_snapshot_t original = s_ctx.snapshot;
    s_ctx.snapshot.index = s_ctx.snapshot.count;
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = original;
    s_ctx.snapshot.index = 0u;
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = original;
    s_ctx.snapshot.name_length = 0u;
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = original;
    s_ctx.snapshot.name_length = ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES + 1u;
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = original;
    s_ctx.snapshot.name[0] = ' ';
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = original;
    s_ctx.snapshot.name[0] = (char)0x80;
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = original;
    s_ctx.snapshot.name[original.name_length] = 'x';
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = original;
    s_ctx.snapshot.kind = (esy_remote_manager_stats_kind_t)0;
    expect_invalid_snapshot(&request);
    s_ctx.snapshot = original;
    s_ctx.snapshot.unit = (esy_remote_manager_stats_unit_t)6;
    expect_invalid_snapshot(&request);
}

void test_stats_reads_reject_a_provider_result_for_another_name(void) {
    initialize();
    const char requested[] = "system.uptime_ms";
    const esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_STATS,
        .command = ESY_REMOTE_MANAGER_STATS_READ,
        .key = requested, .key_length = sizeof(requested) - 1u};
    expect_invalid_snapshot(&request);
    TEST_ASSERT_EQUAL_STRING(requested, s_ctx.last_name);
    s_ctx.snapshot = (esy_remote_manager_stats_snapshot_t){0};
    expect_invalid_snapshot(&request);
}

void test_stats_unsupported_callbacks_and_commands_have_no_sampling_side_effects(void) {
    s_ctx.config.stats_ops = (esy_remote_manager_stats_ops_t){0};
    initialize();
    esy_remote_manager_response_t response;
    for (uint16_t command = ESY_REMOTE_MANAGER_STATS_ENUMERATE;
         command <= ESY_REMOTE_MANAGER_STATS_READ; ++command) {
        const esy_remote_manager_request_t request = request_for(command);
        TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
    }
    const esy_remote_manager_request_t unknown = {
        .group = ESY_REMOTE_MANAGER_GROUP_STATS, .command = UINT16_MAX};
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&unknown, &response));
    const esy_remote_manager_request_t discover = {
        .group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER};
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&discover, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_REMOTE_MANAGER_CAP_DISCOVER, response.capabilities);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.enumeration_calls + s_ctx.read_calls);
}
