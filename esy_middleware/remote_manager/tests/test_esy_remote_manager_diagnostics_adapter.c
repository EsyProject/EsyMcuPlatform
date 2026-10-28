/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_diagnostics_adapter.h"
#include "esy_middleware/diagnostics/esy_diagnostics.h"
#include "unity.h"

#include <string.h>

typedef struct {
    esy_diagnostics_t core;
    esy_diagnostics_entry_t entries[2];
    esy_diagnostics_static_resources_t resources;
    esy_remote_manager_diagnostics_adapter_t adapter;
    esy_remote_manager_diagnostics_adapter_config_t binding;
    esy_remote_manager_diagnostics_ops_t ops;
    esy_remote_manager_t manager;
    unsigned reads, runs, effects;
    bool authorized, reenter;
    esy_err_t run_result;
} diagnostics_adapter_test_ctx_t;
static diagnostics_adapter_test_ctx_t s_ctx;

static void check_reentry(diagnostics_adapter_test_ctx_t *ctx) {
    if (!ctx->reenter) return;
    esy_remote_manager_diagnostics_ops_t output = {.context = ctx};
    esy_diagnostics_snapshot_t nested;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_remote_manager_diagnostics_adapter_init(&ctx->adapter, &ctx->binding));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_remote_manager_diagnostics_adapter_deinit(&ctx->adapter));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_remote_manager_diagnostics_adapter_get_ops(&ctx->adapter, &output));
    TEST_ASSERT_EQUAL_PTR(ctx, output.context);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, ctx->ops.enumerate(ctx->ops.context, 0u, &nested));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, ctx->ops.read(ctx->ops.context, "board.bus", 9u, &nested));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, ctx->ops.run(ctx->ops.context, "board.bus", 9u, &nested));
}
static esy_err_t read_health(void *context, esy_diagnostics_result_t *result) {
    diagnostics_adapter_test_ctx_t *ctx = context;
    ++ctx->reads;
    check_reentry(ctx);
    *result = (esy_diagnostics_result_t){.health = ESY_DIAGNOSTICS_HEALTH_OK};
    return ESY_OK;
}
static esy_err_t self_test(void *context, esy_diagnostics_result_t *result) {
    diagnostics_adapter_test_ctx_t *ctx = context;
    ++ctx->runs; ++ctx->effects;
    check_reentry(ctx);
    *result = (esy_diagnostics_result_t){.health = ESY_DIAGNOSTICS_HEALTH_ERROR,
        .code = 7u, .detail_length = 4u};
    memcpy(result->detail, "open", 5u);
    return ctx->run_result;
}
static esy_err_t authorize(void *context, const esy_remote_manager_request_t *request) {
    (void)request;
    return ((diagnostics_adapter_test_ctx_t *)context)->authorized ? ESY_OK : ESY_ERR_FAIL;
}
static void bind_manager(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_diagnostics_adapter_init(&s_ctx.adapter, &s_ctx.binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_diagnostics_adapter_get_ops(&s_ctx.adapter, &s_ctx.ops));
    const esy_remote_manager_config_t config = {.max_key_length = 48u,
        .max_request_value_length = 4u, .max_response_value_length = 0u,
        .authorize = authorize, .authorize_context = &s_ctx, .diagnostics_ops = s_ctx.ops};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
}
void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.authorized = true;
    s_ctx.entries[0] = (esy_diagnostics_entry_t){.name = "board.bus", .read = read_health,
        .run = self_test, .context = &s_ctx};
    s_ctx.entries[1] = (esy_diagnostics_entry_t){.name = "clock", .read = read_health, .context = &s_ctx};
    s_ctx.resources = (esy_diagnostics_static_resources_t){s_ctx.entries, 2u};
    const esy_diagnostics_config_t config = {.static_resources = &s_ctx.resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_init(&s_ctx.core, &config));
    s_ctx.binding.diagnostics = &s_ctx.core;
    bind_manager();
}
void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_diagnostics_adapter_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_deinit(&s_ctx.core));
}
static esy_err_t dispatch(uint16_t command, const char *name, size_t length,
                           esy_remote_manager_response_t *response) {
    const esy_remote_manager_request_t request = {.group = ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS,
        .command = command, .key = name, .key_length = length};
    return esy_remote_manager_dispatch(&s_ctx.manager, &request, NULL, 0u, response);
}
void test_diagnostics_adapter_metadata_and_reads_never_start_a_self_test(void) {
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.reads + s_ctx.runs);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE, NULL, 0u, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_DIAGNOSTICS_CAP_READ | ESY_DIAGNOSTICS_CAP_RUN,
        response.diagnostic.capabilities);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.reads + s_ctx.runs);
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ, "board.bus", 9u, &response));
    TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_OK, response.diagnostic.result.health);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.reads);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.runs);
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, "board.bus", 9u, &response));
    TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_ERROR, response.diagnostic.result.health);
    TEST_ASSERT_EQUAL_UINT32(7u, response.diagnostic.result.code);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.runs);
}
void test_diagnostics_adapter_routes_global_run_but_respects_per_source_capabilities(void) {
    esy_remote_manager_response_t response;
    const esy_remote_manager_request_t request = {.group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(&s_ctx.manager, &request, NULL, 0u, &response));
    TEST_ASSERT_NOT_EQUAL(0u, response.capabilities & ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_RUN);
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ, "clock", 5u, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_DIAGNOSTICS_CAP_READ, response.diagnostic.capabilities);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, "clock", 5u, &response));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, "missing", 7u, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.runs + s_ctx.effects);
}
void test_diagnostics_adapter_authorization_and_failed_run_keep_test_effects_explicit(void) {
    esy_remote_manager_response_t response;
    s_ctx.authorized = false;
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, "board.bus", 9u, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.runs);
    s_ctx.authorized = true; s_ctx.run_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, "board.bus", 9u, &response));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.effects);
    TEST_ASSERT_EQUAL_UINT32(0u, response.diagnostic.count);
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ, "board.bus", 9u, &response));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.effects);
}
void test_diagnostics_adapter_rejects_lifecycle_and_operation_reentry_without_detaching(void) {
    s_ctx.reenter = true;
    esy_diagnostics_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.read(s_ctx.ops.context, "board.bus", 9u, &snapshot));
    TEST_ASSERT_FALSE(s_ctx.adapter.busy);
    s_ctx.run_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.ops.run(s_ctx.ops.context, "board.bus", 9u, &snapshot));
    TEST_ASSERT_FALSE(s_ctx.adapter.busy);
    TEST_ASSERT_TRUE(s_ctx.adapter.initialized);
}
void test_diagnostics_adapter_validates_bindings_and_detectable_output_overlap(void) {
    esy_remote_manager_diagnostics_adapter_t zero = {0};
    esy_diagnostics_t detached = {0};
    const esy_remote_manager_diagnostics_adapter_config_t missing = {.diagnostics = &detached};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_diagnostics_adapter_init(NULL, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_diagnostics_adapter_init(&zero, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_remote_manager_diagnostics_adapter_init(&zero, &missing));
    TEST_ASSERT_FALSE(zero.initialized);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_remote_manager_diagnostics_adapter_init(&s_ctx.adapter, &s_ctx.binding));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_diagnostics_adapter_get_ops(NULL, &s_ctx.ops));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_diagnostics_adapter_get_ops(&s_ctx.adapter, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_diagnostics_adapter_get_ops(&s_ctx.adapter,
        (esy_remote_manager_diagnostics_ops_t *)(void *)&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_diagnostics_adapter_get_ops(&s_ctx.adapter,
        (esy_remote_manager_diagnostics_ops_t *)(void *)&s_ctx.core));
}
void test_diagnostics_adapter_detach_preserves_core_and_rejects_saved_detached_operations(void) {
    const esy_remote_manager_diagnostics_ops_t saved = s_ctx.ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_diagnostics_adapter_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_diagnostics_adapter_deinit(&s_ctx.adapter));
    esy_diagnostics_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_enumerate(&s_ctx.core, 0u, &snapshot));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.reads + s_ctx.runs);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, saved.read(saved.context, "board.bus", 9u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, saved.enumerate(saved.context, 0u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, saved.run(saved.context, "board.bus", 9u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_remote_manager_diagnostics_adapter_get_ops(&s_ctx.adapter, &s_ctx.ops));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_diagnostics_adapter_deinit(NULL));
}
