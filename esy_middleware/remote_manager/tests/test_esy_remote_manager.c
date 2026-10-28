/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"

#include <string.h>

typedef struct {
    esy_remote_manager_t manager;
    esy_remote_manager_config_t config;
    esy_err_t authorize_result;
    unsigned authorize_calls;
    unsigned operation_calls;
    uint32_t last_request_id;
    bool reenter;
    esy_err_t nested_dispatch_result;
    esy_err_t nested_init_result;
    esy_err_t nested_deinit_result;
} fixture_ctx_t;

static fixture_ctx_t s_ctx;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request)
{
    fixture_ctx_t *ctx = context;
    ctx->authorize_calls++;
    ctx->last_request_id = request->request_id;
    if (ctx->reenter) {
        esy_remote_manager_response_t response;
        ctx->nested_dispatch_result = esy_remote_manager_dispatch(
            &ctx->manager, request, NULL, 0, &response);
        ctx->nested_init_result = esy_remote_manager_init(&ctx->manager,
                                                          &ctx->config);
        ctx->nested_deinit_result = esy_remote_manager_deinit(&ctx->manager);
    }
    return ctx->authorize_result;
}

static esy_err_t read_value(void *context, const char *key, size_t key_length,
                            uint8_t *output, size_t capacity, size_t *length,
                            bool *found)
{
    fixture_ctx_t *ctx = context;
    (void)key;
    (void)key_length;
    (void)output;
    (void)capacity;
    ctx->operation_calls++;
    *found = false;
    *length = 0;
    return ESY_OK;
}

static esy_err_t set_value(void *context, const char *key, size_t key_length,
                           const uint8_t *value, size_t length)
{
    fixture_ctx_t *ctx = context;
    (void)key;
    (void)key_length;
    (void)value;
    (void)length;
    ctx->operation_calls++;
    return ESY_OK;
}

static esy_err_t remove_value(void *context, const char *key, size_t key_length)
{
    fixture_ctx_t *ctx = context;
    (void)key;
    (void)key_length;
    ctx->operation_calls++;
    return ESY_OK;
}

static esy_err_t control_value(void *context)
{
    fixture_ctx_t *ctx = context;
    ctx->operation_calls++;
    return ESY_OK;
}

void setUp(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config = (esy_remote_manager_config_t){
        .max_key_length = 8,
        .max_request_value_length = 16, .max_response_value_length = 16,
        .authorize = authorize,
        .authorize_context = &s_ctx,
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

static esy_remote_manager_request_t discover_request(void)
{
    return (esy_remote_manager_request_t){
        .request_id = UINT32_MAX,
        .group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER,
    };
}

static void assert_dispatched(esy_err_t expected,
                              const esy_remote_manager_request_t *request)
{
    esy_remote_manager_response_t response = {
        .status = 1, .capabilities = UINT32_MAX, .value_length = 7, .found = true,
    };
    TEST_ASSERT_EQUAL(expected, esy_remote_manager_dispatch(
        &s_ctx.manager, request, NULL, 0, &response));
    TEST_ASSERT_EQUAL(expected, response.status);
    if (expected != ESY_OK) {
        TEST_ASSERT_EQUAL_UINT32(0, response.capabilities);
        TEST_ASSERT_EQUAL_UINT(0, response.value_length);
        TEST_ASSERT_FALSE(response.found);
    }
}

void test_manager_init_validates_required_arguments_and_preserves_failed_storage(void)
{
    esy_remote_manager_t initial = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_remote_manager_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_remote_manager_init(&s_ctx.manager, NULL));
    s_ctx.config.authorize = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_remote_manager_init(&s_ctx.manager, &s_ctx.config));
    TEST_ASSERT_EQUAL_MEMORY(&initial, &s_ctx.manager, sizeof(initial));
    s_ctx.config.authorize = authorize;
    s_ctx.config.max_key_length = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                      esy_remote_manager_init(&s_ctx.manager, &s_ctx.config));
    TEST_ASSERT_EQUAL_MEMORY(&initial, &s_ctx.manager, sizeof(initial));
    s_ctx.config.max_key_length = 8;
    initialize();
}

void test_manager_lifecycle_copies_config_and_allows_deinit_and_reinit(void)
{
    esy_remote_manager_request_t request = discover_request();
    esy_remote_manager_response_t response;
    esy_remote_manager_t zero = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_deinit(NULL));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
                      esy_remote_manager_init(&s_ctx.manager, &s_ctx.config));
    s_ctx.config.authorize = NULL;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.manager, sizeof(zero));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    assert_dispatched(ESY_ERR_STATE, &request);
    s_ctx.config.authorize = authorize;
    initialize();
    assert_dispatched(ESY_OK, &request);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.operation_calls);
}

void test_manager_discovery_works_without_any_configuration_provider(void)
{
    esy_remote_manager_request_t request = discover_request();
    esy_remote_manager_response_t response;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL(ESY_OK, response.status);
    TEST_ASSERT_EQUAL_UINT32(ESY_REMOTE_MANAGER_CAP_DISCOVER, response.capabilities);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s_ctx.last_request_id);
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
    TEST_ASSERT_FALSE(response.found);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.operation_calls);
}

