/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/remote_manager/esy_remote_manager_proto_bin.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "unity.h"
#include <string.h>

#define TEST_BUFFER_BYTES 128u
#define TEST_MAX_VALUE_BYTES 128u

typedef struct {
    esy_remote_manager_t manager;
    uint8_t request[TEST_BUFFER_BYTES];
    uint8_t response[TEST_BUFFER_BYTES];
    esy_err_t auth_result;
    unsigned auth_count;
    unsigned begins;
    unsigned writes;
    unsigned finishes;
    unsigned aborts;
    unsigned activates;
    esy_image_manifest_t manifest;
    uint32_t session_id;
    uint32_t next_offset;
    unsigned reads;
    unsigned config_reads;
    unsigned statuses;
    esy_coredump_record_id_t record_id;
    bool dump_available;
    unsigned dump_infos;
    unsigned clears;
    unsigned discards;
} operations_wire_test_ctx_t;

static operations_wire_test_ctx_t s_ctx;
static const uint8_t k_record[] = {10u, 20u, 30u, 40u, 50u, 60u};
static const esy_coredump_record_id_t k_record_id = {
    .bytes = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u}};

static esy_err_t authorize(void *context, const esy_remote_manager_request_t *request) {
    (void)request;
    operations_wire_test_ctx_t *ctx = context;
    ++ctx->auth_count;
    return ctx->auth_result;
}

static esy_err_t ota_info(void *context, esy_remote_manager_ota_info_t *info) {
    (void)context;
    *info = (esy_remote_manager_ota_info_t){0x01020304u, 1, 0x00100000u, 32u};
    return ESY_OK;
}

static void snapshot(operations_wire_test_ctx_t *ctx, esy_image_phase_t phase,
                      esy_image_status_t *status) {
    *status = (esy_image_status_t){.phase = phase, .session_id = ctx->session_id,
        .next_offset = ctx->next_offset, .total_bytes = ctx->manifest.image_size};
}

static esy_err_t ota_begin(void *context, const esy_image_manifest_t *manifest,
                            esy_image_status_t *status) {
    operations_wire_test_ctx_t *ctx = context;
    ++ctx->begins;
    ctx->manifest = *manifest;
    ctx->session_id = 0x11223344u;
    snapshot(ctx, ESY_IMAGE_RECEIVING, status);
    return ESY_OK;
}

static esy_err_t ota_write(void *context, uint32_t id, uint32_t offset,
                            const uint8_t *data, size_t length,
                            esy_image_status_t *status) {
    operations_wire_test_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_HEX32(ctx->session_id, id);
    TEST_ASSERT_EQUAL_UINT32(ctx->next_offset, offset);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, data[0]);
    ++ctx->writes;
    ctx->next_offset += (uint32_t)length;
    snapshot(ctx, ESY_IMAGE_RECEIVING, status);
    return ESY_OK;
}

static esy_err_t ota_finish(void *context, uint32_t id, esy_image_status_t *status) {
    operations_wire_test_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_HEX32(ctx->session_id, id);
    ++ctx->finishes;
    snapshot(ctx, ESY_IMAGE_READY, status);
    return ESY_OK;
}

static esy_err_t ota_abort(void *context, uint32_t id, esy_image_status_t *status) {
    operations_wire_test_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_HEX32(ctx->session_id, id);
    ++ctx->aborts;
    snapshot(ctx, ESY_IMAGE_ABORTED, status);
    status->result_valid = true;
    return ESY_OK;
}

static esy_err_t ota_activate(void *context, uint32_t id, esy_image_status_t *status) {
    operations_wire_test_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_HEX32(ctx->session_id, id);
    ++ctx->activates;
    snapshot(ctx, ESY_IMAGE_ACTIVATED, status);
    status->result_valid = true;
    return ESY_OK;
}

static esy_err_t ota_status(void *context, uint32_t id, esy_image_status_t *status) {
    operations_wire_test_ctx_t *ctx = context;
    (void)id;
    ++ctx->statuses;
    *status = (esy_image_status_t){ESY_IMAGE_FAILED, 0x11223344u,
        256u, 1024u, ESY_ERR_IO, true};
    return ESY_OK;
}

