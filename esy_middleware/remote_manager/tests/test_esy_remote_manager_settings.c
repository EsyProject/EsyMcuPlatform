/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_settings.h"
#include "fake_esy_hal_flash.h"
#include "esy_middleware/file/esy_file_dual_flash_store.h"

#include <string.h>

#define TEST_SETTINGS_BYTES 64u
#define TEST_KEY_BYTES 8u
#define TEST_PROGRAM_BYTES 32u

typedef struct {
    esy_settings_t settings;
    esy_dual_flash_store_t dual_flash_store;
    esy_file_t file;
    esy_file_dual_flash_store_t file_adapter;
    esy_remote_manager_settings_t adapter;
    esy_remote_manager_settings_config_t adapter_config;
    esy_remote_manager_settings_static_resources_t adapter_resources;
    esy_remote_manager_config_ops_t ops;
    esy_remote_manager_t manager;
    uint8_t work[TEST_SETTINGS_BYTES];
    uint8_t scratch[TEST_SETTINGS_BYTES];
    uint8_t record_work[TEST_PROGRAM_BYTES];
    char key_buffer[TEST_KEY_BYTES];
    fake_esy_hal_flash_state_t *flash;
    bool authorized;
} fixture_ctx_t;

static fixture_ctx_t s_ctx;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    fixture_ctx_t *ctx = context;
    (void)request;
    return ctx->authorized ? ESY_OK : ESY_ERR_FAIL;
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    fake_esy_hal_flash_reset();
    s_ctx.flash = fake_esy_hal_flash_get_state();
    s_ctx.flash->program_size = TEST_PROGRAM_BYTES;
    esy_dual_flash_store_static_resources_t resources = {
        .work_buffer = s_ctx.record_work,
        .work_buffer_size = sizeof(s_ctx.record_work),
    };
    const uint32_t region_size = s_ctx.flash->size_bytes / ESY_DUAL_FLASH_STORE_REGION_COUNT;
    esy_dual_flash_store_config_t record = {
        .regions = {{s_ctx.flash->storage_base_addr, region_size},
                    {s_ctx.flash->storage_base_addr + region_size, region_size}},
        .static_resources = &resources,
    };
    TEST_ASSERT_EQUAL(ESY_OK, esy_dual_flash_store_init(&s_ctx.dual_flash_store, &record));
    const esy_file_dual_flash_store_config_t file_config = {.store = &s_ctx.dual_flash_store};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_init(&s_ctx.file_adapter, &file_config));
    esy_file_ops_t file_ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_get_ops(&s_ctx.file_adapter, &file_ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_init(&s_ctx.file, &file_ops));
    esy_settings_static_resources_t settings_resources = {
        .working_buffer = s_ctx.work, .scratch_buffer = s_ctx.scratch,
        .buffer_size = sizeof(s_ctx.work),
    };
    esy_settings_config_t settings_config = {
        .file = &s_ctx.file, .max_key_length = TEST_KEY_BYTES - 1u,
        .static_resources = &settings_resources,
    };
    TEST_ASSERT_EQUAL(ESY_OK, esy_settings_init(&s_ctx.settings, &settings_config));
    s_ctx.adapter_resources = (esy_remote_manager_settings_static_resources_t){
        .key_buffer = s_ctx.key_buffer,
        .key_buffer_size = sizeof(s_ctx.key_buffer),
    };
    s_ctx.adapter_config = (esy_remote_manager_settings_config_t){
        .settings = &s_ctx.settings,
        .static_resources = &s_ctx.adapter_resources,
    };
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_settings_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_settings_deinit(&s_ctx.settings));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_dual_flash_store_deinit(&s_ctx.file_adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_dual_flash_store_deinit(&s_ctx.dual_flash_store));
}

static void initialize(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_settings_init(
        &s_ctx.adapter, &s_ctx.adapter_config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_settings_get_ops(
        &s_ctx.adapter, &s_ctx.ops));
}

