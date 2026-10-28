/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_diagnostics.h"
#include "esy_middleware/remote_manager/esy_remote_manager_proto_bin.h"
#include "unity.h"

#include <string.h>

#define TEST_BUFFER_BYTES 256u
#define TEST_RESPONSE_BYTES 197u
#define TEST_NAME_BYTES 6u

typedef struct {
    esy_remote_manager_t manager;
    esy_diagnostics_snapshot_t snapshot;
    esy_err_t authorization_result;
    esy_err_t provider_result;
    unsigned authorizations, enumerations, reads, runs, effects;
    uint32_t last_index;
    bool enum_assessment;
    uint8_t input[TEST_BUFFER_BYTES], output[TEST_BUFFER_BYTES];
} diagnostics_test_ctx_t;
static diagnostics_test_ctx_t s_ctx;
static const char k_name[] = "io.bus";

static esy_diagnostics_snapshot_t valid_snapshot(void) {
    esy_diagnostics_snapshot_t snapshot = {.count = 2u, .index = 1u,
        .capabilities = ESY_DIAGNOSTICS_CAP_READ | ESY_DIAGNOSTICS_CAP_RUN,
        .name_length = TEST_NAME_BYTES,
        .result = {.health = ESY_DIAGNOSTICS_HEALTH_OK, .code = 0x11223344u,
                   .detail_length = 6u}};
    memcpy(snapshot.name, k_name, sizeof(k_name));
    memcpy(snapshot.result.detail, "stable", 7u);
    return snapshot;
}
static esy_err_t authorize(void *context, const esy_remote_manager_request_t *request) {
    (void)request;
    diagnostics_test_ctx_t *ctx = context;
    ++ctx->authorizations;
    return ctx->authorization_result;
}
static esy_err_t enumerate_source(void *context, uint32_t index,
                                  esy_diagnostics_snapshot_t *snapshot) {
    diagnostics_test_ctx_t *ctx = context;
    ++ctx->enumerations;
    ctx->last_index = index;
    *snapshot = ctx->snapshot;
    if (!ctx->enum_assessment) snapshot->result = (esy_diagnostics_result_t){0};
    return ctx->provider_result;
}
static esy_err_t read_source(void *context, const char *name, size_t length,
                             esy_diagnostics_snapshot_t *snapshot) {
    diagnostics_test_ctx_t *ctx = context;
    ++ctx->reads;
    if (length != TEST_NAME_BYTES || memcmp(name, k_name, length) != 0)
        return ESY_ERR_NOT_SUPPORTED;
    *snapshot = ctx->snapshot;
    return ctx->provider_result;
}
static esy_err_t run_source(void *context, const char *name, size_t length,
                            esy_diagnostics_snapshot_t *snapshot) {
    diagnostics_test_ctx_t *ctx = context;
    ++ctx->runs;
    if (length != TEST_NAME_BYTES || memcmp(name, k_name, length) != 0)
        return ESY_ERR_NOT_SUPPORTED;
    ++ctx->effects;
    *snapshot = ctx->snapshot;
    return ctx->provider_result;
}
static esy_remote_manager_request_t request_for(uint16_t command) {
    esy_remote_manager_request_t request = {.group = ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS,
        .command = command};
    if (command == ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE) request.diagnostic_index = 1u;
    else if (command == ESY_REMOTE_MANAGER_DIAGNOSTICS_READ || command == ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN) {
        request.key = k_name;
        request.key_length = TEST_NAME_BYTES;
    }
    return request;
}
static esy_err_t dispatch(const esy_remote_manager_request_t *request,
                           esy_remote_manager_response_t *response) {
    const esy_err_t result = esy_remote_manager_dispatch(&s_ctx.manager, request, NULL, 0u, response);
    TEST_ASSERT_EQUAL(result, response->status);
    TEST_ASSERT_EQUAL_UINT32(0u, response->value_length);
    return result;
}
void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.snapshot = valid_snapshot();
    const esy_remote_manager_config_t config = {
        .max_key_length = ESY_DIAGNOSTICS_NAME_MAX_BYTES,
        .max_request_value_length = ESY_REMOTE_MANAGER_DIAGNOSTICS_INDEX_BYTES,
        .max_response_value_length = ESY_REMOTE_MANAGER_PROTO_BIN_DIAGNOSTICS_BYTES,
        .authorize = authorize, .authorize_context = &s_ctx,
        .diagnostics_ops = {.enumerate = enumerate_source, .read = read_source,
            .run = run_source, .context = &s_ctx}};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
}
void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}

