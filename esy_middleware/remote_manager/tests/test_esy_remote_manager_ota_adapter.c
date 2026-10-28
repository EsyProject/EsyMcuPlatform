/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/remote_manager/esy_remote_manager_ota_adapter.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "unity.h"
#include <string.h>

#define TEST_CHUNK_BYTES 8u

typedef struct {
    unsigned slots;
    unsigned confirmations;
    unsigned rollbacks;
    uint32_t index;
    esy_remote_manager_image_id_t image;
    esy_err_t result;
} image_owner_test_ctx_t;

typedef struct {
    esy_ota_t ota;
    esy_remote_manager_t manager;
    esy_remote_manager_ota_adapter_t adapter;
    esy_remote_manager_ota_adapter_config_t binding;
    esy_ota_static_resources_t resources;
    uint8_t last_chunk[TEST_CHUNK_BYTES];
    uint8_t image[TEST_CHUNK_BYTES];
    uint32_t next_id;
    unsigned begins;
    unsigned writes;
    unsigned finishes;
    unsigned activations;
    unsigned aborts;
    esy_err_t abort_result;
    image_owner_test_ctx_t image_owner;
} ota_adapter_test_ctx_t;

static ota_adapter_test_ctx_t s_ctx;

static esy_err_t provider_begin(void *context, const esy_image_manifest_t *manifest) {
    ota_adapter_test_ctx_t *ctx = context;
    ++ctx->begins;
    return manifest->image_size <= sizeof(ctx->image) ? ESY_OK : ESY_ERR_INVALID_ARG;
}
static esy_err_t provider_write(void *context, uint32_t offset,
                                 const uint8_t *data, size_t length) {
    ota_adapter_test_ctx_t *ctx = context;
    ++ctx->writes;
    memcpy(ctx->image + offset, data, length);
    return ESY_OK;
}
static esy_err_t provider_finish(void *context) {
    ++((ota_adapter_test_ctx_t *)context)->finishes;
    return ESY_OK;
}
static esy_err_t provider_activate(void *context) {
    ++((ota_adapter_test_ctx_t *)context)->activations;
    return ESY_OK;
}
static esy_err_t provider_abort(void *context) {
    ota_adapter_test_ctx_t *ctx = context;
    ++ctx->aborts;
    return ctx->abort_result;
}
static esy_err_t allocate_id(void *context, uint32_t *id) {
    *id = ++((ota_adapter_test_ctx_t *)context)->next_id;
    return ESY_OK;
}
static esy_err_t owner_info(void *context, esy_remote_manager_ota_info_t *info) {
    (void)context;
    *info = (esy_remote_manager_ota_info_t){1u, -1, TEST_CHUNK_BYTES, TEST_CHUNK_BYTES};
    return ESY_OK;
}
static esy_err_t allow(void *context, const esy_remote_manager_request_t *request) {
    (void)context;
    (void)request;
    return ESY_OK;
}

static esy_err_t owner_slot(void *context, uint32_t index,
                            esy_remote_manager_image_slot_t *slot) {
    image_owner_test_ctx_t *owner = context;
    TEST_ASSERT_EQUAL_PTR(&s_ctx.image_owner, owner);
    ++owner->slots;
    owner->index = index;
    if (owner->result != ESY_OK) return owner->result;
    *slot = (esy_remote_manager_image_slot_t){
        .slot_count = 3u, .index = index,
        .image = {.slot = 20u + index, .version = 5u, .sha256 = {0xA5u}},
        .capacity = 1024u, .image_size = 400u,
        .flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT | ESY_REMOTE_MANAGER_IMAGE_BOOTABLE};
    return ESY_OK;
}

static esy_err_t owner_confirm(void *context,
                               const esy_remote_manager_image_id_t *image,
                               esy_remote_manager_image_slot_t *slot) {
    image_owner_test_ctx_t *owner = context;
    TEST_ASSERT_EQUAL_PTR(&s_ctx.image_owner, owner);
    ++owner->confirmations;
    owner->image = *image;
    if (owner->result != ESY_OK) return owner->result;
    *slot = (esy_remote_manager_image_slot_t){
        .slot_count = 3u, .index = 1u, .image = *image,
        .capacity = 1024u, .image_size = 400u,
        .flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT | ESY_REMOTE_MANAGER_IMAGE_RUNNING |
            ESY_REMOTE_MANAGER_IMAGE_CONFIRMED | ESY_REMOTE_MANAGER_IMAGE_BOOTABLE};
    return ESY_OK;
}