void test_settings_adapter_validates_resources_before_borrowing_store(void) {
    esy_settings_t zero_store = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_settings_init(NULL, &s_ctx.adapter_config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_settings_init(&s_ctx.adapter, NULL));
    s_ctx.adapter_config.settings = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_settings_init(&s_ctx.adapter, &s_ctx.adapter_config));
    s_ctx.adapter_config.settings = &zero_store;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_settings_init(&s_ctx.adapter, &s_ctx.adapter_config));
    s_ctx.adapter_config.settings = &s_ctx.settings;
    s_ctx.adapter_config.static_resources = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_settings_init(&s_ctx.adapter, &s_ctx.adapter_config));
    s_ctx.adapter_config.static_resources = &s_ctx.adapter_resources;
    s_ctx.adapter_resources.key_buffer = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_settings_init(&s_ctx.adapter, &s_ctx.adapter_config));
    s_ctx.adapter_resources.key_buffer = s_ctx.key_buffer;
    s_ctx.adapter_resources.key_buffer_size = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_settings_init(&s_ctx.adapter, &s_ctx.adapter_config));
    s_ctx.adapter_resources.key_buffer_size = sizeof(s_ctx.key_buffer);
    initialize();
}

void test_settings_adapter_lifecycle_never_owns_flash_or_settings_lifecycle(void) {
    const uint32_t reads = s_ctx.flash->read_count;
    esy_settings_info_t info;
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_settings_init(&s_ctx.adapter, &s_ctx.adapter_config));
    TEST_ASSERT_NOT_NULL(s_ctx.ops.get);
    TEST_ASSERT_NOT_NULL(s_ctx.ops.set);
    TEST_ASSERT_NOT_NULL(s_ctx.ops.remove);
    TEST_ASSERT_NOT_NULL(s_ctx.ops.save);
    TEST_ASSERT_NULL(s_ctx.ops.apply);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_settings_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_settings_deinit(&s_ctx.adapter));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_settings_deinit(NULL));
    TEST_ASSERT_EQUAL(ESY_OK, esy_settings_get_info(&s_ctx.settings, &info));
    TEST_ASSERT_EQUAL_UINT32(reads, s_ctx.flash->read_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.flash->write_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.flash->erase_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.flash->init_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.flash->deinit_count);
    initialize();
}

void test_settings_adapter_get_ops_clears_failed_output(void) {
    esy_remote_manager_config_ops_t zero = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_settings_get_ops(NULL, &s_ctx.ops));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.ops, sizeof(zero));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_settings_get_ops(&s_ctx.adapter, &s_ctx.ops));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_settings_get_ops(&s_ctx.adapter, NULL));
}

void test_settings_adapter_converts_nonterminated_keys_and_stages_binary_values(void) {
    const char key[] = {'m', 'o', 'd', 'e'};
    const uint8_t value[] = {0, 0xff, 0x80};
    uint8_t output[sizeof(value)];
    size_t length = 0;
    bool found = false;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.set(s_ctx.ops.context,
        key, sizeof(key), value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("mode", s_ctx.key_buffer);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.get(s_ctx.ops.context,
        key, sizeof(key), output, sizeof(output), &length, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT(sizeof(value), length);
    TEST_ASSERT_EQUAL_MEMORY(value, output, sizeof(value));
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.flash->write_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.flash->erase_count);
}

void test_settings_adapter_preserves_missing_empty_and_capacity_query_semantics(void) {
    const uint8_t value[] = {1, 2};
    size_t length = SIZE_MAX;
    bool found = true;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.get(s_ctx.ops.context,
        "key", 3, NULL, 0, &length, &found));
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_UINT(0, length);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.set(s_ctx.ops.context, "key", 3, NULL, 0));
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.get(s_ctx.ops.context,
        "key", 3, NULL, 0, &length, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT(0, length);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.set(s_ctx.ops.context,
        "key", 3, value, sizeof(value)));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, s_ctx.ops.get(s_ctx.ops.context,
        "key", 3, NULL, 0, &length, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT(sizeof(value), length);
}