void test_diagnostics_enumeration_never_queries_or_runs_and_discovery_is_per_operation(void) {
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(1u, response.diagnostic.index);
    TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_UNKNOWN, response.diagnostic.result.health);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.reads + s_ctx.runs + s_ctx.effects);
    request = (esy_remote_manager_request_t){.group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER};
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    const uint32_t expected = ESY_REMOTE_MANAGER_CAP_DISCOVER |
        ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_ENUMERATE | ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_READ |
        ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_RUN;
    TEST_ASSERT_EQUAL_HEX32(expected, response.capabilities);
    s_ctx.manager.config.diagnostics_ops.run = NULL;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_HEX32(expected & ~ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_RUN, response.capabilities);
}

void test_diagnostics_typed_core_has_no_encoding_budget_and_unhealthy_findings_succeed(void) {
    s_ctx.manager.config.max_response_value_length = 0u;
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ);
    esy_remote_manager_response_t response;
    s_ctx.snapshot.result.health = ESY_DIAGNOSTICS_HEALTH_UNKNOWN;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.runs);
    s_ctx.snapshot.result.health = ESY_DIAGNOSTICS_HEALTH_ERROR;
    request.command = ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_ERROR, response.diagnostic.result.health);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.effects);
}

void test_diagnostics_run_failure_can_follow_effects_without_automatic_retry_or_result(void) {
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN);
    esy_remote_manager_response_t response;
    s_ctx.provider_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.runs);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.effects);
    TEST_ASSERT_EQUAL_UINT32(0u, response.diagnostic.count);
    s_ctx.provider_result = ESY_OK;
    request.command = ESY_REMOTE_MANAGER_DIAGNOSTICS_READ;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.effects);
}

void test_diagnostics_authorization_denial_and_unknown_sources_never_start_tests(void) {
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN);
    esy_remote_manager_response_t response;
    s_ctx.authorization_result = ESY_ERR_FAIL;
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.runs + s_ctx.effects);
    s_ctx.authorization_result = ESY_OK;
    request.key = "other"; request.key_length = 5u;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.effects);
    request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ);
    s_ctx.manager.config.diagnostics_ops.read = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
    request = request_for(UINT16_MAX);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
}

static void invalid_request(esy_remote_manager_request_t request) {
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
}
void test_diagnostics_rejects_every_foreign_normalized_argument_before_authorization(void) {
    const esy_remote_manager_request_t baseline = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ);
    esy_remote_manager_request_t request = baseline;
    request.manifest.version = 1u; invalid_request(request);
    request = baseline; request.session_id = 1u; invalid_request(request);
    request = baseline; request.offset = 1u; invalid_request(request);
    request = baseline; request.length = 1u; invalid_request(request);
    request = baseline; request.slot_index = 1u; invalid_request(request);
    request = baseline; request.image.slot = 1u; invalid_request(request);
    request = baseline; request.stat_index = 1u; invalid_request(request);
    request = baseline; request.record_id.bytes[0] = 1u; invalid_request(request);
    request = baseline; request.diagnostic_index = 1u; invalid_request(request);
    request = baseline; request.value = (const uint8_t *)"x"; request.value_length = 1u; invalid_request(request);
    request = (esy_remote_manager_request_t){.group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, .diagnostic_index = 1u};
    invalid_request(request);
}
void test_diagnostics_rejects_invalid_names_enumeration_keys_and_semantic_budgets(void) {
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ);
    request.key_length = 0u; invalid_request(request);
    request.key = "bad name"; request.key_length = 8u; invalid_request(request);
    const char embedded[] = {'a', '\0', 'b'};
    request.key = embedded; request.key_length = sizeof(embedded); invalid_request(request);
    char oversized[ESY_DIAGNOSTICS_NAME_MAX_BYTES + 1u]; memset(oversized, 'a', sizeof(oversized));
    s_ctx.manager.config.max_key_length = sizeof(oversized);
    request.key = oversized; request.key_length = sizeof(oversized); invalid_request(request);
    request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE);
    request.key = k_name; request.key_length = TEST_NAME_BYTES; invalid_request(request);
    request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE);
    s_ctx.manager.config.max_request_value_length = 3u; invalid_request(request);
    request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ);
    s_ctx.manager.config.max_key_length = TEST_NAME_BYTES - 1u; invalid_request(request);
}

