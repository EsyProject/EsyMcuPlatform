/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"

#include <string.h>

#define TEST_KEY_CAPACITY 8u
#define TEST_VALUE_CAPACITY 16u

typedef struct {
    esy_remote_manager_t manager;
    esy_remote_manager_config_t config;
    uint8_t value[TEST_VALUE_CAPACITY];
    size_t value_length;
    bool found;
    char last_key[TEST_KEY_CAPACITY];
    size_t last_key_length;
    size_t last_get_capacity;
    unsigned authorize_calls;
    unsigned get_calls;
    unsigned set_calls;
    unsigned remove_calls;
    unsigned apply_calls;
    unsigned save_calls;
    esy_err_t get_result;
    esy_err_t set_result;
    esy_err_t remove_result;
    esy_err_t apply_result;
    esy_err_t save_result;
    bool malformed_get;
    size_t claimed_length;
    bool claimed_found;
    bool reenter_set;
    esy_err_t nested_result;
} fixture_ctx_t;

static fixture_ctx_t s_ctx;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request)
{
    fixture_ctx_t *ctx = context;
    (void)request;
    ctx->authorize_calls++;
    return ESY_OK;
}

static void record_key(fixture_ctx_t *ctx, const char *key, size_t length)
{
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof(ctx->last_key), length);
    memcpy(ctx->last_key, key, length);
    ctx->last_key_length = length;
}

static esy_err_t get_value(void *context, const char *key, size_t key_length,
                           uint8_t *output, size_t capacity, size_t *length,
                           bool *found)
{
    fixture_ctx_t *ctx = context;
    ctx->get_calls++;
    ctx->last_get_capacity = capacity;
    record_key(ctx, key, key_length);
    if (ctx->malformed_get) {
        *length = ctx->claimed_length;
        *found = ctx->claimed_found;
        return ctx->get_result;
    }
    *found = ctx->found;
    *length = ctx->found ? ctx->value_length : 0;
    if (ctx->get_result != ESY_OK) return ctx->get_result;
    if (*length > capacity) return ESY_ERR_NO_MEM;
    if (*length != 0) memcpy(output, ctx->value, *length);
    return ESY_OK;
}

static esy_err_t set_value(void *context, const char *key, size_t key_length,
                           const uint8_t *value, size_t length)
{
    fixture_ctx_t *ctx = context;
    ctx->set_calls++;
    record_key(ctx, key, key_length);
    if (ctx->reenter_set) {
        const esy_remote_manager_request_t nested = {
            .group = ESY_REMOTE_MANAGER_GROUP_CONFIG,
            .command = ESY_REMOTE_MANAGER_CONFIG_SAVE,
        };
        esy_remote_manager_response_t response;
        ctx->nested_result = esy_remote_manager_dispatch(
            &ctx->manager, &nested, NULL, 0, &response);
    }
    if (ctx->set_result == ESY_OK) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof(ctx->value), length);
        if (length != 0) memcpy(ctx->value, value, length);
        ctx->value_length = length;
        ctx->found = true;
    }
    return ctx->set_result;
}

static esy_err_t remove_value(void *context, const char *key, size_t key_length)
{
    fixture_ctx_t *ctx = context;
    ctx->remove_calls++;
    record_key(ctx, key, key_length);
    if (ctx->remove_result == ESY_OK) {
        ctx->found = false;
        ctx->value_length = 0;
    }
    return ctx->remove_result;
}

static esy_err_t apply_value(void *context)
{
    fixture_ctx_t *ctx = context;
    ctx->apply_calls++;
    return ctx->apply_result;
}

static esy_err_t save_value(void *context)
{
    fixture_ctx_t *ctx = context;
    ctx->save_calls++;
    return ctx->save_result;
}

void setUp(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config = (esy_remote_manager_config_t){
        .max_key_length = TEST_KEY_CAPACITY,
        .max_request_value_length = TEST_VALUE_CAPACITY, .max_response_value_length = TEST_VALUE_CAPACITY,
        .authorize = authorize,
        .authorize_context = &s_ctx,
        .config_ops = {
            .get = get_value, .set = set_value, .remove = remove_value,
            .apply = apply_value, .save = save_value, .context = &s_ctx,
        },
    };
}

void tearDown(void)
{
}

static void initialize(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager,
                                                     &s_ctx.config));
}

static esy_remote_manager_request_t key_request(uint16_t command)
{
    return (esy_remote_manager_request_t){
        .group = ESY_REMOTE_MANAGER_GROUP_CONFIG,
        .command = command,
        .key = "network", .key_length = 7,
    };
}

static esy_remote_manager_request_t control_request(uint16_t command)
{
    return (esy_remote_manager_request_t){
        .group = ESY_REMOTE_MANAGER_GROUP_CONFIG, .command = command,
    };
}

