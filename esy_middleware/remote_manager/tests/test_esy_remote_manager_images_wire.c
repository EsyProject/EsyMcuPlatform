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

#define TEST_REQUEST_BYTES 80u
#define TEST_RESPONSE_BYTES 96u
#define TEST_SUCCESS_BYTES 85u
#define TEST_IMAGE_ARGUMENT_BYTES 40u

typedef struct {
    esy_remote_manager_t manager;
    uint8_t request[TEST_REQUEST_BYTES];
    uint8_t response[TEST_RESPONSE_BYTES];
    esy_remote_manager_image_slot_t slot;
    esy_remote_manager_image_id_t identity;
    esy_err_t authorization_result;
    esy_err_t provider_result;
    unsigned authorizations;
    unsigned slots;
    unsigned confirmations;
    unsigned rollbacks;
    uint32_t requested_index;
} images_wire_ctx_t;

static images_wire_ctx_t s_ctx;
static const uint8_t k_image_argument[TEST_IMAGE_ARGUMENT_BYTES] = {
    0x78u, 0x56u, 0x34u, 0x12u, 0x44u, 0x33u, 0x22u, 0x11u,
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u,
    16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u,
};
static const uint8_t k_slot_argument[] = {1u, 0u, 0u, 0u};

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    images_wire_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4u, request->request_id);
    TEST_ASSERT_EQUAL_UINT32(0u, request->session_id);
    TEST_ASSERT_EQUAL_UINT32(0u, request->value_length);
    ++ctx->authorizations;
    return ctx->authorization_result;
}

static esy_err_t query_slot(void *context, uint32_t index,
                            esy_remote_manager_image_slot_t *slot) {
    images_wire_ctx_t *ctx = context;
    ++ctx->slots;
    ctx->requested_index = index;
    if (ctx->provider_result != ESY_OK) return ctx->provider_result;
    if (index >= ctx->slot.slot_count &&
        !(index == 0u && ctx->slot.slot_count == 0u)) return ESY_ERR_INVALID_ARG;
    *slot = ctx->slot;
    slot->index = index;
    return ESY_OK;
}

static esy_err_t confirm_image(void *context,
                               const esy_remote_manager_image_id_t *identity,
                               esy_remote_manager_image_slot_t *slot) {
    images_wire_ctx_t *ctx = context;
    ++ctx->confirmations;
    ctx->identity = *identity;
    *slot = ctx->slot;
    slot->flags |= ESY_REMOTE_MANAGER_IMAGE_RUNNING |
                   ESY_REMOTE_MANAGER_IMAGE_CONFIRMED;
    return ctx->provider_result;
}

static esy_err_t rollback_image(void *context,
                                const esy_remote_manager_image_id_t *identity,
                                esy_remote_manager_image_slot_t *slot) {
    images_wire_ctx_t *ctx = context;
    ++ctx->rollbacks;
    ctx->identity = *identity;
    *slot = ctx->slot;
    slot->flags |= ESY_REMOTE_MANAGER_IMAGE_PENDING;
    return ctx->provider_result;
}

static size_t make_request(uint16_t command, const uint8_t *argument,
                            size_t length) {
    memset(s_ctx.request, 0, sizeof(s_ctx.request));
    const uint8_t header[] = {
        ESY_PROTO_DOMAIN_REMOTE_MANAGER, 1u, 1u, 0u, 2u, 0u,
        (uint8_t)command, (uint8_t)(command >> 8u),
        0xd4u, 0xc3u, 0xb2u, 0xa1u, 0u, 0u, (uint8_t)length, 0u, 0u, 0u,
    };
    memcpy(s_ctx.request, header, sizeof(header));
    if (length != 0u) memcpy(s_ctx.request + sizeof(header), argument, length);
    return sizeof(header) + length;
}