void test_diagnostics_empty_inventory_and_enumeration_metadata_are_strictly_validated(void) {
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE);
    esy_remote_manager_response_t response;
    s_ctx.snapshot = (esy_diagnostics_snapshot_t){0}; request.diagnostic_index = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    s_ctx.snapshot.name[ESY_DIAGNOSTICS_NAME_MAX_BYTES] = 'x';
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
    s_ctx.snapshot = (esy_diagnostics_snapshot_t){0}; request.diagnostic_index = 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
    s_ctx.snapshot = valid_snapshot(); s_ctx.enum_assessment = true;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
    s_ctx.snapshot.result = (esy_diagnostics_result_t){0};
    s_ctx.snapshot.result.detail[ESY_DIAGNOSTICS_DETAIL_MAX_BYTES] = 'x';
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
    s_ctx.snapshot.result = (esy_diagnostics_result_t){0};
    s_ctx.snapshot.index = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
}

void test_diagnostics_rejects_malformed_descriptors_and_result_lengths_before_publication(void) {
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ);
    esy_remote_manager_response_t response;
    for (unsigned defect = 0u; defect < 12u; ++defect) {
        s_ctx.snapshot = valid_snapshot();
        switch (defect) {
        case 0u: s_ctx.snapshot.count = 0u; break;
        case 1u: s_ctx.snapshot.index = s_ctx.snapshot.count; break;
        case 2u: s_ctx.snapshot.capabilities = ESY_DIAGNOSTICS_CAP_RUN; break;
        case 3u: s_ctx.snapshot.capabilities |= 4u; break;
        case 4u: s_ctx.snapshot.name_length = 0u; break;
        case 5u: s_ctx.snapshot.name_length = ESY_DIAGNOSTICS_NAME_MAX_BYTES + 1u; break;
        case 6u: s_ctx.snapshot.name[TEST_NAME_BYTES] = 'x'; break;
        case 7u: s_ctx.snapshot.name[0] = '@'; break;
        case 8u: s_ctx.snapshot.result.health = (esy_diagnostics_health_t)4; break;
        case 9u: s_ctx.snapshot.result.detail_length = ESY_DIAGNOSTICS_DETAIL_MAX_BYTES + 1u; break;
        case 10u: s_ctx.snapshot.result.detail[6] = 'x'; break;
        default: s_ctx.snapshot.name[0] = 'X'; break;
        }
        TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
        TEST_ASSERT_EQUAL_UINT32(0u, response.diagnostic.count);
    }
}
void test_diagnostics_rejects_control_detail_bytes_positive_errors_and_unfinished_run_results(void) {
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ);
    esy_remote_manager_response_t response;
    const unsigned char invalid[] = {0u, 0x1fu, 0x7fu, 0x80u};
    for (size_t i = 0u; i < sizeof(invalid); ++i) {
        s_ctx.snapshot = valid_snapshot(); s_ctx.snapshot.result.detail[0] = (char)invalid[i];
        TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
    }
    s_ctx.snapshot = valid_snapshot(); s_ctx.provider_result = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
    s_ctx.provider_result = ESY_OK; request.command = ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN;
    s_ctx.snapshot.result.health = ESY_DIAGNOSTICS_HEALTH_UNKNOWN;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
    s_ctx.snapshot.result.health = ESY_DIAGNOSTICS_HEALTH_OK;
    s_ctx.snapshot.capabilities = ESY_DIAGNOSTICS_CAP_READ;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
}