static esy_err_t owner_rollback(void *context,
                                const esy_remote_manager_image_id_t *image,
                                esy_remote_manager_image_slot_t *slot) {
    image_owner_test_ctx_t *owner = context;
    TEST_ASSERT_EQUAL_PTR(&s_ctx.image_owner, owner);
    ++owner->rollbacks;
    owner->image = *image;
    if (owner->result != ESY_OK) return owner->result;
    *slot = (esy_remote_manager_image_slot_t){
        .slot_count = 3u, .index = 1u, .image = *image,
        .capacity = 1024u, .image_size = 400u,
        .flags = ESY_REMOTE_MANAGER_IMAGE_PRESENT | ESY_REMOTE_MANAGER_IMAGE_PENDING |
            ESY_REMOTE_MANAGER_IMAGE_BOOTABLE};
    return ESY_OK;
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.resources = (esy_ota_static_resources_t){s_ctx.last_chunk, sizeof(s_ctx.last_chunk)};
    const esy_ota_config_t core = {
        .provider = {.begin = provider_begin, .write = provider_write,
            .finish = provider_finish, .activate = provider_activate,
            .abort = provider_abort, .context = &s_ctx},
        .max_image_bytes = TEST_CHUNK_BYTES, .max_chunk_bytes = TEST_CHUNK_BYTES,
        .static_resources = &s_ctx.resources, .new_session_id = allocate_id,
        .session_id_context = &s_ctx};
    TEST_ASSERT_EQUAL(ESY_OK, esy_ota_init(&s_ctx.ota, &core));
    s_ctx.binding = (esy_remote_manager_ota_adapter_config_t){
        .ota = &s_ctx.ota, .info = owner_info, .info_context = &s_ctx};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_init(&s_ctx.adapter, &s_ctx.binding));
    esy_remote_manager_config_t config = {.max_key_length = 1u,
        .max_request_value_length = 112u, .max_response_value_length = 24u,
        .authorize = allow};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_get_ops(&s_ctx.adapter, &config.ota_ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
}
void tearDown(void) {
    s_ctx.abort_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_ota_deinit(&s_ctx.ota));
}
static esy_err_t dispatch(esy_remote_manager_request_t *request,
                            esy_remote_manager_response_t *response) {
    request->group = ESY_REMOTE_MANAGER_GROUP_OTA;
    return esy_remote_manager_dispatch(&s_ctx.manager, request, NULL, 0u, response);
}

static void bind_image_owner(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_deinit(&s_ctx.adapter));
    s_ctx.binding.images = (esy_remote_manager_image_ops_t){
        .slot = owner_slot, .confirm = owner_confirm, .rollback = owner_rollback,
        .context = &s_ctx.image_owner};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_init(&s_ctx.adapter, &s_ctx.binding));
    esy_remote_manager_config_t config = {.max_key_length = 1u,
        .max_request_value_length = 112u, .max_response_value_length = 0u,
        .authorize = allow};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_get_ops(&s_ctx.adapter, &config.ota_ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
}

void test_real_ota_adapter_uploads_repeats_finishes_and_activates_without_reset(void) {
    esy_remote_manager_request_t request = {.command = ESY_REMOTE_MANAGER_OTA_BEGIN,
        .manifest = {.version = 2u, .image_size = 4u}};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    const uint32_t id = response.ota_status.session_id;
    TEST_ASSERT_EQUAL(ESY_IMAGE_RECEIVING, response.ota_status.phase);
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.begins);
    const uint8_t bytes[] = {1u, 2u, 3u, 4u};
    request = (esy_remote_manager_request_t){.command = ESY_REMOTE_MANAGER_OTA_WRITE,
        .session_id = id, .value = bytes, .value_length = sizeof(bytes)};
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.writes);
    TEST_ASSERT_EQUAL_UINT32(sizeof(bytes), response.ota_status.next_offset);
    TEST_ASSERT_EQUAL_MEMORY(bytes, s_ctx.image, sizeof(bytes));
    request = (esy_remote_manager_request_t){.command = ESY_REMOTE_MANAGER_OTA_FINISH,
        .session_id = id};
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL(ESY_IMAGE_READY, response.ota_status.phase);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.finishes);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.activations);
    request.command = ESY_REMOTE_MANAGER_OTA_ACTIVATE;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL(ESY_IMAGE_ACTIVATED, response.ota_status.phase);
    TEST_ASSERT_TRUE(response.ota_status.result_valid);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.activations);
}

void test_real_ota_adapter_rejects_stale_status_and_preserves_failed_abort_for_retry(void) {
    esy_remote_manager_request_t request = {.command = ESY_REMOTE_MANAGER_OTA_BEGIN,
        .manifest = {.version = 2u, .image_size = 4u}};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    const uint32_t id = response.ota_status.session_id;
    request = (esy_remote_manager_request_t){.command = ESY_REMOTE_MANAGER_OTA_STATUS,
        .session_id = id + 1u};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, dispatch(&request, &response));
    request.session_id = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(id, response.ota_status.session_id);
    request.command = ESY_REMOTE_MANAGER_OTA_ABORT;
    request.session_id = id;
    s_ctx.abort_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.aborts);
    s_ctx.abort_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL(ESY_IMAGE_ABORTED, response.ota_status.phase);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.aborts);
}