static esy_err_t dump_info(void *context, esy_remote_manager_coredump_info_t *info) {
    operations_wire_test_ctx_t *ctx = context;
    ++ctx->dump_infos;
    if (!ctx->dump_available) {
        *info = (esy_remote_manager_coredump_info_t){0};
        return ESY_OK;
    }
    *info = (esy_remote_manager_coredump_info_t){.record_size = sizeof(k_record),
        .record_crc = 0x11223344u, .available = true, .record_id = ctx->record_id};
    return ESY_OK;
}

static esy_err_t dump_read(void *context, const esy_coredump_record_id_t *record_id,
                           uint32_t offset, uint8_t *output, size_t length) {
    operations_wire_test_ctx_t *ctx = context;
    ++ctx->reads;
    if (!ctx->dump_available ||
        memcmp(record_id->bytes, ctx->record_id.bytes, sizeof(record_id->bytes)) != 0)
        return ESY_ERR_STATE;
    if (offset > sizeof(k_record) || length > sizeof(k_record) - offset)
        return ESY_ERR_INVALID_ARG;
    if (length != 0u) memcpy(output, k_record + offset, length);
    return ESY_OK;
}

static esy_err_t dump_clear(void *context, const esy_coredump_record_id_t *record_id) {
    operations_wire_test_ctx_t *ctx = context;
    ++ctx->clears;
    if (!ctx->dump_available ||
        memcmp(record_id->bytes, ctx->record_id.bytes, sizeof(record_id->bytes)) != 0)
        return ESY_ERR_STATE;
    ++ctx->discards;
    ctx->dump_available = false;
    return ESY_OK;
}

static esy_err_t config_get(void *context, const char *key, size_t length,
                             uint8_t *output, size_t capacity,
                             size_t *output_length, bool *found) {
    ++((operations_wire_test_ctx_t *)context)->config_reads;
    if (length != 4u || memcmp(key, "mode", 4u) != 0) return ESY_ERR_NOT_SUPPORTED;
    *found = true;
    *output_length = 2u;
    if (capacity < 2u) return ESY_ERR_NO_MEM;
    memcpy(output, "ok", 2u);
    return ESY_OK;
}

static size_t request(uint16_t group, uint16_t cmd, const uint8_t *value, size_t length) {
    memset(s_ctx.request, 0, sizeof(s_ctx.request));
    const uint8_t header[] = {ESY_PROTO_DOMAIN_REMOTE_MANAGER, 1u, 1u, 0u,
        (uint8_t)group, (uint8_t)(group >> 8u), (uint8_t)cmd, (uint8_t)(cmd >> 8u),
        0x44u, 0x33u, 0x22u, 0x11u, 0u, 0u, (uint8_t)length, 0u, 0u, 0u};
    memcpy(s_ctx.request, header, sizeof(header));
    if (length != 0u) memcpy(s_ctx.request + sizeof(header), value, length);
    return sizeof(header) + length;
}

static size_t process(size_t length, size_t capacity) {
    size_t result = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_proto_bin_process(&s_ctx.manager,
        s_ctx.request, length, s_ctx.response, capacity, &result));
    return result;
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.record_id = k_record_id;
    s_ctx.dump_available = true;
    const esy_remote_manager_config_t config = {
        .max_key_length = 16u, .max_request_value_length = TEST_MAX_VALUE_BYTES, .max_response_value_length = TEST_MAX_VALUE_BYTES,
        .authorize = authorize, .authorize_context = &s_ctx,
        .ota_ops = {.info = ota_info, .begin = ota_begin, .write = ota_write, .finish = ota_finish,
                    .abort = ota_abort, .activate = ota_activate,
                    .status = ota_status, .context = &s_ctx},
        .config_ops = {.get = config_get, .context = &s_ctx},
        .coredump_ops = {.info = dump_info, .read = dump_read,
            .clear = dump_clear, .context = &s_ctx}};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}