static size_t process(size_t request_length, size_t capacity) {
    size_t output_length = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_proto_bin_process(
        &s_ctx.manager, s_ctx.request, request_length, s_ctx.response, capacity,
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
    s_ctx.slot = (esy_remote_manager_image_slot_t){
        .slot_count = 3u, .index = 1u,
        .image = {.slot = 0x12345678u, .version = 0x11223344u},
        .capacity = 0x01000000u, .image_size = 0x00123456u,
        .flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT | ESY_REMOTE_MANAGER_IMAGE_BOOTABLE,
    };
    memcpy(s_ctx.slot.image.sha256, k_image_argument + 8u, ESY_IMAGE_SHA256_BYTES);
    const esy_remote_manager_config_t config = {
        .max_key_length = 4u, .max_request_value_length = 40u,
        .max_response_value_length = 60u,
        .authorize = authorize, .authorize_context = &s_ctx,
        .ota_ops = {.images = {.slot = query_slot, .confirm = confirm_image,
            .rollback = rollback_image, .context = &s_ctx}},
    };
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}

void test_slot_wire_has_exact_v1_snapshot_with_logical_slot_distinct_from_index(void) {
    const size_t length = make_request(ESY_REMOTE_MANAGER_OTA_SLOT,
        k_slot_argument, sizeof(k_slot_argument));
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    const uint8_t expected[] = {
        2u, 2u, 1u, 0u, 2u, 0u, 8u, 0u, 0xd4u, 0xc3u, 0xb2u, 0xa1u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 60u, 0u, 0u, 0u,
        3u, 0u, 0u, 0u, 1u, 0u, 0u, 0u,
        0x78u, 0x56u, 0x34u, 0x12u, 0x44u, 0x33u, 0x22u, 0x11u,
        0u, 0u, 0u, 1u, 0x56u, 0x34u, 0x12u, 0u, 17u, 0u, 0u, 0u,
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u,
        16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u,
    };
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, sizeof(expected));
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.response, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.requested_index);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.slots);
}

void test_confirm_and_rollback_decode_exact_identity_and_return_distinct_boot_flags(void) {
    size_t length = make_request(ESY_REMOTE_MANAGER_OTA_CONFIRM,
        k_image_argument, sizeof(k_image_argument));
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, s_ctx.identity.slot);
    TEST_ASSERT_EQUAL_HEX32(0x11223344u, s_ctx.identity.version);
    TEST_ASSERT_EQUAL_MEMORY(k_image_argument + 8u, s_ctx.identity.sha256,
                              ESY_IMAGE_SHA256_BYTES);
    TEST_ASSERT_EQUAL_UINT8(27u, s_ctx.response[49]);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.confirmations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.rollbacks);
    length = make_request(ESY_REMOTE_MANAGER_OTA_ROLLBACK,
        k_image_argument, sizeof(k_image_argument));
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, TEST_SUCCESS_BYTES));
    TEST_ASSERT_EQUAL_UINT8(21u, s_ctx.response[49]);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.confirmations);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.rollbacks);
}

void test_all_image_commands_preflight_the_full_snapshot_before_authorization(void) {
    for (uint16_t command = ESY_REMOTE_MANAGER_OTA_SLOT;
         command <= ESY_REMOTE_MANAGER_OTA_ROLLBACK; ++command) {
        const bool slot = command == ESY_REMOTE_MANAGER_OTA_SLOT;
        const size_t length = make_request(command,
            slot ? k_slot_argument : k_image_argument,
            slot ? sizeof(k_slot_argument) : sizeof(k_image_argument));
        memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
        TEST_ASSERT_EQUAL_UINT32(25u, process(length, TEST_SUCCESS_BYTES - 1u));
        TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, response_status());
        TEST_ASSERT_EQUAL_UINT8(60u, s_ctx.response[21]);
        TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[25]);
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.slots + s_ctx.confirmations + s_ctx.rollbacks);
}

void test_configured_response_budget_prevents_image_mutation_with_large_wire_storage(void) {
    s_ctx.manager.config.max_response_value_length = 59u;
    const size_t length = make_request(ESY_REMOTE_MANAGER_OTA_ROLLBACK,
        k_image_argument, sizeof(k_image_argument));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, response_status());
    TEST_ASSERT_EQUAL_UINT8(60u, s_ctx.response[21]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.rollbacks);
}

void test_malformed_image_arguments_have_no_response_or_callbacks(void) {
    uint8_t argument[TEST_IMAGE_ARGUMENT_BYTES + 1u] = {0};
    for (uint16_t command = ESY_REMOTE_MANAGER_OTA_SLOT;
         command <= ESY_REMOTE_MANAGER_OTA_ROLLBACK; ++command) {
        const size_t valid = command == ESY_REMOTE_MANAGER_OTA_SLOT ? 4u : 40u;
        const size_t invalid_lengths[] = {0u, valid - 1u, valid + 1u};
        for (size_t i = 0u; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); ++i) {
            const size_t length = make_request(command, argument, invalid_lengths[i]);
            size_t output_length = 99u;
            memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
            TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
                esy_remote_manager_proto_bin_process(&s_ctx.manager, s_ctx.request,
                    length, s_ctx.response, sizeof(s_ctx.response), &output_length));
            TEST_ASSERT_EQUAL_UINT32(0u, output_length);
            TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[0]);
        }
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.slots + s_ctx.confirmations + s_ctx.rollbacks);
}