static size_t wire_request(uint16_t command, const char *key, size_t key_length,
                            const uint8_t *value, size_t value_length) {
    memset(s_ctx.input, 0, sizeof(s_ctx.input));
    const uint8_t header[] = {2u, 1u, 1u, 0u, 6u, 0u, (uint8_t)command,
        (uint8_t)(command >> 8u), 0x44u, 0x33u, 0x22u, 0x11u,
        (uint8_t)key_length, 0u, (uint8_t)value_length, 0u, 0u, 0u};
    memcpy(s_ctx.input, header, sizeof(header));
    if (key_length != 0u) memcpy(s_ctx.input + sizeof(header), key, key_length);
    if (value_length != 0u) memcpy(s_ctx.input + sizeof(header) + key_length, value, value_length);
    return sizeof(header) + key_length + value_length;
}
static size_t process(size_t length, size_t capacity) {
    size_t output_length = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_proto_bin_process(&s_ctx.manager,
        s_ctx.input, length, s_ctx.output, capacity, &output_length));
    return output_length;
}
void test_diagnostics_wire_v1_has_exact_golden_metadata_and_zero_padded_text(void) {
    memset(s_ctx.snapshot.name + TEST_NAME_BYTES + 1u, 0xa5,
        ESY_DIAGNOSTICS_NAME_MAX_BYTES - TEST_NAME_BYTES);
    memset(s_ctx.snapshot.result.detail + 7u, 0xa5, ESY_DIAGNOSTICS_DETAIL_MAX_BYTES - 6u);
    const size_t length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ, k_name,
        TEST_NAME_BYTES, NULL, 0u);
    memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(TEST_RESPONSE_BYTES, process(length, TEST_RESPONSE_BYTES));
    const uint8_t header[] = {2u, 2u, 1u, 0u, 6u, 0u, 2u, 0u, 0x44u, 0x33u, 0x22u, 0x11u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 172u, 0u, 0u, 0u};
    const uint8_t metadata[ESY_REMOTE_MANAGER_PROTO_BIN_DIAGNOSTICS_BYTES] = {
        [0] = 2u, [4] = 1u, [8] = 3u, [12] = 1u,
        [16] = 0x44u, [17] = 0x33u, [18] = 0x22u, [19] = 0x11u,
        [20] = 6u, [24] = 6u,
        [28] = 'i', [29] = 'o', [30] = '.', [31] = 'b', [32] = 'u', [33] = 's',
        [76] = 's', [77] = 't', [78] = 'a', [79] = 'b', [80] = 'l', [81] = 'e'};
    TEST_ASSERT_EQUAL_MEMORY(header, s_ctx.output, sizeof(header));
    TEST_ASSERT_EQUAL_MEMORY(metadata, s_ctx.output + sizeof(header), sizeof(metadata));
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.output[TEST_RESPONSE_BYTES]);
}
void test_diagnostics_wire_short_responses_and_budgets_cannot_authorize_or_start_self_tests(void) {
    const size_t length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, k_name,
        TEST_NAME_BYTES, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, TEST_RESPONSE_BYTES - 1u));
    TEST_ASSERT_EQUAL_UINT8(0xfdu, s_ctx.output[12]);
    TEST_ASSERT_EQUAL_UINT8(172u, s_ctx.output[21]);
    s_ctx.manager.config.max_response_value_length = 171u;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.output)));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations + s_ctx.runs + s_ctx.effects);
    s_ctx.manager.config.max_response_value_length = 172u;
    TEST_ASSERT_EQUAL_UINT32(TEST_RESPONSE_BYTES, process(length, TEST_RESPONSE_BYTES));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.effects);
}
void test_diagnostics_wire_enumeration_decodes_index_without_querying_and_empty_inventory_is_zero(void) {
    const uint8_t index[] = {4u, 3u, 2u, 1u};
    s_ctx.snapshot.index = 0x01020304u; s_ctx.snapshot.count = 0x01020305u;
    size_t length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE, NULL, 0u, index, sizeof(index));
    TEST_ASSERT_EQUAL_UINT32(TEST_RESPONSE_BYTES, process(length, TEST_RESPONSE_BYTES));
    TEST_ASSERT_EQUAL_HEX32(0x01020304u, s_ctx.last_index);
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.output[37]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.reads + s_ctx.runs);
    s_ctx.snapshot = (esy_diagnostics_snapshot_t){0};
    const uint8_t first[4] = {0};
    length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE, NULL, 0u, first, sizeof(first));
    TEST_ASSERT_EQUAL_UINT32(TEST_RESPONSE_BYTES, process(length, TEST_RESPONSE_BYTES));
    const uint8_t zero[ESY_REMOTE_MANAGER_PROTO_BIN_DIAGNOSTICS_BYTES] = {0};
    TEST_ASSERT_EQUAL_MEMORY(zero, s_ctx.output + 25u, sizeof(zero));
}
void test_diagnostics_wire_malformed_arguments_and_failed_runs_never_append_partial_results(void) {
    const uint8_t value[5] = {0};
    const size_t invalid_lengths[] = {0u, 3u, 5u};
    for (size_t i = 0u; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); ++i) {
        const size_t length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE,
            NULL, 0u, value, invalid_lengths[i]);
        size_t output_length = 99u;
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_proto_bin_process(&s_ctx.manager,
            s_ctx.input, length, s_ctx.output, sizeof(s_ctx.output), &output_length));
        TEST_ASSERT_EQUAL_UINT32(0u, output_length);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    size_t length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, k_name, TEST_NAME_BYTES, value, 1u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.output)));
    TEST_ASSERT_EQUAL_UINT8(0xfeu, s_ctx.output[12]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    s_ctx.provider_result = ESY_ERR_IO;
    length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, k_name, TEST_NAME_BYTES, NULL, 0u);
    memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.output)));
    TEST_ASSERT_EQUAL_UINT8(0xf8u, s_ctx.output[12]);
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.output[21]);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.output[25]);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.runs);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.effects);
}

