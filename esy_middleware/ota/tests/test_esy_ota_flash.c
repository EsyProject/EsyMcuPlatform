/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <string.h>
#include "esy_middleware/ota/esy_ota_flash.h"
#include "esy_common/utils/integrity/esy_sha256.h"
#include "fake_esy_bootloader.h"
#include "fake_esy_hal_flash.h"

#define TEST_CACHE_BYTES 32u
#define TEST_IMAGE_BYTES 80u

typedef struct {
    esy_ota_flash_t flash;
    esy_ota_provider_t provider;
    esy_ota_flash_static_resources_t resources;
    esy_ota_flash_config_t config;
    esy_image_manifest_t manifest;
    uint8_t cache[TEST_CACHE_BYTES];
    uint8_t image[TEST_IMAGE_BYTES];
} flash_test_ctx_t;

static flash_test_ctx_t s_ctx;

static void make_manifest(size_t size) {
    s_ctx.manifest = (esy_image_manifest_t){.version = 7u, .image_size = (uint32_t)size};
    esy_sha256(s_ctx.image, size, s_ctx.manifest.image_sha256);
    memset(s_ctx.manifest.signature, 0x5a, sizeof(s_ctx.manifest.signature));
}
static void begin_image(size_t size) {
    make_manifest(size);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
}
static void resize_cache(size_t bytes) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.abort(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_deinit(&s_ctx.flash));
    s_ctx.resources.cache_size = bytes;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_init(&s_ctx.flash, &s_ctx.config));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_get_provider(&s_ctx.flash, &s_ctx.provider));
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    fake_esy_bootloader_reset();
    fake_esy_hal_flash_reset();
    for (size_t i = 0u; i < sizeof(s_ctx.image); ++i) s_ctx.image[i] = (uint8_t)i;
    s_ctx.resources = (esy_ota_flash_static_resources_t){s_ctx.cache, 16u};
    s_ctx.config.static_resources = &s_ctx.resources;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_init(&s_ctx.flash, &s_ctx.config));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_get_provider(&s_ctx.flash, &s_ctx.provider));
}
void tearDown(void) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_deinit(&s_ctx.flash));
}

void test_finish_verifies_only_and_activation_publishes_signed_metadata_once(void) {
    fake_esy_bootloader_state_t *boot = fake_esy_bootloader_get_state();
    begin_image(5u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 5u));
    TEST_ASSERT_EQUAL_UINT(0u, fake_esy_hal_flash_get_state()->write_count);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.activate(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(0u, boot->write_meta_count);
    TEST_ASSERT_EQUAL_UINT(0u, boot->activate_count);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.write(s_ctx.provider.context, 5u, s_ctx.image, 1u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.activate(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(1u, boot->write_meta_count);
    TEST_ASSERT_EQUAL_UINT(1u, boot->activate_count);
    TEST_ASSERT_EQUAL_UINT(1u, boot->meta_writes_at_activation);
    TEST_ASSERT_EQUAL_UINT(ESY_FIRMWARE_STATE_TRY_BOOT, boot->last_write_meta_header.state);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.manifest.image_sha256, boot->last_write_meta_header.image_sha256, ESY_IMAGE_SHA256_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.manifest.signature, boot->last_write_meta_header.signature, ESY_IMAGE_SIGNATURE_BYTES);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.activate(s_ctx.provider.context));
}

void test_padding_is_not_hashed_and_cache_flushes_complete_program_units(void) {
    fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
    flash->program_size = 8u;
    begin_image(18u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 3u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 3u, s_ctx.image + 3u, 15u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(8u, flash->last_write_len);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.image, flash->storage, 18u);
    for (unsigned i = 18u; i < 24u; ++i) TEST_ASSERT_EQUAL_HEX8(0xffu, flash->storage[i]);
    TEST_ASSERT_EQUAL_UINT(0u, fake_esy_bootloader_get_state()->write_meta_count);
}

void test_nonuniform_sectors_with_small_cache_are_not_reerased(void) {
    resize_cache(8u);
    fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
    flash->program_size = 4u;
    flash->sector_size_after_address = 0x1020u;
    flash->sector_size_after = 32u;
    begin_image(70u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 5u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 5u, s_ctx.image + 5u, 65u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(4u, flash->erase_count);
    TEST_ASSERT_EQUAL_HEX32(0x1040u, flash->last_erase_addr);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.image, flash->storage, 70u);
    TEST_ASSERT_EQUAL_UINT(0u, fake_esy_bootloader_get_state()->write_meta_count);
}

void test_hash_mismatch_and_read_error_cannot_publish_metadata(void) {
    begin_image(16u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 16u));
    fake_esy_hal_flash_get_state()->storage[0] ^= 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_HASH_FAIL, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.activate(s_ctx.provider.context));
    begin_image(16u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 16u));
    fake_esy_hal_flash_get_state()->read_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(0u, fake_esy_bootloader_get_state()->write_meta_count);
}

void test_partial_program_and_erase_errors_end_the_provider_session(void) {
    begin_image(16u);
    fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
    flash->erase_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 16u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.finish(s_ctx.provider.context));
    flash->erase_result = ESY_OK;
    flash->write_result = ESY_ERR_IO;
    begin_image(16u);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 16u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.activate(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(0u, fake_esy_bootloader_get_state()->write_meta_count);
}

void test_tail_flush_errors_are_not_reported_as_verified(void) {
    begin_image(5u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 5u));
    fake_esy_hal_flash_get_state()->erase_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.finish(s_ctx.provider.context));
    fake_esy_hal_flash_get_state()->erase_result = ESY_OK;
    begin_image(5u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 5u));
    fake_esy_hal_flash_get_state()->write_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.activate(s_ctx.provider.context));
}