void test_ota_info_wire_has_explicit_endian_and_correlation(void) {
    size_t length = request(ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_INFO, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(41u, process(length, sizeof(s_ctx.response)));
    const uint8_t expected[] = {2u, 2u, 1u, 0u, 2u, 0u, 1u, 0u,
        0x44u, 0x33u, 0x22u, 0x11u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 16u, 0u, 0u, 0u, 4u, 3u, 2u, 1u, 1u, 0u, 0u, 0u,
        0u, 0u, 16u, 0u, 32u, 0u, 0u, 0u};
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.response, sizeof(expected));
}

void test_ota_status_wire_preserves_session_offset_signed_result_and_terminal_flag(void) {
    const uint8_t current[] = {0u, 0u, 0u, 0u};
    size_t length = request(ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_STATUS, current, 4u);
    TEST_ASSERT_EQUAL_UINT32(49u, process(length, sizeof(s_ctx.response)));
    const uint8_t expected[] = {5u, 0u, 0u, 0u, 0x44u, 0x33u, 0x22u, 0x11u,
        0u, 1u, 0u, 0u, 0u, 4u, 0u, 0u, 0xf8u, 0xffu, 0xffu, 0xffu,
        1u, 0u, 0u, 0u};
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.response + 25u, sizeof(expected));
}

static size_t begin_request(void) {
    uint8_t manifest[ESY_REMOTE_MANAGER_OTA_MANIFEST_BYTES] = {0};
    manifest[0] = 4u;
    manifest[4] = 3u;
    manifest[8] = 0x81u;
    manifest[40] = 0x82u;
    return request(ESY_REMOTE_MANAGER_GROUP_OTA, ESY_REMOTE_MANAGER_OTA_BEGIN,
                   manifest, sizeof(manifest));
}

void test_begin_checks_complete_success_response_capacity_and_authorization_before_side_effects(void) {
    size_t length = begin_request();
    size_t encoded = 99u;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_remote_manager_proto_bin_process(&s_ctx.manager,
        s_ctx.request, length, s_ctx.response, 24u, &encoded));
    TEST_ASSERT_EQUAL_UINT32(0u, encoded);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, 48u));
    TEST_ASSERT_EQUAL_UINT8(0xfdu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT8(24u, s_ctx.response[21]);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.auth_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.begins);
    s_ctx.auth_result = ESY_ERR_STATE;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.begins);
    s_ctx.auth_result = ESY_OK;
    TEST_ASSERT_EQUAL_UINT32(49u, process(length, 49u));
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.begins);
    TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.manifest.version);
    TEST_ASSERT_EQUAL_UINT8(0x81u, s_ctx.manifest.image_sha256[0]);
    TEST_ASSERT_EQUAL_UINT8(0x82u, s_ctx.manifest.signature[0]);
}

void test_every_image_mutation_has_typed_arguments_and_a_status_snapshot(void) {
    size_t length = begin_request();
    TEST_ASSERT_EQUAL_UINT32(49u, process(length, sizeof(s_ctx.response)));
    uint8_t write[] = {0x44u, 0x33u, 0x22u, 0x11u, 0u, 0u, 0u, 0u, 0xa5u, 1u, 2u};
    length = request(2u, ESY_REMOTE_MANAGER_OTA_WRITE, write, sizeof(write));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, 48u));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.writes);
    TEST_ASSERT_EQUAL_UINT32(49u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
    TEST_ASSERT_EQUAL_UINT8(3u, s_ctx.response[33]);
    for (uint16_t cmd = ESY_REMOTE_MANAGER_OTA_FINISH; cmd <= ESY_REMOTE_MANAGER_OTA_ACTIVATE; ++cmd) {
        length = request(2u, cmd, write, 4u);
        TEST_ASSERT_EQUAL_UINT32(25u, process(length, 48u));
        TEST_ASSERT_EQUAL_UINT32(49u, process(length, sizeof(s_ctx.response)));
    }
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.finishes);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.aborts);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.activates);
}

