/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_fs_flash.h"
#include "esy_fs_littlefs.h"
#include "fake_esy_hal_flash.h"
#include <string.h>

#define TEST_FLASH_BASE 0x1000u
#define TEST_FLASH_BYTES 8192u
#define TEST_FLASH_BLOCK 256u
#define TEST_FLASH_CACHE 64u

typedef struct {
    esy_fs_flash_t flash;
    esy_fs_flash_config_t config;
    uint8_t image[TEST_FLASH_BYTES];
} fs_flash_test_ctx_t;
static fs_flash_test_ctx_t s_ctx;

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(s_ctx.image, 0xff, sizeof(s_ctx.image));
    fake_esy_hal_flash_reset();
    fake_esy_hal_flash_state_t *state = fake_esy_hal_flash_get_state();
    state->storage_base_addr = TEST_FLASH_BASE;
    state->size_bytes = sizeof(s_ctx.image);
    state->backing_storage = s_ctx.image;
    state->backing_storage_size = sizeof(s_ctx.image);
    state->sector_size = 128u;
    state->program_size = 16u;
    s_ctx.config = (esy_fs_flash_config_t){.address = TEST_FLASH_BASE,
        .size = TEST_FLASH_BYTES, .block_size = TEST_FLASH_BLOCK};
}
void tearDown(void) {}

void test_fs_flash_validates_all_logical_blocks_across_heterogeneous_physical_sectors(void) {
    fake_esy_hal_flash_state_t *state = fake_esy_hal_flash_get_state();
    state->sector_size_after_address = TEST_FLASH_BASE + 1024u;
    state->sector_size_after = 256u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    TEST_ASSERT_GREATER_THAN(TEST_FLASH_BYTES / TEST_FLASH_BLOCK, state->sector_count);
    TEST_ASSERT_EQUAL(TEST_FLASH_BASE + TEST_FLASH_BYTES - TEST_FLASH_BLOCK, state->last_sector_address);
    TEST_ASSERT_EQUAL(0u, state->init_count);
    TEST_ASSERT_EQUAL(0u, state->erase_count);
    TEST_ASSERT_EQUAL(0u, state->write_count);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    esy_fs_flash_t copy = s_ctx.flash;
    struct lfs_config config;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_flash_get_config(&copy, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_flash_deinit(&copy));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_deinit(&s_ctx.flash));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_deinit(&s_ctx.flash));
    TEST_ASSERT_EQUAL(0u, state->deinit_count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
}

void test_fs_flash_rejects_later_partial_sector_even_when_first_block_is_valid(void) {
    fake_esy_hal_flash_state_t *state = fake_esy_hal_flash_get_state();
    state->sector_size_after_address = TEST_FLASH_BASE + 1024u;
    state->sector_size_after = 512u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    TEST_ASSERT_NULL(s_ctx.flash.self);
    TEST_ASSERT_EQUAL(TEST_FLASH_BASE + 1024u, state->last_sector_address);
    TEST_ASSERT_EQUAL(0u, state->erase_count);
    s_ctx.config.block_size = 512u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
}

void test_fs_flash_rejects_out_of_bounds_overflow_alignment_and_geometry_failures(void) {
    fake_esy_hal_flash_state_t *state = fake_esy_hal_flash_get_state();
    s_ctx.config.address += 16u;
    s_ctx.config.size -= TEST_FLASH_BLOCK;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    s_ctx.config.address = UINT32_MAX - 127u;
    s_ctx.config.size = 512u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    s_ctx.config.address = TEST_FLASH_BASE;
    s_ctx.config.size = TEST_FLASH_BYTES;
    state->program_size = 24u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    state->program_size = 16u;
    state->geometry_result = ESY_ERR_NOT_SUPPORTED;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    state->geometry_result = ESY_OK;
    state->sector_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    state->sector_result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
}