void test_authorization_denial_prevents_slot_queries_and_all_image_mutations(void) {
    s_ctx.authorization_result = ESY_ERR_STATE;
    for (uint16_t command = ESY_REMOTE_MANAGER_OTA_SLOT;
         command <= ESY_REMOTE_MANAGER_OTA_ROLLBACK; ++command) {
        const bool slot = command == ESY_REMOTE_MANAGER_OTA_SLOT;
        const size_t length = make_request(command,
            slot ? k_slot_argument : k_image_argument,
            slot ? sizeof(k_slot_argument) : sizeof(k_image_argument));
        memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
        TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
        TEST_ASSERT_EQUAL(ESY_ERR_STATE, response_status());
        TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.response[21]);
        TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[25]);
    }
    TEST_ASSERT_EQUAL_UINT(3u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.slots + s_ctx.confirmations + s_ctx.rollbacks);
}

void test_provider_failures_never_append_partial_slot_data(void) {
    s_ctx.provider_result = ESY_ERR_IO;
    for (uint16_t command = ESY_REMOTE_MANAGER_OTA_SLOT;
         command <= ESY_REMOTE_MANAGER_OTA_ROLLBACK; ++command) {
        const bool slot = command == ESY_REMOTE_MANAGER_OTA_SLOT;
        const size_t length = make_request(command,
            slot ? k_slot_argument : k_image_argument,
            slot ? sizeof(k_slot_argument) : sizeof(k_image_argument));
        memset(s_ctx.response, 0xa5, sizeof(s_ctx.response));
        TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
        TEST_ASSERT_EQUAL(ESY_ERR_IO, response_status());
        TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.response[21]);
        TEST_ASSERT_EQUAL_UINT8(0xa5u, s_ctx.response[25]);
    }
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.slots);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.confirmations);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.rollbacks);
}

void test_image_request_budget_counts_the_decoded_identity_and_index(void) {
    s_ctx.manager.config.max_request_value_length = 39u;
    size_t length = make_request(ESY_REMOTE_MANAGER_OTA_CONFIRM,
        k_image_argument, sizeof(k_image_argument));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response_status());
    s_ctx.manager.config.max_request_value_length = 3u;
    length = make_request(ESY_REMOTE_MANAGER_OTA_SLOT,
        k_slot_argument, sizeof(k_slot_argument));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response_status());
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.slots + s_ctx.confirmations + s_ctx.rollbacks);
}

void test_slot_index_outside_inventory_is_reported_without_a_fabricated_snapshot(void) {
    const uint8_t invalid_index[] = {0xffu, 0xffu, 0xffu, 0xffu};
    const size_t length = make_request(ESY_REMOTE_MANAGER_OTA_SLOT,
        invalid_index, sizeof(invalid_index));
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, response_status());
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, s_ctx.requested_index);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.slots);
}

void test_zero_slot_inventory_returns_an_explicit_all_zero_snapshot(void) {
    s_ctx.slot = (esy_remote_manager_image_slot_t){0};
    const uint8_t first[] = {0u, 0u, 0u, 0u};
    const uint8_t empty[ESY_REMOTE_MANAGER_PROTO_BIN_IMAGE_SLOT_BYTES] = {0};
    const size_t length = make_request(ESY_REMOTE_MANAGER_OTA_SLOT, first, sizeof(first));
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCESS_BYTES, process(length, sizeof(s_ctx.response)));
    TEST_ASSERT_EQUAL_MEMORY(empty, s_ctx.response + 25u, sizeof(empty));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.slots);
}

void test_discovery_reports_only_installed_boot_image_operations_without_executing_them(void) {
    const size_t length = make_request(ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, NULL, 0u);
    s_ctx.request[4] = ESY_REMOTE_MANAGER_GROUP_SYSTEM;
    TEST_ASSERT_EQUAL_UINT32(25u, process(length, sizeof(s_ctx.response)));
    const uint8_t expected[] = {1u, 0u, 14u, 0u};
    TEST_ASSERT_EQUAL_MEMORY(expected, s_ctx.response + 16u, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.authorizations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.slots + s_ctx.confirmations + s_ctx.rollbacks);
}