void test_metadata_shortage_never_runs_queries_and_does_not_write_partial_payload(void) {
    const uint8_t current[4] = {0};
    size_t length = request(2u, ESY_REMOTE_MANAGER_OTA_STATUS, current, sizeof(current));
    memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, 26u));
    TEST_ASSERT_EQUAL_UINT8(0xfdu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT8(24u, s_ctx.response[21]);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[25]);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.statuses);
}

void test_invalid_version_empty_begin_and_truncated_session_arguments_are_rejected(void) {
    size_t length = begin_request();
    size_t encoded = 0u;
    s_ctx.request[2] = 0xffu;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_remote_manager_proto_bin_process(
        &s_ctx.manager, s_ctx.request, length, s_ctx.response, sizeof(s_ctx.response), &encoded));
    length = request(2u, ESY_REMOTE_MANAGER_OTA_BEGIN, NULL, 0u);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_proto_bin_process(
        &s_ctx.manager, s_ctx.request, length, s_ctx.response, sizeof(s_ctx.response), &encoded));
    length = request(2u, ESY_REMOTE_MANAGER_OTA_STATUS, NULL, 0u);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_proto_bin_process(
        &s_ctx.manager, s_ctx.request, length, s_ctx.response, sizeof(s_ctx.response), &encoded));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.auth_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.begins);
}

void test_coredump_info_and_bounded_range_wire_share_one_record_identity(void) {
    size_t length = request(ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_INFO, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(53u, process(length, sizeof(s_ctx.response)));
    const uint8_t info[] = {1u, 0u, 0u, 0u, 6u, 0u, 0u, 0u, 0x44u, 0x33u, 0x22u, 0x11u,
        1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u};
    TEST_ASSERT_EQUAL_MEMORY(info, s_ctx.response + 25u, sizeof(info));
    const uint8_t range[] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
        9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 1u, 0u, 0u, 0u, 3u, 0u, 0u, 0u};
    length = request(ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_READ, range, sizeof(range));
    TEST_ASSERT_EQUAL_UINT32(28u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_MEMORY(k_record + 1u, s_ctx.response + 25u, 3u);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.reads);
}

void test_coredump_read_rejects_truncated_arguments_and_overflow_without_reading(void) {
    const uint8_t range[] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
        9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u,
        0xffu, 0xffu, 0xffu, 0xffu, 2u, 0u, 0u, 0u};
    size_t length = request(ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_READ, range, sizeof(range) - 1u);
    size_t encoded = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_proto_bin_process(&s_ctx.manager,
        s_ctx.request, length, s_ctx.response, sizeof(s_ctx.response), &encoded));
    length = request(ESY_REMOTE_MANAGER_GROUP_COREDUMP, ESY_REMOTE_MANAGER_COREDUMP_READ, range, sizeof(range));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xfeu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.reads);
}

void test_configuration_wire_returns_bounded_output_and_rejects_nul_keys(void) {
    size_t length = request(ESY_REMOTE_MANAGER_GROUP_CONFIG, ESY_REMOTE_MANAGER_CONFIG_GET,
        (const uint8_t *)"mode", 4u);
    s_ctx.request[12] = 4u;
    s_ctx.request[14] = 0u;
    TEST_ASSERT_EQUAL_UINT32(27u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_MEMORY("ok", s_ctx.response + 25u, 2u);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.config_reads);
    TEST_ASSERT_EQUAL_UINT8(1u, s_ctx.response[20]);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, 26u));
    TEST_ASSERT_EQUAL_UINT8(0xfdu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT8(2u, s_ctx.response[21]);
    TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.config_reads);
    const unsigned auth_count = s_ctx.auth_count;
    s_ctx.request[19] = 0u;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xfeu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.config_reads);
    TEST_ASSERT_EQUAL_UINT32(auth_count, s_ctx.auth_count);
}

void test_configured_response_budget_prevents_begin_even_when_wire_buffer_is_large(void) {
    const size_t length = begin_request();
    s_ctx.manager.config.max_response_value_length = 23u;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xfdu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT8(24u, s_ctx.response[21]);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.auth_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.begins);
}