void test_fs_flash_native_callbacks_enforce_bounds_alignment_and_map_io_errors(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    struct lfs_config device;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_get_config(&s_ctx.flash, &device));
    TEST_ASSERT_EQUAL(16u, device.prog_size);
    TEST_ASSERT_EQUAL(1u, device.read_size);
    TEST_ASSERT_EQUAL(TEST_FLASH_BLOCK, device.block_size);
    TEST_ASSERT_EQUAL(TEST_FLASH_BYTES / TEST_FLASH_BLOCK, device.block_count);
    uint8_t data[16];
    memset(data, 0x42, sizeof(data));
    TEST_ASSERT_EQUAL(LFS_ERR_INVAL, device.prog(&device, 0u, 1u, data, sizeof(data)));
    TEST_ASSERT_EQUAL(LFS_ERR_INVAL, device.prog(&device, 0u, 0u, data, 15u));
    TEST_ASSERT_EQUAL(LFS_ERR_INVAL, device.read(&device, device.block_count, 0u, data, sizeof(data)));
    TEST_ASSERT_EQUAL(LFS_ERR_INVAL, device.read(&device, 0u, UINT32_MAX, data, sizeof(data)));
    TEST_ASSERT_EQUAL(LFS_ERR_INVAL, device.read(&device, 0u, TEST_FLASH_BLOCK - 1u, data, sizeof(data)));
    TEST_ASSERT_EQUAL(0u, fake_esy_hal_flash_get_state()->read_count);
    TEST_ASSERT_EQUAL(0u, fake_esy_hal_flash_get_state()->write_count);
    TEST_ASSERT_EQUAL(0, device.prog(&device, 1u, 16u, data, sizeof(data)));
    TEST_ASSERT_EQUAL_MEMORY(data, s_ctx.image + TEST_FLASH_BLOCK + 16u, sizeof(data));
    memset(data, 0u, sizeof(data));
    TEST_ASSERT_EQUAL(0, device.read(&device, 1u, 16u, data, sizeof(data)));
    TEST_ASSERT_EACH_EQUAL_HEX8(0x42, data, sizeof(data));
    TEST_ASSERT_EQUAL(0, device.erase(&device, 1u));
    TEST_ASSERT_EACH_EQUAL_HEX8(0xff, s_ctx.image + TEST_FLASH_BLOCK, TEST_FLASH_BLOCK);
    fake_esy_hal_flash_state_t *state = fake_esy_hal_flash_get_state();
    state->read_result = ESY_ERR_IO;
    state->write_result = ESY_ERR_TIMEOUT;
    state->erase_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(LFS_ERR_IO, device.read(&device, 0u, 0u, data, sizeof(data)));
    TEST_ASSERT_EQUAL(LFS_ERR_IO, device.prog(&device, 0u, 0u, data, sizeof(data)));
    TEST_ASSERT_EQUAL(LFS_ERR_IO, device.erase(&device, 0u));
    TEST_ASSERT_EQUAL(0, device.sync(&device));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_deinit(&s_ctx.flash));
    TEST_ASSERT_EQUAL(LFS_ERR_BADF, device.sync(&device));
}

void test_fs_flash_real_littlefs_round_trip_preserves_neighboring_flash_regions(void) {
    s_ctx.config.address += TEST_FLASH_BLOCK;
    s_ctx.config.size -= 2u * TEST_FLASH_BLOCK;
    memset(s_ctx.image, 0x81, TEST_FLASH_BLOCK);
    memset(s_ctx.image + sizeof(s_ctx.image) - TEST_FLASH_BLOCK, 0x81, TEST_FLASH_BLOCK);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_init(&s_ctx.flash, &s_ctx.config));
    struct lfs_config device;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_get_config(&s_ctx.flash, &device));
    device.cache_size = TEST_FLASH_CACHE;
    device.lookahead_size = 8u;
    device.block_cycles = 100;
    esy_fs_littlefs_t backend = {0};
    esy_fs_littlefs_file_t files[1];
    uint8_t read_buffer[TEST_FLASH_CACHE], program_buffer[TEST_FLASH_CACHE];
    uint8_t file_buffers[TEST_FLASH_CACHE], lookahead[8];
    const esy_fs_littlefs_static_resources_t resources = {.files = files, .file_count = 1u,
        .read_buffer = read_buffer, .read_capacity = sizeof(read_buffer),
        .program_buffer = program_buffer, .program_capacity = sizeof(program_buffer),
        .file_buffers = file_buffers, .file_buffers_capacity = sizeof(file_buffers),
        .lookahead_buffer = lookahead, .lookahead_capacity = sizeof(lookahead)};
    const esy_fs_littlefs_config_t config = {.device = &device, .static_resources = &resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_init(&backend, &config));
    esy_fs_ops_t ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_get_ops(&backend, &ops));
    esy_fs_t fs = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_init(&fs, &ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_format(&fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_replace(&fs, "/settings", "/stage", "config", 6u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&fs));
    esy_fs_file_t file = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&fs, &file, "/settings", ESY_FS_OPEN_READ));
    char actual[6];
    size_t count;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&file, actual, sizeof(actual), &count));
    TEST_ASSERT_EQUAL(sizeof(actual), count);
    TEST_ASSERT_EQUAL_MEMORY("config", actual, sizeof(actual));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_deinit(&fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_deinit(&backend));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_flash_deinit(&s_ctx.flash));
    TEST_ASSERT_EACH_EQUAL_HEX8(0x81, s_ctx.image, TEST_FLASH_BLOCK);
    TEST_ASSERT_EACH_EQUAL_HEX8(0x81, s_ctx.image + sizeof(s_ctx.image) - TEST_FLASH_BLOCK, TEST_FLASH_BLOCK);
}