void test_adapter_lifecycle_detaches_without_changing_the_borrowed_ota_core(void) {
    esy_remote_manager_ota_ops_t ops;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_ota_adapter_init(NULL, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_remote_manager_ota_adapter_init(&s_ctx.adapter, &s_ctx.binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_remote_manager_ota_adapter_get_ops(&s_ctx.adapter, &ops));
    esy_image_status_t status;
    TEST_ASSERT_EQUAL(ESY_OK, esy_ota_status(&s_ctx.ota, &status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_IDLE, status.phase);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.aborts);
}

void test_image_provider_is_optional_and_absent_callbacks_are_not_advertised(void) {
    esy_remote_manager_ota_ops_t ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_get_ops(&s_ctx.adapter, &ops));
    TEST_ASSERT_NULL(ops.images.slot);
    TEST_ASSERT_NULL(ops.images.confirm);
    TEST_ASSERT_NULL(ops.images.rollback);
    esy_remote_manager_request_t request = {.group = ESY_REMOTE_MANAGER_GROUP_SYSTEM,
        .command = ESY_REMOTE_MANAGER_SYSTEM_DISCOVER};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(&s_ctx.manager,
        &request, NULL, 0u, &response));
    const uint32_t image_capabilities = ESY_REMOTE_MANAGER_CAP_OTA_SLOT |
        ESY_REMOTE_MANAGER_CAP_OTA_CONFIRM | ESY_REMOTE_MANAGER_CAP_OTA_ROLLBACK;
    TEST_ASSERT_EQUAL_HEX32(0u, response.capabilities & image_capabilities);
    request.command = ESY_REMOTE_MANAGER_OTA_SLOT;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.begins);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.activations);
}

void test_image_provider_binding_copies_callbacks_and_the_independent_context(void) {
    bind_image_owner();
    esy_remote_manager_ota_ops_t ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_get_ops(&s_ctx.adapter, &ops));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.adapter, ops.context);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.image_owner, ops.images.context);
    TEST_ASSERT_TRUE(ops.images.slot == owner_slot);
    TEST_ASSERT_TRUE(ops.images.confirm == owner_confirm);
    TEST_ASSERT_TRUE(ops.images.rollback == owner_rollback);
    s_ctx.binding.images = (esy_remote_manager_image_ops_t){0};
    esy_remote_manager_image_slot_t slot;
    TEST_ASSERT_EQUAL(ESY_OK, ops.images.slot(ops.images.context, 2u, &slot));
    TEST_ASSERT_EQUAL_UINT32(22u, slot.image.slot);
    TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.image_owner.index);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_ota_adapter_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.image_owner.slots);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.image_owner.confirmations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.image_owner.rollbacks);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.aborts);
    esy_image_status_t status;
    TEST_ASSERT_EQUAL(ESY_OK, esy_ota_status(&s_ctx.ota, &status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_IDLE, status.phase);
}

void test_image_operations_dispatch_without_an_upload_or_opaque_response_budget(void) {
    bind_image_owner();
    esy_remote_manager_request_t request = {
        .command = ESY_REMOTE_MANAGER_OTA_SLOT, .slot_index = 1u};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_UINT32(3u, response.image_slot.slot_count);
    TEST_ASSERT_EQUAL_UINT32(21u, response.image_slot.image.slot);
    const esy_remote_manager_image_id_t identity = response.image_slot.image;
    request = (esy_remote_manager_request_t){
        .command = ESY_REMOTE_MANAGER_OTA_CONFIRM, .image = identity};
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_MEMORY(&identity, &s_ctx.image_owner.image, sizeof(identity));
    TEST_ASSERT_TRUE((response.image_slot.flags & ESY_REMOTE_MANAGER_IMAGE_CONFIRMED) != 0u);
    request.command = ESY_REMOTE_MANAGER_OTA_ROLLBACK;
    TEST_ASSERT_EQUAL(ESY_OK, dispatch(&request, &response));
    TEST_ASSERT_EQUAL_MEMORY(&identity, &s_ctx.image_owner.image, sizeof(identity));
    TEST_ASSERT_TRUE((response.image_slot.flags & ESY_REMOTE_MANAGER_IMAGE_PENDING) != 0u);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.image_owner.slots);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.image_owner.confirmations);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.image_owner.rollbacks);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.begins);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.activations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.aborts);
    esy_image_status_t status;
    TEST_ASSERT_EQUAL(ESY_OK, esy_ota_status(&s_ctx.ota, &status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_IDLE, status.phase);
}

void test_image_provider_errors_are_preserved_without_touching_upload_state(void) {
    bind_image_owner();
    s_ctx.image_owner.result = ESY_ERR_IO;
    const uint16_t commands[] = {ESY_REMOTE_MANAGER_OTA_SLOT,
        ESY_REMOTE_MANAGER_OTA_CONFIRM, ESY_REMOTE_MANAGER_OTA_ROLLBACK};
    esy_remote_manager_request_t request = {0};
    esy_remote_manager_response_t response;
    for (size_t i = 0u; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        request.command = commands[i];
        if (request.command != ESY_REMOTE_MANAGER_OTA_SLOT) {
            request.image = (esy_remote_manager_image_id_t){
                .slot = 21u, .version = 5u, .sha256 = {0xA5u}};
        }
        TEST_ASSERT_EQUAL(ESY_ERR_IO, dispatch(&request, &response));
    }
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.image_owner.slots);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.image_owner.confirmations);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.image_owner.rollbacks);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.begins);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.activations);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.aborts);
}