void test_request_budget_counts_manifest_and_write_prefix_separately_from_response_budget(void) {
    size_t length = begin_request();
    s_ctx.manager.config.max_request_value_length = 103u;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xfeu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.begins);
    s_ctx.manager.config.max_request_value_length = 104u;
    TEST_ASSERT_EQUAL_UINT32(49u, process(length, sizeof(s_ctx.response)));
    uint8_t write[] = {0x44u, 0x33u, 0x22u, 0x11u, 0u, 0u, 0u, 0u, 0xa5u};
    s_ctx.manager.config.max_request_value_length = 8u;
    length = request(2u, ESY_REMOTE_MANAGER_OTA_WRITE, write, sizeof(write));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.writes);
    s_ctx.manager.config.max_request_value_length = 9u;
    TEST_ASSERT_EQUAL_UINT32(49u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.writes);
}

void test_unknown_image_command_and_unassigned_domain_never_mutate(void) {
    size_t length = request(2u, UINT16_MAX, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xfau, s_ctx.response[12]);
    s_ctx.request[0] = 0x01u;
    size_t encoded = 9u;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_remote_manager_proto_bin_process(
        &s_ctx.manager, s_ctx.request, length, s_ctx.response, sizeof(s_ctx.response), &encoded));
    TEST_ASSERT_EQUAL_UINT32(0u, encoded);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.begins + s_ctx.writes + s_ctx.finishes +
        s_ctx.aborts + s_ctx.activates);
}

void test_discovery_advertises_every_installed_image_operation_without_executing_it(void) {
    const size_t length = request(ESY_REMOTE_MANAGER_GROUP_SYSTEM,
                                  ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    const uint32_t bits = (uint32_t)s_ctx.response[16] |
        ((uint32_t)s_ctx.response[17] << 8u) |
        ((uint32_t)s_ctx.response[18] << 16u) |
        ((uint32_t)s_ctx.response[19] << 24u);
    const uint32_t image_bits = ESY_REMOTE_MANAGER_CAP_OTA_INFO |
        ESY_REMOTE_MANAGER_CAP_OTA_BEGIN | ESY_REMOTE_MANAGER_CAP_OTA_STATUS |
        ESY_REMOTE_MANAGER_CAP_OTA_WRITE | ESY_REMOTE_MANAGER_CAP_OTA_FINISH |
        ESY_REMOTE_MANAGER_CAP_OTA_ABORT | ESY_REMOTE_MANAGER_CAP_OTA_ACTIVATE;
    TEST_ASSERT_EQUAL_HEX32(image_bits, bits & image_bits);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.begins + s_ctx.writes + s_ctx.finishes +
        s_ctx.activates + s_ctx.aborts);
}

static size_t record_request(uint16_t command, const esy_coredump_record_id_t *id,
                              uint32_t offset, uint32_t length) {
    uint8_t value[ESY_REMOTE_MANAGER_COREDUMP_READ_BYTES] = {0};
    memcpy(value, id->bytes, ESY_COREDUMP_RECORD_ID_BYTES);
    for (size_t i = 0u; i < sizeof(uint32_t); ++i) {
        value[ESY_COREDUMP_RECORD_ID_BYTES + i] = (uint8_t)(offset >> (i * 8u));
        value[ESY_COREDUMP_RECORD_ID_BYTES + sizeof(uint32_t) + i] = (uint8_t)(length >> (i * 8u));
    }
    return request(ESY_REMOTE_MANAGER_GROUP_COREDUMP, command, value,
        command == ESY_REMOTE_MANAGER_COREDUMP_READ ? sizeof(value) : ESY_COREDUMP_RECORD_ID_BYTES);
}

void test_coredump_info_reserves_all_identity_bytes_before_provider_access(void) {
    const size_t length = request(ESY_REMOTE_MANAGER_GROUP_COREDUMP,
        ESY_REMOTE_MANAGER_COREDUMP_INFO, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, 52u));
    TEST_ASSERT_EQUAL_UINT8(0xfdu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT8(28u, s_ctx.response[21]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.auth_count);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.dump_infos);
    s_ctx.manager.config.max_response_value_length = 27u;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.dump_infos);
    s_ctx.manager.config.max_response_value_length = 28u;
    TEST_ASSERT_EQUAL_UINT32(53u, process(length, 53u));
    TEST_ASSERT_EQUAL_MEMORY(k_record_id.bytes, s_ctx.response + 37u, ESY_COREDUMP_RECORD_ID_BYTES);
}

