/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"

#include <string.h>

#define TEST_IMAGE_CAPACITY 16384u
#define TEST_IMAGE_SIZE 4096u

typedef struct {
    esy_remote_manager_t manager;
    esy_remote_manager_config_t config;
    esy_remote_manager_image_slot_t slot;
    esy_remote_manager_image_id_t last_image;
    esy_err_t authorization;
    esy_err_t provider_result;
    uint32_t last_index;
    unsigned authorization_calls;
    unsigned slot_calls;
    unsigned confirm_calls;
    unsigned rollback_calls;
} remote_manager_images_test_ctx_t;

static remote_manager_images_test_ctx_t s_ctx;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    remote_manager_images_test_ctx_t *ctx = context;
    (void)request;
    ++ctx->authorization_calls;
    return ctx->authorization;
}

static esy_err_t image_slot(void *context, uint32_t index,
                            esy_remote_manager_image_slot_t *slot) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
    remote_manager_images_test_ctx_t *ctx = context;
    ++ctx->slot_calls;
    ctx->last_index = index;
    *slot = ctx->slot;
    return ctx->provider_result;
}

static esy_err_t image_confirm(void *context,
                               const esy_remote_manager_image_id_t *image,
                               esy_remote_manager_image_slot_t *slot) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
    remote_manager_images_test_ctx_t *ctx = context;
    ++ctx->confirm_calls;
    ctx->last_image = *image;
    *slot = ctx->slot;
    return ctx->provider_result;
}

static esy_err_t image_rollback(void *context,
                                const esy_remote_manager_image_id_t *image,
                                esy_remote_manager_image_slot_t *slot) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
    remote_manager_images_test_ctx_t *ctx = context;
    ++ctx->rollback_calls;
    ctx->last_image = *image;
    *slot = ctx->slot;
    return ctx->provider_result;
}

static esy_remote_manager_request_t request_for(uint16_t command) {
    esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_OTA, .command = command};
    if (command == ESY_REMOTE_MANAGER_OTA_SLOT)
        request.slot_index = s_ctx.slot.index;
    else if (command == ESY_REMOTE_MANAGER_OTA_CONFIRM ||
             command == ESY_REMOTE_MANAGER_OTA_ROLLBACK)
        request.image = s_ctx.slot.image;
    return request;
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

static void assert_same_image(const esy_remote_manager_image_id_t *expected,
                              const esy_remote_manager_image_id_t *actual) {
    TEST_ASSERT_EQUAL_UINT32(expected->slot, actual->slot);
    TEST_ASSERT_EQUAL_UINT32(expected->version, actual->version);
    TEST_ASSERT_EQUAL_MEMORY(expected->sha256, actual->sha256,
                             ESY_IMAGE_SHA256_BYTES);
}

static void assert_no_image_result(const esy_remote_manager_response_t *response) {
    const uint8_t empty_digest[ESY_IMAGE_SHA256_BYTES] = {0};
    TEST_ASSERT_EQUAL_UINT32(0u, response->image_slot.slot_count);
    TEST_ASSERT_EQUAL_UINT32(0u, response->image_slot.index);
    TEST_ASSERT_EQUAL_UINT32(0u, response->image_slot.image.slot);
    TEST_ASSERT_EQUAL_UINT32(0u, response->image_slot.image.version);
    TEST_ASSERT_EQUAL_MEMORY(empty_digest, response->image_slot.image.sha256,
                             sizeof(empty_digest));
    TEST_ASSERT_EQUAL_UINT32(0u, response->image_slot.capacity);
    TEST_ASSERT_EQUAL_UINT32(0u, response->image_slot.image_size);
    TEST_ASSERT_EQUAL_UINT32(0u, response->image_slot.flags);
    TEST_ASSERT_EQUAL_UINT(0u, response->value_length);
}

static void expect_invalid_request(const esy_remote_manager_request_t *request) {
    esy_remote_manager_response_t response;
    const unsigned before = s_ctx.authorization_calls;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, dispatch(request, &response));
    TEST_ASSERT_EQUAL_UINT(before, s_ctx.authorization_calls);
    assert_no_image_result(&response);
}