static void assert_result(esy_err_t expected,
                          const esy_remote_manager_request_t *request)
{
    esy_remote_manager_response_t response = {
        .status = 1, .capabilities = UINT32_MAX, .value_length = 10, .found = true,
    };
    TEST_ASSERT_EQUAL(expected, esy_remote_manager_dispatch(
        &s_ctx.manager, request, NULL, 0, &response));
    TEST_ASSERT_EQUAL(expected, response.status);
    TEST_ASSERT_EQUAL_UINT32(0, response.capabilities);
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
    TEST_ASSERT_FALSE(response.found);
}

void test_config_get_copies_binary_values_with_nonterminated_keys(void)
{
    const char key[] = {'n', 'e', 't'};
    const uint8_t expected[] = {0x00, 0xff, 0x80, 0x01};
    uint8_t output[sizeof(expected)] = {0};
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    esy_remote_manager_response_t response;
    memcpy(s_ctx.value, expected, sizeof(expected));
    s_ctx.value_length = sizeof(expected);
    s_ctx.found = true;
    request.key = key;
    request.key_length = sizeof(key);
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, output, sizeof(output), &response));
    TEST_ASSERT_EQUAL(ESY_OK, response.status);
    TEST_ASSERT_TRUE(response.found);
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), response.value_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, output, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(sizeof(key), s_ctx.last_key_length);
    TEST_ASSERT_EQUAL_MEMORY(key, s_ctx.last_key, sizeof(key));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.get_calls);
}

void test_config_get_distinguishes_absence_from_empty_value_without_output_storage(void)
{
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    esy_remote_manager_response_t response;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_FALSE(response.found);
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
    s_ctx.found = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_TRUE(response.found);
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
    TEST_ASSERT_EQUAL_UINT(2, s_ctx.get_calls);
}

void test_config_get_reports_required_capacity_without_retrying_provider(void)
{
    uint8_t output[2];
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    esy_remote_manager_response_t response;
    s_ctx.found = true;
    s_ctx.value_length = 3;
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, output, sizeof(output), &response));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, response.status);
    TEST_ASSERT_TRUE(response.found);
    TEST_ASSERT_EQUAL_UINT(3, response.value_length);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.get_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.authorize_calls);
}

void test_config_get_limits_provider_capacity_to_configured_value_budget(void)
{
    uint8_t output[32];
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    esy_remote_manager_response_t response;
    s_ctx.config.max_request_value_length = 4; s_ctx.config.max_response_value_length = 4;
    s_ctx.found = true;
    s_ctx.value_length = 4;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, output, sizeof(output), &response));
    TEST_ASSERT_EQUAL_UINT(4, s_ctx.last_get_capacity);
    TEST_ASSERT_EQUAL_UINT(4, response.value_length);
}

void test_config_set_accepts_binary_and_empty_values_without_implicit_apply_or_save(void)
{
    const char key[TEST_KEY_CAPACITY] = {'c', 'a', 'l', 'i', 'b', 'r', 'a', 't'};
    const uint8_t value[TEST_VALUE_CAPACITY] = {0, 0xff, 1, 0x80};
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_SET);
    request.key = key;
    request.key_length = sizeof(key);
    request.value = value;
    request.value_length = sizeof(value);
    initialize();
    assert_result(ESY_OK, &request);
    TEST_ASSERT_EQUAL_UINT(sizeof(key), s_ctx.last_key_length);
    TEST_ASSERT_EQUAL_MEMORY(key, s_ctx.last_key, sizeof(key));
    TEST_ASSERT_EQUAL_UINT(sizeof(value), s_ctx.value_length);
    TEST_ASSERT_EQUAL_MEMORY(value, s_ctx.value, sizeof(value));
    TEST_ASSERT_TRUE(s_ctx.found);
    request.value = NULL;
    request.value_length = 0;
    assert_result(ESY_OK, &request);
    TEST_ASSERT_TRUE(s_ctx.found);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.value_length);
    TEST_ASSERT_EQUAL_UINT(2, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.get_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.remove_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.apply_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.save_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.apply_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.save_calls);
}

void test_config_zero_value_budget_allows_only_empty_values(void)
{
    const uint8_t value = 1;
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_SET);
    s_ctx.config.max_request_value_length = 0; s_ctx.config.max_response_value_length = 0;
    initialize();
    assert_result(ESY_OK, &request);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.set_calls);
    request.value = &value;
    request.value_length = 1;
    assert_result(ESY_ERR_INVALID_ARG, &request);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.authorize_calls);
    request.command = ESY_REMOTE_MANAGER_CONFIG_GET;
    request.value = NULL;
    request.value_length = 0;
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_TRUE(response.found);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.last_get_capacity);
}

void test_config_remove_calls_only_remove_and_preserves_explicit_control_steps(void)
{
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_REMOVE);
    s_ctx.found = true;
    s_ctx.value_length = 2;
    initialize();
    assert_result(ESY_OK, &request);
    TEST_ASSERT_FALSE(s_ctx.found);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.remove_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.get_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.apply_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.save_calls);
}

void test_config_apply_and_save_are_distinct_explicit_operations(void)
{
    esy_remote_manager_request_t request = control_request(ESY_REMOTE_MANAGER_CONFIG_APPLY);
    initialize();
    assert_result(ESY_OK, &request);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.apply_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.save_calls);
    request.command = ESY_REMOTE_MANAGER_CONFIG_SAVE;
    assert_result(ESY_OK, &request);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.apply_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.save_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.get_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.remove_calls);
}