void test_coredump_stale_wire_requests_cannot_collect_or_discard_a_new_record_with_same_crc(void) {
    const esy_coredump_record_id_t old_id = s_ctx.record_id;
    size_t length = record_request(ESY_REMOTE_MANAGER_COREDUMP_CLEAR, &old_id, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.discards);
    s_ctx.dump_available = true;
    s_ctx.record_id.bytes[ESY_COREDUMP_RECORD_ID_BYTES - 1u] ^= 0x80u;
    length = record_request(ESY_REMOTE_MANAGER_COREDUMP_READ, &old_id, 0u, 3u);
    memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xf6u, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.response[21]);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[25]);
    length = record_request(ESY_REMOTE_MANAGER_COREDUMP_READ, &old_id, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xf6u, s_ctx.response[12]);
    length = record_request(ESY_REMOTE_MANAGER_COREDUMP_CLEAR, &old_id, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xf6u, s_ctx.response[12]);
    TEST_ASSERT_TRUE(s_ctx.dump_available);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.discards);
    length = record_request(ESY_REMOTE_MANAGER_COREDUMP_READ, &s_ctx.record_id, 1u, 3u);
    TEST_ASSERT_EQUAL_UINT32(28u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_MEMORY(k_record + 1u, s_ctx.response + 25u, 3u);
}

void test_coredump_invalid_argument_lengths_are_rejected_without_dispatch(void) {
    const uint8_t bytes[ESY_REMOTE_MANAGER_COREDUMP_READ_BYTES] = {1u};
    const size_t read_sizes[] = {0u, 8u, 23u};
    size_t output_length;
    for (size_t i = 0u; i < sizeof(read_sizes) / sizeof(read_sizes[0]); ++i) {
        const size_t length = request(ESY_REMOTE_MANAGER_GROUP_COREDUMP,
            ESY_REMOTE_MANAGER_COREDUMP_READ, bytes, read_sizes[i]);
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_proto_bin_process(
            &s_ctx.manager, s_ctx.request, length, s_ctx.response, sizeof(s_ctx.response), &output_length));
        TEST_ASSERT_EQUAL_UINT32(0u, output_length);
    }
    const size_t clear_sizes[] = {0u, 15u, 17u};
    for (size_t i = 0u; i < sizeof(clear_sizes) / sizeof(clear_sizes[0]); ++i) {
        const size_t length = request(ESY_REMOTE_MANAGER_GROUP_COREDUMP,
            ESY_REMOTE_MANAGER_COREDUMP_CLEAR, bytes, clear_sizes[i]);
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_proto_bin_process(
            &s_ctx.manager, s_ctx.request, length, s_ctx.response, sizeof(s_ctx.response), &output_length));
        TEST_ASSERT_EQUAL_UINT32(0u, output_length);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.auth_count);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.reads + s_ctx.clears);
}

void test_coredump_zero_ids_and_authorization_denial_never_reach_providers(void) {
    const esy_coredump_record_id_t zero = {0};
    size_t length = record_request(ESY_REMOTE_MANAGER_COREDUMP_READ, &zero, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xfeu, s_ctx.response[12]);
    length = record_request(ESY_REMOTE_MANAGER_COREDUMP_CLEAR, &zero, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xfeu, s_ctx.response[12]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.auth_count);
    s_ctx.auth_result = ESY_ERR_FAIL;
    length = record_request(ESY_REMOTE_MANAGER_COREDUMP_READ, &s_ctx.record_id, 0u, 3u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT8(0xffu, s_ctx.response[12]);
    length = record_request(ESY_REMOTE_MANAGER_COREDUMP_CLEAR, &s_ctx.record_id, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.reads + s_ctx.clears + s_ctx.discards);
}