static void expect_invalid_snapshot(const esy_remote_manager_request_t *request) {
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(request, &response));
    assert_no_image_result(&response);
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.slot = (esy_remote_manager_image_slot_t){
        .slot_count = 3u, .index = 1u,
        .image = {.slot = 73u, .version = 0x01020304u},
        .capacity = TEST_IMAGE_CAPACITY, .image_size = TEST_IMAGE_SIZE,
        .flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT |
                 ESY_REMOTE_MANAGER_IMAGE_BOOTABLE};
    for (size_t i = 0u; i < sizeof(s_ctx.slot.image.sha256); ++i)
        s_ctx.slot.image.sha256[i] = (uint8_t)(0xa0u + i);
    s_ctx.config = (esy_remote_manager_config_t){
        .max_key_length = 8u,
        .max_request_value_length = ESY_REMOTE_MANAGER_OTA_IMAGE_ID_BYTES,
        .max_response_value_length = 0u,
        .authorize = authorize, .authorize_context = &s_ctx,
        .ota_ops = {.images = {.slot = image_slot, .confirm = image_confirm,
                              .rollback = image_rollback, .context = &s_ctx}}};
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
}

void test_images_enumerate_sparse_logical_slots_without_an_output_buffer(void) {
    initialize();
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(3u, response.image_slot.slot_count);
    TEST_ASSERT_EQUAL_UINT32(1u, response.image_slot.index);
    TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_CAPACITY, response.image_slot.capacity);
    TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_SIZE, response.image_slot.image_size);
    assert_same_image(&s_ctx.slot.image, &response.image_slot.image);
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.last_index);
    TEST_ASSERT_EQUAL_UINT(0u, response.value_length);
    s_ctx.slot = (esy_remote_manager_image_slot_t){
        .slot_count = 3u, .index = 2u, .image = {.slot = 901u},
        .capacity = TEST_IMAGE_CAPACITY};
    request.slot_index = 2u;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(901u, response.image_slot.image.slot);
    TEST_ASSERT_EQUAL_UINT32(0u, response.image_slot.flags);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.slot_calls);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.confirm_calls + s_ctx.rollback_calls);
}

void test_images_allow_an_empty_inventory_only_at_index_zero(void) {
    s_ctx.slot = (esy_remote_manager_image_slot_t){0};
    initialize();
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    assert_no_image_result(&response);
    request.slot_index = 1u;
    expect_invalid_snapshot(&request);
    request.slot_index = 0u;
    s_ctx.slot.capacity = TEST_IMAGE_CAPACITY;
    expect_invalid_snapshot(&request);
    s_ctx.slot.capacity = 0u;
    s_ctx.slot.image.slot = 1u;
    expect_invalid_snapshot(&request);
}

void test_images_confirm_and_rollback_preserve_the_explicit_target_identity(void) {
    initialize();
    s_ctx.slot.flags |= ESY_REMOTE_MANAGER_IMAGE_RUNNING |
                        ESY_REMOTE_MANAGER_IMAGE_CONFIRMED;
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    assert_same_image(&request.image, &s_ctx.last_image);
    assert_same_image(&request.image, &response.image_slot.image);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.confirm_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.slot_calls + s_ctx.rollback_calls);
    s_ctx.slot.flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT |
                       ESY_REMOTE_MANAGER_IMAGE_BOOTABLE |
                       ESY_REMOTE_MANAGER_IMAGE_PENDING;
    request.command = ESY_REMOTE_MANAGER_OTA_ROLLBACK;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    assert_same_image(&request.image, &s_ctx.last_image);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.rollback_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.slot_calls);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.authorization_calls);
}