void test_diagnostics_wire_maximum_text_spans_fit_without_terminators_or_overflow(void) {
    memset(s_ctx.snapshot.result.detail, '~', ESY_DIAGNOSTICS_DETAIL_MAX_BYTES);
    s_ctx.snapshot.result.detail[ESY_DIAGNOSTICS_DETAIL_MAX_BYTES] = '\0';
    s_ctx.snapshot.result.detail_length = ESY_DIAGNOSTICS_DETAIL_MAX_BYTES;
    size_t length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_READ, k_name,
        TEST_NAME_BYTES, NULL, 0u);
    memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(TEST_RESPONSE_BYTES, process(length, TEST_RESPONSE_BYTES));
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.snapshot.result.detail, s_ctx.output + 101u,
        ESY_DIAGNOSTICS_DETAIL_MAX_BYTES);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.output[TEST_RESPONSE_BYTES]);
    memset(s_ctx.snapshot.name, 'a', ESY_DIAGNOSTICS_NAME_MAX_BYTES);
    s_ctx.snapshot.name[ESY_DIAGNOSTICS_NAME_MAX_BYTES] = '\0';
    s_ctx.snapshot.name_length = ESY_DIAGNOSTICS_NAME_MAX_BYTES;
    const uint8_t index[] = {1u, 0u, 0u, 0u};
    length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_ENUMERATE, NULL, 0u, index, sizeof(index));
    TEST_ASSERT_EQUAL_UINT32(TEST_RESPONSE_BYTES, process(length, TEST_RESPONSE_BYTES));
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.snapshot.name, s_ctx.output + 53u, ESY_DIAGNOSTICS_NAME_MAX_BYTES);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.runs);
}

void test_diagnostics_wire_permission_and_malformed_results_do_not_publish_assessments(void) {
    const size_t length = wire_request(ESY_REMOTE_MANAGER_DIAGNOSTICS_RUN, k_name,
        TEST_NAME_BYTES, NULL, 0u);
    s_ctx.authorization_result = ESY_ERR_FAIL;
    memset(s_ctx.output, 0xa5, sizeof(s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.output)));
    TEST_ASSERT_EQUAL_UINT8(0xffu, s_ctx.output[12]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.runs + s_ctx.effects);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.output[25]);
    s_ctx.authorization_result = ESY_OK;
    s_ctx.snapshot.result.health = ESY_DIAGNOSTICS_HEALTH_UNKNOWN;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.output)));
    TEST_ASSERT_EQUAL_UINT8(0xf6u, s_ctx.output[12]);
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.output[21]);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.output[25]);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.effects);
}
