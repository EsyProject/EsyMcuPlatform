/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_ota.h"
#include "esy_middleware/remote_manager/esy_remote_manager_coredump.h"

#include <string.h>

#define TEST_VALUE_BYTES 16u

typedef struct {
    esy_remote_manager_t manager;
    esy_remote_manager_config_t config;
    esy_remote_manager_ota_info_t image;
    esy_image_status_t ota;
    esy_remote_manager_coredump_info_t dump;
    esy_err_t authorization;
    esy_err_t info_result;
    esy_err_t begin_result;
    esy_err_t status_result;
    esy_err_t config_get_result;
    esy_err_t dump_info_result;
    esy_err_t dump_read_result;
    esy_err_t dump_clear_result;
    unsigned auth_calls;
    unsigned info_calls;
    unsigned begin_calls;
    unsigned status_calls;
    unsigned config_get_calls;
    unsigned dump_info_calls;
    unsigned dump_read_calls;
    unsigned dump_clear_calls;
    uint8_t config_output[TEST_VALUE_BYTES];
    size_t config_output_length;
    size_t config_capacity;
    char config_key[TEST_VALUE_BYTES];
    size_t config_key_length;
    uint32_t last_offset;
    size_t last_read_length;
    bool begin_accepted;
} fixture_ctx_t;

static fixture_ctx_t s_ctx;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    fixture_ctx_t *ctx = context;
    (void)request;
    ctx->auth_calls++;
    return ctx->authorization;
}

static esy_err_t ota_info(void *context, esy_remote_manager_ota_info_t *info) {
    fixture_ctx_t *ctx = context;
    ctx->info_calls++;
    *info = ctx->image;
    return ctx->info_result;
}

static esy_err_t ota_begin(void *context, const esy_image_manifest_t *manifest,
                            esy_image_status_t *status) {
    fixture_ctx_t *ctx = context;
    ctx->begin_calls++;
    if (ctx->begin_result == ESY_OK) ctx->begin_accepted = true;
    *status = (esy_image_status_t){.phase = ESY_IMAGE_RECEIVING,
        .session_id = 1u, .total_bytes = manifest->image_size};
    return ctx->begin_result;
}

static esy_err_t ota_status(void *context, uint32_t session_id, esy_image_status_t *status) {
    (void)session_id;
    fixture_ctx_t *ctx = context;
    ctx->status_calls++;
    *status = ctx->ota;
    return ctx->status_result;
}

static esy_err_t config_get(void *context, const char *key, size_t length,
                             uint8_t *output, size_t capacity,
                             size_t *output_length, bool *found) {
    fixture_ctx_t *ctx = context;
    ctx->config_get_calls++;
    ctx->config_capacity = capacity;
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof(ctx->config_key), length);
    memcpy(ctx->config_key, key, length);
    ctx->config_key_length = length;
    *found = true;
    *output_length = ctx->config_output_length;
    if (ctx->config_get_result != ESY_OK) return ctx->config_get_result;
    if (*output_length > capacity) return ESY_ERR_NO_MEM;
    if (*output_length != 0) memcpy(output, ctx->config_output, *output_length);
    return ESY_OK;
}

static esy_err_t dump_info(void *context, esy_remote_manager_coredump_info_t *info) {
    fixture_ctx_t *ctx = context;
    ctx->dump_info_calls++;
    *info = ctx->dump;
    return ctx->dump_info_result;
}

static esy_err_t dump_read(void *context, const esy_coredump_record_id_t *record_id,
                            uint32_t offset, uint8_t *output,
                            size_t length) {
    fixture_ctx_t *ctx = context;
    ctx->dump_read_calls++;
    ctx->last_offset = offset;
    ctx->last_read_length = length;
    if (memcmp(record_id->bytes, ctx->dump.record_id.bytes, sizeof(record_id->bytes)) != 0)
        return ESY_ERR_STATE;
    if (ctx->dump_read_result != ESY_OK) return ctx->dump_read_result;
    if (offset > ctx->dump.record_size || length > ctx->dump.record_size - offset)
        return ESY_ERR_INVALID_ARG;
    for (size_t index = 0; index < length; ++index) output[index] = (uint8_t)(offset + index);
    return ESY_OK;
}