void test_activation_failure_reports_uncertainty_without_repeating_publication(void) {
    begin_image(5u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 5u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.finish(s_ctx.provider.context));
    fake_esy_bootloader_get_state()->activate_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.activate(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(1u, fake_esy_bootloader_get_state()->write_meta_count);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.activate(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(1u, fake_esy_bootloader_get_state()->activate_count);
}

void test_metadata_write_error_prevents_native_activation(void) {
    begin_image(5u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 5u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.finish(s_ctx.provider.context));
    fake_esy_bootloader_get_state()->write_meta_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.activate(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(0u, fake_esy_bootloader_get_state()->activate_count);
}

void test_slot_snapshot_is_retained_until_explicit_activation(void) {
    begin_image(5u);
    fake_esy_bootloader_get_state()->flash_slot = -1;
    fake_esy_bootloader_get_state()->flash_slot_addr = 0x1040u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 5u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_HEX32(0x1000u, fake_esy_hal_flash_get_state()->last_write_addr);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.activate(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(1, fake_esy_bootloader_get_state()->last_write_meta_slot);
}

void test_invalid_slot_geometry_and_cache_are_rejected_before_destructive_io(void) {
    make_manifest(17u);
    fake_esy_bootloader_state_t *boot = fake_esy_bootloader_get_state();
    fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
    boot->flash_slot_size = 16u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    boot->flash_slot_size = 17u;
    flash->program_size = 8u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    flash->program_size = 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    boot->flash_slot_size = 32u;
    ++boot->flash_slot_addr;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    boot->flash_slot_addr = 0x1000u;
    flash->sector_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    TEST_ASSERT_EQUAL_UINT(0u, flash->erase_count);
    TEST_ASSERT_EQUAL_UINT(0u, flash->write_count);
}

void test_unavailable_slot_and_geometry_errors_propagate_before_writes(void) {
    make_manifest(1u);
    fake_esy_bootloader_state_t *boot = fake_esy_bootloader_get_state();
    fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
    boot->flash_slot = -1;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    boot->flash_slot = 1; boot->flash_slot_size = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    boot->flash_slot_size = 256u;
    flash->geometry_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    flash->geometry_result = ESY_OK; flash->program_size = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    flash->program_size = 4u;
    resize_cache(3u);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    TEST_ASSERT_EQUAL_UINT(0u, flash->erase_count);
}

void test_geometry_change_cannot_erase_outside_the_claimed_slot(void) {
    begin_image(16u);
    fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
    flash->sector_size = 512u; flash->size_bytes = 512u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 16u));
    TEST_ASSERT_EQUAL_UINT(0u, flash->erase_count);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.finish(s_ctx.provider.context));
}

void test_incomplete_or_oversized_input_ends_only_the_current_upload(void) {
    begin_image(5u);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.write(s_ctx.provider.context, 1u, s_ctx.image, 1u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.finish(s_ctx.provider.context));
    begin_image(5u);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 6u));
    begin_image(5u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.abort(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.abort(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_UINT(0u, fake_esy_bootloader_get_state()->write_meta_count);
}

void test_provider_and_handle_invalid_arguments_and_lifecycle_are_checked(void) {
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_flash_init(&s_ctx.flash, &s_ctx.config));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.begin(NULL, &s_ctx.manifest));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.begin(s_ctx.provider.context, NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.write(NULL, 0u, s_ctx.image, 1u));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.finish(NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.activate(NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.abort(NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_flash_get_provider(NULL, &s_ctx.provider));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_flash_get_provider(&s_ctx.flash, NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_flash_deinit(NULL));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_deinit(&s_ctx.flash));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_deinit(&s_ctx.flash));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_flash_get_provider(&s_ctx.flash, &s_ctx.provider));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_flash_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_flash_init(&s_ctx.flash, NULL));
    s_ctx.resources.cache_buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_flash_init(&s_ctx.flash, &s_ctx.config));
    s_ctx.resources.cache_buffer = s_ctx.cache; s_ctx.resources.cache_size = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_flash_init(&s_ctx.flash, &s_ctx.config));
    s_ctx.resources.cache_size = 16u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_init(&s_ctx.flash, &s_ctx.config));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_flash_get_provider(&s_ctx.flash, &s_ctx.provider));
    s_ctx.manifest.version = 0u; s_ctx.manifest.image_size = 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
}

void test_activation_failure_prevents_new_erase_even_after_abort(void) {
    begin_image(16u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 16u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.finish(s_ctx.provider.context));
    fake_esy_bootloader_get_state()->activate_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_ctx.provider.activate(s_ctx.provider.context));
    const uint32_t erases = fake_esy_hal_flash_get_state()->erase_count;
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.abort(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 16u));
    TEST_ASSERT_EQUAL_UINT(erases, fake_esy_hal_flash_get_state()->erase_count);
    /* Simulate owner reconciliation before explicitly replacing the binding. */
    fake_esy_bootloader_get_state()->activate_result = ESY_OK;
    resize_cache(16u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
}

void test_successful_activation_keeps_published_image_reserved_until_reconciliation(void) {
    begin_image(16u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.write(s_ctx.provider.context, 0u, s_ctx.image, 16u));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.finish(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.activate(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_OK, s_ctx.provider.abort(s_ctx.provider.context));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, s_ctx.provider.begin(s_ctx.provider.context, &s_ctx.manifest));
    TEST_ASSERT_EQUAL_UINT(1u, fake_esy_hal_flash_get_state()->erase_count);
}