void test_manager_capabilities_reflect_only_supplied_provider_operations(void)
{
    esy_remote_manager_request_t request = discover_request();
    esy_remote_manager_response_t response;
    s_ctx.config.config_ops = (esy_remote_manager_config_ops_t){
        .get = read_value, .set = set_value, .remove = remove_value,
        .apply = control_value, .save = control_value, .context = &s_ctx,
    };
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT32(
        ESY_REMOTE_MANAGER_CAP_DISCOVER | ESY_REMOTE_MANAGER_CAP_CONFIG_GET |
        ESY_REMOTE_MANAGER_CAP_CONFIG_SET | ESY_REMOTE_MANAGER_CAP_CONFIG_REMOVE |
        ESY_REMOTE_MANAGER_CAP_CONFIG_APPLY | ESY_REMOTE_MANAGER_CAP_CONFIG_SAVE,
        response.capabilities);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.operation_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    s_ctx.config.config_ops = (esy_remote_manager_config_ops_t){
        .set = set_value, .save = control_value, .context = &s_ctx,
    };
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_REMOTE_MANAGER_CAP_DISCOVER |
        ESY_REMOTE_MANAGER_CAP_CONFIG_SET | ESY_REMOTE_MANAGER_CAP_CONFIG_SAVE,
        response.capabilities);
}

void test_manager_dispatch_validates_output_and_resets_response_on_failures(void)
{
    esy_remote_manager_request_t request = discover_request();
    esy_remote_manager_response_t response = {
        .status = 1, .capabilities = 7, .value_length = 9, .found = true,
    };
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_dispatch(
        NULL, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response.status);
    TEST_ASSERT_EQUAL_UINT32(0, response.capabilities);
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
    TEST_ASSERT_FALSE(response.found);
    assert_dispatched(ESY_ERR_INVALID_ARG, NULL);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 1, &response));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.authorize_calls);
}

void test_manager_rejects_invalid_key_and_value_spans_before_authorization(void)
{
    const char embedded_nul[] = {'a', '\0', 'b'};
    const uint8_t value[17] = {0};
    esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_CONFIG,
        .command = ESY_REMOTE_MANAGER_CONFIG_SET,
        .key = "key", .key_length = 3,
    };
    initialize();
    request.key = NULL;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.key = "123456789";
    request.key_length = 9;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.key = embedded_nul;
    request.key_length = sizeof(embedded_nul);
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.key = "key";
    request.key_length = 0;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.key_length = 3;
    request.value_length = 1;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.value = value;
    request.value_length = sizeof(value);
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.operation_calls);
}

void test_manager_rejects_unused_known_command_arguments_before_authorization(void)
{
    const uint8_t value = 1;
    esy_remote_manager_request_t request = discover_request();
    initialize();
    request.key = "x";
    request.key_length = 1;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.key = NULL;
    request.key_length = 0;
    request.value = &value;
    request.value_length = 1;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.group = ESY_REMOTE_MANAGER_GROUP_CONFIG;
    request.command = ESY_REMOTE_MANAGER_CONFIG_GET;
    request.key = "x";
    request.key_length = 1;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_REMOVE;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_APPLY;
    request.value_length = 0;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_SAVE;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_GET;
    request.key_length = 0;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_APPLY;
    request.value_length = 1;
    assert_dispatched(ESY_ERR_INVALID_ARG, &request);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.authorize_calls);
}

void test_manager_authorization_denies_without_provider_calls_and_can_recover(void)
{
    esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_CONFIG,
        .command = ESY_REMOTE_MANAGER_CONFIG_SET,
        .key = "key", .key_length = 3,
    };
    s_ctx.config.config_ops.set = set_value;
    s_ctx.config.config_ops.context = &s_ctx;
    initialize();
    s_ctx.authorize_result = ESY_ERR_IO;
    assert_dispatched(ESY_ERR_IO, &request);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.operation_calls);
    s_ctx.authorize_result = 1;
    assert_dispatched(ESY_ERR_STATE, &request);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.operation_calls);
    s_ctx.authorize_result = ESY_OK;
    assert_dispatched(ESY_OK, &request);
    TEST_ASSERT_EQUAL_UINT(3, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.operation_calls);
}

void test_manager_unknown_groups_commands_and_absent_operations_are_unsupported(void)
{
    esy_remote_manager_request_t request = discover_request();
    initialize();
    request.group = 3u;
    assert_dispatched(ESY_ERR_NOT_SUPPORTED, &request);
    request.group = UINT16_MAX;
    assert_dispatched(ESY_ERR_NOT_SUPPORTED, &request);
    request.group = ESY_REMOTE_MANAGER_GROUP_SYSTEM;
    request.command = UINT16_MAX;
    assert_dispatched(ESY_ERR_NOT_SUPPORTED, &request);
    request.group = ESY_REMOTE_MANAGER_GROUP_CONFIG;
    assert_dispatched(ESY_ERR_NOT_SUPPORTED, &request);
    request.command = ESY_REMOTE_MANAGER_CONFIG_GET;
    request.key = "key";
    request.key_length = 3;
    assert_dispatched(ESY_ERR_NOT_SUPPORTED, &request);
    TEST_ASSERT_EQUAL_UINT(5, s_ctx.authorize_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.operation_calls);
}

void test_manager_busy_guard_rejects_callback_reentry_and_preserves_live_handle(void)
{
    esy_remote_manager_request_t request = discover_request();
    initialize();
    s_ctx.reenter = true;
    assert_dispatched(ESY_OK, &request);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.nested_dispatch_result);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.nested_init_result);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.nested_deinit_result);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.authorize_calls);
    s_ctx.reenter = false;
    assert_dispatched(ESY_OK, &request);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}