static esy_err_t dump_clear(void *context, const esy_coredump_record_id_t *record_id) {
    fixture_ctx_t *ctx = context;
    ctx->dump_clear_calls++;
    if (memcmp(record_id->bytes, ctx->dump.record_id.bytes, sizeof(record_id->bytes)) != 0)
        return ESY_ERR_STATE;
    return ctx->dump_clear_result;
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.image = (esy_remote_manager_ota_info_t){
        .current_version = 12, .running_slot = 1, .max_image_bytes = 4096,
    };
    s_ctx.ota = (esy_image_status_t){.phase = ESY_IMAGE_IDLE};
    s_ctx.dump = (esy_remote_manager_coredump_info_t){
        .available = true, .record_size = 100, .record_crc = 0x1234,
        .record_id = {.bytes = {0xa5u}},
    };
    s_ctx.config = (esy_remote_manager_config_t){
        .max_key_length = 8, .max_request_value_length = 128u, .max_response_value_length = TEST_VALUE_BYTES,
        .authorize = authorize, .authorize_context = &s_ctx,
        .ota_ops = {.info = ota_info, .begin = ota_begin,
                    .status = ota_status, .context = &s_ctx},
        .config_ops = {.get = config_get, .context = &s_ctx},
        .coredump_ops = {.info = dump_info, .read = dump_read,
                         .clear = dump_clear, .context = &s_ctx},
    };
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}

static void initialize(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &s_ctx.config));
}

static esy_remote_manager_request_t request_for(uint16_t group, uint16_t command) {
    esy_remote_manager_request_t request = {.group = group, .command = command};
    if (group == ESY_REMOTE_MANAGER_GROUP_OTA && command == ESY_REMOTE_MANAGER_OTA_BEGIN)
        request.manifest = (esy_image_manifest_t){.version = 1u, .image_size = 4u};
    if (group == ESY_REMOTE_MANAGER_GROUP_COREDUMP &&
        (command == ESY_REMOTE_MANAGER_COREDUMP_READ || command == ESY_REMOTE_MANAGER_COREDUMP_CLEAR))
        request.record_id = s_ctx.dump.record_id;
    return request;
}

static esy_err_t dispatch(const esy_remote_manager_request_t *request,
                           uint8_t *output, size_t capacity,
                           esy_remote_manager_response_t *response) {
    esy_err_t status = esy_remote_manager_dispatch(&s_ctx.manager,
        request, output, capacity, response);
    TEST_ASSERT_EQUAL(status, response->status);
    return status;
}

void test_operations_discovery_reports_only_available_owner_callbacks(void) {
    const esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_SYSTEM, ESY_REMOTE_MANAGER_SYSTEM_DISCOVER);
    esy_remote_manager_response_t response;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_REMOTE_MANAGER_CAP_DISCOVER |
        ESY_REMOTE_MANAGER_CAP_OTA_INFO | ESY_REMOTE_MANAGER_CAP_OTA_BEGIN |
        ESY_REMOTE_MANAGER_CAP_OTA_STATUS | ESY_REMOTE_MANAGER_CAP_CONFIG_GET |
        ESY_REMOTE_MANAGER_CAP_COREDUMP_INFO | ESY_REMOTE_MANAGER_CAP_COREDUMP_READ |
        ESY_REMOTE_MANAGER_CAP_COREDUMP_CLEAR, response.capabilities);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.info_calls + s_ctx.begin_calls + s_ctx.status_calls +
        s_ctx.config_get_calls + s_ctx.dump_info_calls + s_ctx.dump_read_calls + s_ctx.dump_clear_calls);
}