void test_settings_adapter_saves_explicitly_and_reads_persisted_values_after_reload(void) {
    const uint8_t value = 42;
    uint8_t output = 0;
    size_t length = 0;
    bool found = false;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.set(s_ctx.ops.context, "key", 3, &value, 1));
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.save(s_ctx.ops.context));
    TEST_ASSERT_GREATER_THAN_UINT32(0, s_ctx.flash->write_count);
    TEST_ASSERT_GREATER_THAN_UINT32(0, s_ctx.flash->erase_count);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.remove(s_ctx.ops.context, "key", 3));
    TEST_ASSERT_EQUAL(ESY_OK, esy_settings_reload(&s_ctx.settings));
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.get(s_ctx.ops.context,
        "key", 3, &output, 1, &length, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT8(value, output);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.remove(s_ctx.ops.context, "key", 3));
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.save(s_ctx.ops.context));
    TEST_ASSERT_EQUAL(ESY_OK, esy_settings_reload(&s_ctx.settings));
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.get(s_ctx.ops.context,
        "key", 3, NULL, 0, &length, &found));
    TEST_ASSERT_FALSE(found);
}

void test_settings_adapter_rejects_invalid_key_spans_without_store_changes(void) {
    const char embedded[] = {'a', '\0', 'b'};
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.set(s_ctx.ops.context, NULL, 1, NULL, 0));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.set(s_ctx.ops.context, "", 0, NULL, 0));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.set(s_ctx.ops.context, "12345678", 8, NULL, 0));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.remove(s_ctx.ops.context, embedded, sizeof(embedded)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.save(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.flash->write_count);
    esy_settings_info_t info;
    TEST_ASSERT_EQUAL(ESY_OK, esy_settings_get_info(&s_ctx.settings, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.entry_count);
}

void test_settings_adapter_propagates_uncertain_save_without_implicit_retry(void) {
    const uint8_t value = 3;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.set(s_ctx.ops.context, "key", 3, &value, 1));
    s_ctx.flash->write_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, s_ctx.ops.save(s_ctx.ops.context));
    const uint32_t writes = s_ctx.flash->write_count;
    esy_settings_info_t info;
    TEST_ASSERT_EQUAL(ESY_OK, esy_settings_get_info(&s_ctx.settings, &info));
    TEST_ASSERT_EQUAL(ESY_SETTINGS_UNCERTAIN, info.state);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.ops.save(s_ctx.ops.context));
    TEST_ASSERT_EQUAL_UINT32(writes, s_ctx.flash->write_count);
}

void test_settings_adapter_integrates_with_core_authorization_before_mutation(void) {
    const uint8_t value = 1;
    size_t length = 0;
    bool found = true;
    esy_remote_manager_response_t response;
    initialize();
    esy_remote_manager_config_t config = {
        .max_key_length = TEST_KEY_BYTES - 1u,
        .max_request_value_length = TEST_SETTINGS_BYTES, .max_response_value_length = TEST_SETTINGS_BYTES,
        .authorize = authorize, .authorize_context = &s_ctx,
        .config_ops = s_ctx.ops,
    };
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&s_ctx.manager, &config));
    esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_CONFIG,
        .command = ESY_REMOTE_MANAGER_CONFIG_SET,
        .key = "key", .key_length = 3, .value = &value, .value_length = 1,
    };
    TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL(ESY_OK, esy_settings_get(&s_ctx.settings,
        "key", NULL, 0, &length, &found));
    TEST_ASSERT_FALSE(found);
    s_ctx.authorized = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_EQUAL_UINT32(0, s_ctx.flash->write_count);
    request = (esy_remote_manager_request_t){
        .group = ESY_REMOTE_MANAGER_GROUP_CONFIG,
        .command = ESY_REMOTE_MANAGER_CONFIG_SAVE,
    };
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_dispatch(
        &s_ctx.manager, &request, NULL, 0, &response));
    TEST_ASSERT_GREATER_THAN_UINT32(0, s_ctx.flash->write_count);
}