void test_images_discovery_and_unsupported_commands_follow_the_installed_subset(void) {
    s_ctx.config.ota_ops.images.confirm = NULL;
    s_ctx.config.ota_ops.images.rollback = NULL;
    initialize();
    esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_REMOTE_MANAGER_CAP_DISCOVER |
        ESY_REMOTE_MANAGER_CAP_OTA_SLOT, response.capabilities);
    request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
    request.command = ESY_REMOTE_MANAGER_OTA_ROLLBACK;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
    request = request_for(UINT16_MAX);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(4u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u,
        s_ctx.slot_calls + s_ctx.confirm_calls + s_ctx.rollback_calls);
}

void test_images_discovery_reports_all_image_callbacks_without_executing_them(void) {
    initialize();
    const esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(ESY_REMOTE_MANAGER_CAP_DISCOVER |
        ESY_REMOTE_MANAGER_CAP_OTA_SLOT | ESY_REMOTE_MANAGER_CAP_OTA_CONFIRM |
        ESY_REMOTE_MANAGER_CAP_OTA_ROLLBACK, response.capabilities);
    TEST_ASSERT_EQUAL_UINT(0u,
        s_ctx.slot_calls + s_ctx.confirm_calls + s_ctx.rollback_calls);
}

void test_images_authorize_every_enumeration_confirmation_and_rollback(void) {
    s_ctx.authorization = ESY_ERR_FAIL;
    initialize();
    esy_remote_manager_response_t response;
    for (uint16_t command = ESY_REMOTE_MANAGER_OTA_SLOT;
         command <= ESY_REMOTE_MANAGER_OTA_ROLLBACK; ++command) {
        const esy_remote_manager_request_t request = request_for(command);
        TEST_ASSERT_EQUAL(ESY_ERR_FAIL, dispatch(&request, &response));
        assert_no_image_result(&response);
    }
    TEST_ASSERT_EQUAL_UINT(3u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(0u,
        s_ctx.slot_calls + s_ctx.confirm_calls + s_ctx.rollback_calls);
}

void test_images_request_budgets_count_the_index_or_complete_image_identity(void) {
    s_ctx.config.max_request_value_length = ESY_REMOTE_MANAGER_OTA_SLOT_INDEX_BYTES - 1u;
    initialize();
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    esy_remote_manager_response_t response;
    expect_invalid_request(&request);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    s_ctx.config.max_request_value_length = ESY_REMOTE_MANAGER_OTA_SLOT_INDEX_BYTES;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    expect_invalid_request(&request);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    s_ctx.config.max_request_value_length = ESY_REMOTE_MANAGER_OTA_IMAGE_ID_BYTES - 1u;
    initialize();
    expect_invalid_request(&request);
    request.command = ESY_REMOTE_MANAGER_OTA_ROLLBACK;
    expect_invalid_request(&request);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.slot_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.confirm_calls + s_ctx.rollback_calls);
}

void test_images_reject_unused_arguments_before_authorization(void) {
    initialize();
    esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    request.image.sha256[0] = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    request.image.slot = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    request.slot_index = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    request.session_id = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_OTA_ROLLBACK);
    request.offset = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    request.length = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    request.manifest.version = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    request.key = "x";
    request.key_length = 1u;
    expect_invalid_request(&request);
    request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    request.value = (const uint8_t *)"x";
    request.value_length = 1u;
    expect_invalid_request(&request);
    request = (esy_remote_manager_request_t){
        .group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER, .slot_index = 1u};
    expect_invalid_request(&request);
    request.slot_index = 0u;
    request.image.version = 1u;
    expect_invalid_request(&request);
    TEST_ASSERT_EQUAL_UINT(0u,
        s_ctx.slot_calls + s_ctx.confirm_calls + s_ctx.rollback_calls);
}

void test_images_provider_errors_return_no_stale_result_and_never_retry(void) {
    s_ctx.provider_result = ESY_ERR_IO;
    initialize();
    esy_remote_manager_response_t response;
    for (uint16_t command = ESY_REMOTE_MANAGER_OTA_SLOT;
         command <= ESY_REMOTE_MANAGER_OTA_ROLLBACK; ++command) {
        const esy_remote_manager_request_t request = request_for(command);
        memset(&response, 0xa5, sizeof(response));
        TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, &response));
        assert_no_image_result(&response);
    }
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.slot_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.confirm_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.rollback_calls);
}