void test_operations_ota_queries_snapshots_and_begins_a_typed_image_session(void) {
    s_ctx.config.max_response_value_length = 24u;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_INFO);
    esy_remote_manager_response_t response;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT32(12, response.ota_info.current_version);
    TEST_ASSERT_EQUAL(1, response.ota_info.running_slot);
    TEST_ASSERT_EQUAL_UINT32(4096, response.ota_info.max_image_bytes);
    request.command = ESY_REMOTE_MANAGER_OTA_BEGIN;
    request.manifest = (esy_image_manifest_t){.version = 1u, .image_size = 4u};
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_TRUE(s_ctx.begin_accepted);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.begin_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.status_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.ota.next_offset);
    s_ctx.ota = (esy_image_status_t){
        .phase = ESY_IMAGE_RECEIVING, .session_id = 1u, .next_offset = 2, .total_bytes = 4,
    };
    request.command = ESY_REMOTE_MANAGER_OTA_STATUS;
    request.manifest = (esy_image_manifest_t){0};
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL(ESY_IMAGE_RECEIVING, response.ota_status.phase);
    TEST_ASSERT_EQUAL_UINT32(2, response.ota_status.next_offset);
    TEST_ASSERT_FALSE(response.ota_status.result_valid);
    s_ctx.ota.phase = ESY_IMAGE_ACTIVATED;
    s_ctx.ota.next_offset = 4;
    s_ctx.ota.result_valid = true;
    s_ctx.ota.result = 7;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_TRUE(response.ota_status.result_valid);
    TEST_ASSERT_EQUAL(7, response.ota_status.result);
}

void test_operations_ota_rejects_invalid_provider_snapshots_and_clears_error_results(void) {
    s_ctx.config.max_response_value_length = 24u;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_INFO);
    esy_remote_manager_response_t response;
    initialize();
    s_ctx.image.running_slot = -2;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT32(0, response.ota_info.current_version);
    s_ctx.image.running_slot = -1;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    request.command = ESY_REMOTE_MANAGER_OTA_STATUS;
    request.manifest = (esy_image_manifest_t){0};
    s_ctx.ota.phase = (esy_image_phase_t)99;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, NULL, 0, &response));
    s_ctx.ota.phase = ESY_IMAGE_RECEIVING;
    s_ctx.ota.next_offset = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, NULL, 0, &response));
    s_ctx.ota.total_bytes = 1;
    s_ctx.ota.session_id = 1u;
    s_ctx.ota.result_valid = true;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, NULL, 0, &response));
    s_ctx.ota.phase = ESY_IMAGE_FAILED;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    s_ctx.status_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT32(0, response.ota_status.total_bytes);
}

void test_operations_configuration_get_returns_bounded_binary_output(void) {
    const char key[] = {'m', 'o', 'd', 'e'};
    const uint8_t expected[] = {0, 0xff, 0x80};
    uint8_t output[32];
    esy_remote_manager_response_t response;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_CONFIG, ESY_REMOTE_MANAGER_CONFIG_GET);
    request.key = key;
    request.key_length = sizeof(key);
    memcpy(s_ctx.config_output, expected, sizeof(expected));
    s_ctx.config_output_length = sizeof(expected);
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, output, sizeof(output), &response));
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), response.value_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, output, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(TEST_VALUE_BYTES, s_ctx.config_capacity);
    TEST_ASSERT_EQUAL_UINT(sizeof(key), s_ctx.config_key_length);
    TEST_ASSERT_EQUAL_MEMORY(key, s_ctx.config_key, sizeof(key));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.config_get_calls);
    TEST_ASSERT_TRUE(response.found);
}

void test_operations_configuration_get_rejects_invalid_keys_and_values_before_authorization(void) {
    const char nul[] = {'a', '\0'};
    const char valid[] = {'a', 'b'};
    const uint8_t value = 1u;
    esy_remote_manager_response_t response;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_CONFIG, ESY_REMOTE_MANAGER_CONFIG_GET);
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    request.key_length = sizeof(valid);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    request.key = nul;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    request.key = valid;
    request.key_length = s_ctx.config.max_key_length + 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    request.key_length = sizeof(valid);
    request.value = &value;
    request.value_length = 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.auth_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.config_get_calls);
}

void test_operations_configuration_get_preserves_required_output_length_without_retry(void) {
    const char key[] = {'a'};
    uint8_t output[2];
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_CONFIG, ESY_REMOTE_MANAGER_CONFIG_GET);
    esy_remote_manager_response_t response;
    request.key = key;
    request.key_length = sizeof(key);
    s_ctx.config_output_length = 3;
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, dispatch(&request, output, sizeof(output), &response));
    TEST_ASSERT_EQUAL_UINT(3, response.value_length);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.config_get_calls);
    s_ctx.config_get_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, output, sizeof(output), &response));
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
    s_ctx.config_get_result = ESY_ERR_NO_MEM;
    s_ctx.config_output_length = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, output, sizeof(output), &response));
    s_ctx.config_output_length = TEST_VALUE_BYTES + 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, output, sizeof(output), &response));
}