void test_config_propagates_each_provider_error_and_clears_nonread_response_fields(void)
{
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    s_ctx.get_result = ESY_ERR_IO;
    s_ctx.set_result = ESY_ERR_CRC_FAIL;
    s_ctx.remove_result = ESY_ERR_BUSY;
    s_ctx.apply_result = ESY_ERR_TIMEOUT;
    s_ctx.save_result = ESY_ERR_FAIL;
    s_ctx.found = true;
    s_ctx.value_length = 3;
    initialize();
    assert_result(ESY_ERR_IO, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_SET;
    assert_result(ESY_ERR_CRC_FAIL, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_REMOVE;
    assert_result(ESY_ERR_BUSY, &request);
    request = control_request(ESY_REMOTE_MANAGER_CONFIG_APPLY);
    assert_result(ESY_ERR_TIMEOUT, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_SAVE;
    assert_result(ESY_ERR_FAIL, &request);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.get_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.remove_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.apply_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.save_calls);
}

void test_config_rejects_positive_provider_statuses_as_contract_violations(void)
{
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    s_ctx.get_result = 1;
    s_ctx.set_result = 1;
    s_ctx.remove_result = 1;
    s_ctx.apply_result = 1;
    s_ctx.save_result = 1;
    initialize();
    assert_result(ESY_ERR_STATE, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_SET;
    assert_result(ESY_ERR_STATE, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_REMOVE;
    assert_result(ESY_ERR_STATE, &request);
    request = control_request(ESY_REMOTE_MANAGER_CONFIG_APPLY);
    assert_result(ESY_ERR_STATE, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_SAVE;
    assert_result(ESY_ERR_STATE, &request);
}

void test_config_absent_operations_are_independently_unsupported(void)
{
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    s_ctx.config.config_ops = (esy_remote_manager_config_ops_t){0};
    initialize();
    assert_result(ESY_ERR_NOT_SUPPORTED, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_SET;
    assert_result(ESY_ERR_NOT_SUPPORTED, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_REMOVE;
    assert_result(ESY_ERR_NOT_SUPPORTED, &request);
    request = control_request(ESY_REMOTE_MANAGER_CONFIG_APPLY);
    assert_result(ESY_ERR_NOT_SUPPORTED, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_SAVE;
    assert_result(ESY_ERR_NOT_SUPPORTED, &request);
    TEST_ASSERT_EQUAL_UINT(5, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.get_calls + s_ctx.set_calls + s_ctx.remove_calls +
                                  s_ctx.apply_calls + s_ctx.save_calls);
}

void test_config_get_rejects_success_with_length_beyond_output_capacity(void)
{
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    s_ctx.malformed_get = true;
    s_ctx.claimed_length = 1;
    s_ctx.claimed_found = true;
    initialize();
    assert_result(ESY_ERR_STATE, &request);
}

void test_config_get_rejects_missing_value_with_nonzero_length(void)
{
    uint8_t output[2];
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    esy_remote_manager_response_t response;
    s_ctx.malformed_get = true;
    s_ctx.claimed_length = 1;
    s_ctx.claimed_found = false;
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, output, sizeof(output), &response));
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
    TEST_ASSERT_FALSE(response.found);
}

void test_config_get_rejects_no_memory_without_a_found_oversized_value(void)
{
    uint8_t output[2];
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    esy_remote_manager_response_t response;
    s_ctx.malformed_get = true;
    s_ctx.get_result = ESY_ERR_NO_MEM;
    s_ctx.claimed_length = 1;
    s_ctx.claimed_found = false;
    initialize();
    assert_result(ESY_ERR_STATE, &request);
    s_ctx.claimed_length = 0;
    s_ctx.claimed_found = true;
    assert_result(ESY_ERR_STATE, &request);
    s_ctx.claimed_length = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, output, sizeof(output), &response));
    TEST_ASSERT_FALSE(response.found);
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
}

void test_config_get_rejects_reported_length_beyond_configured_value_budget(void)
{
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_GET);
    s_ctx.malformed_get = true;
    s_ctx.get_result = ESY_ERR_NO_MEM;
    s_ctx.claimed_length = TEST_VALUE_CAPACITY + 1;
    s_ctx.claimed_found = true;
    initialize();
    assert_result(ESY_ERR_STATE, &request);
    s_ctx.get_result = ESY_OK;
    assert_result(ESY_ERR_STATE, &request);
}

void test_config_provider_callback_cannot_reenter_to_save_changes(void)
{
    esy_remote_manager_request_t request = key_request(ESY_REMOTE_MANAGER_CONFIG_SET);
    s_ctx.reenter_set = true;
    initialize();
    assert_result(ESY_OK, &request);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.nested_result);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.set_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.save_calls);
    request = control_request(ESY_REMOTE_MANAGER_CONFIG_SAVE);
    assert_result(ESY_OK, &request);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.save_calls);
}