void test_images_reject_invalid_provider_inventory_bounds_and_flag_bits(void) {
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    const esy_remote_manager_image_slot_t original = s_ctx.slot;
    s_ctx.slot.index = s_ctx.slot.slot_count;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.index = 0u;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.capacity = 0u;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.image_size = s_ctx.slot.capacity + 1u;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.flags |= 1u << 31u;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.image_size = 0u;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.slot_count = 0u;
    expect_invalid_snapshot(&request);
}

void test_images_empty_slots_cannot_claim_an_image_or_boot_state(void) {
    s_ctx.slot = (esy_remote_manager_image_slot_t){
        .slot_count = 1u, .index = 0u,
        .image = {.slot = 0u}, .capacity = TEST_IMAGE_CAPACITY};
    const esy_remote_manager_image_slot_t empty = s_ctx.slot;
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_SLOT);
    s_ctx.slot.image.version = 1u;
    expect_invalid_snapshot(&request);
    s_ctx.slot = empty;
    s_ctx.slot.image.sha256[ESY_IMAGE_SHA256_BYTES - 1u] = 1u;
    expect_invalid_snapshot(&request);
    s_ctx.slot = empty;
    s_ctx.slot.image_size = 1u;
    expect_invalid_snapshot(&request);
    for (uint32_t flag = ESY_REMOTE_MANAGER_IMAGE_RUNNING;
         flag <= ESY_REMOTE_MANAGER_IMAGE_BOOTABLE; flag <<= 1u) {
        s_ctx.slot = empty;
        s_ctx.slot.flags = flag;
        expect_invalid_snapshot(&request);
    }
}

void test_images_confirm_results_must_match_the_target_and_confirm_running_state(void) {
    s_ctx.slot.flags |= ESY_REMOTE_MANAGER_IMAGE_RUNNING |
                        ESY_REMOTE_MANAGER_IMAGE_CONFIRMED;
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    const esy_remote_manager_image_slot_t original = s_ctx.slot;
    ++s_ctx.slot.image.slot;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    ++s_ctx.slot.image.version;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.image.sha256[ESY_IMAGE_SHA256_BYTES - 1u] ^= 1u;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.flags &= ~ESY_REMOTE_MANAGER_IMAGE_RUNNING;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.flags &= ~ESY_REMOTE_MANAGER_IMAGE_CONFIRMED;
    expect_invalid_snapshot(&request);
    s_ctx.slot = (esy_remote_manager_image_slot_t){0};
    expect_invalid_snapshot(&request);
}

void test_images_rollback_results_must_be_bootable_pending_and_not_running(void) {
    s_ctx.slot.flags |= ESY_REMOTE_MANAGER_IMAGE_PENDING;
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_ROLLBACK);
    const esy_remote_manager_image_slot_t original = s_ctx.slot;
    s_ctx.slot.flags &= ~ESY_REMOTE_MANAGER_IMAGE_PENDING;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.flags &= ~ESY_REMOTE_MANAGER_IMAGE_BOOTABLE;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    s_ctx.slot.flags |= ESY_REMOTE_MANAGER_IMAGE_RUNNING;
    expect_invalid_snapshot(&request);
    s_ctx.slot = original;
    ++s_ctx.slot.image.version;
    expect_invalid_snapshot(&request);
}

void test_images_zero_version_and_digest_are_identity_values_not_sentinels(void) {
    s_ctx.slot = (esy_remote_manager_image_slot_t){
        .slot_count = 1u, .index = 0u,
        .capacity = TEST_IMAGE_CAPACITY, .image_size = TEST_IMAGE_SIZE,
        .flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT |
                 ESY_REMOTE_MANAGER_IMAGE_RUNNING |
                 ESY_REMOTE_MANAGER_IMAGE_CONFIRMED};
    initialize();
    const esy_remote_manager_request_t request = request_for(ESY_REMOTE_MANAGER_OTA_CONFIRM);
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    assert_same_image(&request.image, &response.image_slot.image);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.confirm_calls);
}