void test_operations_coredump_info_read_and_clear_are_separate_requests(void) {
    uint8_t output[3];
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_INFO);
    esy_remote_manager_response_t response;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_TRUE(response.coredump_info.available);
    TEST_ASSERT_EQUAL_UINT32(100, response.coredump_info.record_size);
    request.command = ESY_REMOTE_MANAGER_COREDUMP_READ;
    request.record_id = s_ctx.dump.record_id;
    request.offset = 4;
    request.length = sizeof(output);
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, output, sizeof(output), &response));
    TEST_ASSERT_EQUAL_UINT(sizeof(output), response.value_length);
    TEST_ASSERT_EQUAL_UINT32(4, s_ctx.last_offset);
    TEST_ASSERT_EQUAL_UINT(3, s_ctx.last_read_length);
    TEST_ASSERT_EQUAL_UINT8(4, output[0]);
    TEST_ASSERT_EQUAL_UINT8(6, output[2]);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.dump_clear_calls);
    request = request_for(ESY_REMOTE_MANAGER_GROUP_COREDUMP,
                           ESY_REMOTE_MANAGER_COREDUMP_CLEAR);
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.dump_clear_calls);
}

void test_operations_coredump_bounds_prevent_provider_access_before_output_is_available(void) {
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_READ);
    esy_remote_manager_response_t response;
    initialize();
    request.length = 3;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(3, response.value_length);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.dump_read_calls);
    request.length = TEST_VALUE_BYTES + 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.dump_read_calls);
    request.offset = UINT32_MAX;
    request.length = 1;
    const unsigned auth_calls = s_ctx.auth_calls;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(auth_calls, s_ctx.auth_calls);
    request.length = 0;
    s_ctx.dump_read_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.dump_read_calls);
    TEST_ASSERT_EQUAL_UINT(0, response.value_length);
}

void test_operations_coredump_rejects_inconsistent_provider_record_identity(void) {
    const esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_INFO);
    esy_remote_manager_response_t response;
    initialize();
    const esy_coredump_record_id_t identity = s_ctx.dump.record_id;
    s_ctx.dump.record_id = (esy_coredump_record_id_t){0};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, NULL, 0, &response));
    s_ctx.dump.record_id = identity;
    s_ctx.dump.record_size = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, NULL, 0, &response));
    s_ctx.dump.available = false;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, NULL, 0, &response));
    s_ctx.dump.record_crc = 0;
    s_ctx.dump.record_size = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, NULL, 0, &response));
    s_ctx.dump.record_size = 0;
    s_ctx.dump.record_id = (esy_coredump_record_id_t){0};
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_FALSE(response.coredump_info.available);
    s_ctx.dump_info_result = ESY_ERR_CRC_FAIL;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, dispatch(&request, NULL, 0, &response));
}

void test_operations_authorization_precedes_each_new_domain_provider(void) {
    s_ctx.config.max_response_value_length = 24u;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_BEGIN);
    esy_remote_manager_response_t response;
    s_ctx.authorization = ESY_ERR_FAIL;
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, dispatch(&request, NULL, 0, &response));
    request = request_for(ESY_REMOTE_MANAGER_GROUP_CONFIG,
                           ESY_REMOTE_MANAGER_CONFIG_GET);
    request.key = "mode";
    request.key_length = 4u;
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, dispatch(&request, NULL, 0, &response));
    request = request_for(ESY_REMOTE_MANAGER_GROUP_COREDUMP,
                           ESY_REMOTE_MANAGER_COREDUMP_CLEAR);
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.begin_calls + s_ctx.config_get_calls + s_ctx.dump_clear_calls);
    TEST_ASSERT_FALSE(s_ctx.begin_accepted);
}

void test_operations_invalid_extra_arguments_fail_before_authorization(void) {
    const uint8_t value = 1;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_BEGIN);
    esy_remote_manager_response_t response;
    initialize();
    request.offset = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    request.offset = 0;
    request.value = &value;
    request.value_length = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    request = request_for(ESY_REMOTE_MANAGER_GROUP_COREDUMP,
                           ESY_REMOTE_MANAGER_COREDUMP_INFO);
    request.key = "x";
    request.key_length = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.auth_calls);
}

void test_operations_unsupported_callbacks_and_commands_do_not_cross_domains(void) {
    s_ctx.config.max_response_value_length = 24u;
    esy_remote_manager_response_t response;
    s_ctx.config.ota_ops = (esy_remote_manager_ota_ops_t){0};
    s_ctx.config.config_ops = (esy_remote_manager_config_ops_t){0};
    s_ctx.config.coredump_ops = (esy_remote_manager_coredump_ops_t){0};
    initialize();
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_INFO);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request.command = ESY_REMOTE_MANAGER_OTA_BEGIN;
    request.manifest = (esy_image_manifest_t){.version = 1u, .image_size = 4u};
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request.command = ESY_REMOTE_MANAGER_OTA_STATUS;
    request.manifest = (esy_image_manifest_t){0};
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request.command = UINT16_MAX;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request = request_for(ESY_REMOTE_MANAGER_GROUP_CONFIG, UINT16_MAX);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request.command = ESY_REMOTE_MANAGER_CONFIG_GET;
    request.key = "mode";
    request.key_length = 4u;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request = request_for(ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_INFO);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request.command = ESY_REMOTE_MANAGER_COREDUMP_READ;
    request.record_id = s_ctx.dump.record_id;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request.command = ESY_REMOTE_MANAGER_COREDUMP_CLEAR;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
    request.command = UINT16_MAX;
    request.record_id = (esy_coredump_record_id_t){0};
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, NULL, 0, &response));
}

void test_operations_provider_failures_are_propagated_without_retry(void) {
    s_ctx.config.max_response_value_length = 24u;
    esy_remote_manager_response_t response;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_INFO);
    s_ctx.info_result = ESY_ERR_IO;
    s_ctx.begin_result = ESY_ERR_BUSY;
    s_ctx.status_result = ESY_ERR_FAIL;
    s_ctx.dump_clear_result = ESY_ERR_IO;
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, NULL, 0, &response));
    request.command = ESY_REMOTE_MANAGER_OTA_BEGIN;
    request.manifest = (esy_image_manifest_t){.version = 1u, .image_size = 4u};
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_FALSE(s_ctx.begin_accepted);
    request.command = ESY_REMOTE_MANAGER_OTA_STATUS;
    request.manifest = (esy_image_manifest_t){0};
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, dispatch(&request, NULL, 0, &response));
    request = request_for(ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_CLEAR);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.info_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.begin_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.status_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.dump_clear_calls);
}

void test_coredump_identity_is_required_only_for_read_and_clear_before_authorization(void) {
    initialize();
    esy_remote_manager_response_t response;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_READ);
    request.record_id = (esy_coredump_record_id_t){0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0u, &response));
    request.command = ESY_REMOTE_MANAGER_COREDUMP_CLEAR;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0u, &response));
    request.command = ESY_REMOTE_MANAGER_COREDUMP_INFO;
    request.record_id = s_ctx.dump.record_id;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0u, &response));
    request.group = ESY_REMOTE_MANAGER_GROUP_SYSTEM;
    request.command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0u, &response));
    request.group = ESY_REMOTE_MANAGER_GROUP_CONFIG;
    request.command = ESY_REMOTE_MANAGER_CONFIG_SAVE;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0u, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.auth_calls);
}

void test_coredump_argument_budgets_count_the_complete_record_id(void) {
    initialize();
    esy_remote_manager_response_t response;
    esy_remote_manager_request_t request = request_for(
        ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_READ);
    s_ctx.manager.config.max_request_value_length = 23u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0u, &response));
    s_ctx.manager.config.max_request_value_length = 24u;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0u, &response));
    request.command = ESY_REMOTE_MANAGER_COREDUMP_CLEAR;
    s_ctx.manager.config.max_request_value_length = 15u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(&request, NULL, 0u, &response));
    s_ctx.manager.config.max_request_value_length = 16u;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, NULL, 0u, &response));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.auth_calls);
}
