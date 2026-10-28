/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_fs_littlefs.h"
#include <string.h>

#define TEST_BLOCK_SIZE 256u
#define TEST_BLOCK_COUNT 48u
#define TEST_CACHE_SIZE 64u
#define TEST_FILE_COUNT 2u
#define TEST_PAYLOAD_SIZE 1500u

typedef struct {
    esy_fs_littlefs_t backend;
    esy_fs_t volume;
    esy_fs_littlefs_file_t files[TEST_FILE_COUNT];
    esy_fs_littlefs_dir_t directories[1];
    uint8_t read_buffer[TEST_CACHE_SIZE];
    uint8_t program_buffer[TEST_CACHE_SIZE];
    uint8_t lookahead_buffer[8];
    uint8_t file_buffers[TEST_FILE_COUNT * TEST_CACHE_SIZE];
    uint8_t image[TEST_BLOCK_COUNT * TEST_BLOCK_SIZE];
    struct lfs_config device;
    esy_fs_littlefs_static_resources_t resources;
    size_t steps;
    size_t cut_step;
    size_t cut_prefix;
    bool powered;
} littlefs_test_ctx_t;
static littlefs_test_ctx_t s_ctx;

static bool valid_span(lfs_block_t block, lfs_off_t offset, lfs_size_t size) {
    return block < TEST_BLOCK_COUNT && offset <= TEST_BLOCK_SIZE && size <= TEST_BLOCK_SIZE - offset;
}

static int ram_read(const struct lfs_config *config, lfs_block_t block, lfs_off_t offset,
                    void *buffer, lfs_size_t size) {
    littlefs_test_ctx_t *ctx = config->context;
    if (!ctx->powered) return LFS_ERR_IO;
    if (!valid_span(block, offset, size)) return LFS_ERR_INVAL;
    memcpy(buffer, ctx->image + block * TEST_BLOCK_SIZE + offset, size);
    return 0;
}

static bool cut_now(littlefs_test_ctx_t *ctx) {
    ++ctx->steps;
    if (ctx->cut_step != 0u && ctx->steps == ctx->cut_step) {
        ctx->powered = false;
        return true;
    }
    return false;
}

static int ram_program(const struct lfs_config *config, lfs_block_t block, lfs_off_t offset,
                       const void *buffer, lfs_size_t size) {
    littlefs_test_ctx_t *ctx = config->context;
    if (!ctx->powered) return LFS_ERR_IO;
    if (!valid_span(block, offset, size)) return LFS_ERR_INVAL;
    const bool cut = cut_now(ctx);
    const size_t amount = cut && ctx->cut_prefix < size ? ctx->cut_prefix : size;
    uint8_t *destination = ctx->image + block * TEST_BLOCK_SIZE + offset;
    const uint8_t *source = buffer;
    for (size_t i = 0u; i < amount; ++i) destination[i] &= source[i];
    return cut ? LFS_ERR_IO : 0;
}

static int ram_erase(const struct lfs_config *config, lfs_block_t block) {
    littlefs_test_ctx_t *ctx = config->context;
    if (!ctx->powered) return LFS_ERR_IO;
    if (block >= TEST_BLOCK_COUNT) return LFS_ERR_INVAL;
    const bool cut = cut_now(ctx);
    const size_t amount = cut && ctx->cut_prefix < TEST_BLOCK_SIZE ? ctx->cut_prefix : TEST_BLOCK_SIZE;
    memset(ctx->image + block * TEST_BLOCK_SIZE, 0xff, amount);
    return cut ? LFS_ERR_IO : 0;
}

static int ram_sync(const struct lfs_config *config) {
    littlefs_test_ctx_t *ctx = config->context;
    if (!ctx->powered) return LFS_ERR_IO;
    return cut_now(ctx) ? LFS_ERR_IO : 0;
}

static void initialize_backend(void) {
    const esy_fs_littlefs_config_t config = {.device = &s_ctx.device,
                                            .static_resources = &s_ctx.resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_init(&s_ctx.backend, &config));
    esy_fs_ops_t ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_get_ops(&s_ctx.backend, &ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_init(&s_ctx.volume, &ops));
}

static void format_mount(void) {
    initialize_backend();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_format(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volume));
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(s_ctx.image, 0xff, sizeof(s_ctx.image));
    s_ctx.powered = true;
    s_ctx.device = (struct lfs_config){.context = &s_ctx, .read = ram_read, .prog = ram_program,
        .erase = ram_erase, .sync = ram_sync, .read_size = 16u, .prog_size = 16u,
        .block_size = TEST_BLOCK_SIZE, .block_count = TEST_BLOCK_COUNT, .block_cycles = 100,
        .cache_size = TEST_CACHE_SIZE, .lookahead_size = sizeof(s_ctx.lookahead_buffer)};
    s_ctx.resources = (esy_fs_littlefs_static_resources_t){.files = s_ctx.files,
        .file_count = TEST_FILE_COUNT, .directories = s_ctx.directories, .directory_count = 1u,
        .read_buffer = s_ctx.read_buffer, .read_capacity = sizeof(s_ctx.read_buffer),
        .program_buffer = s_ctx.program_buffer, .program_capacity = sizeof(s_ctx.program_buffer),
        .lookahead_buffer = s_ctx.lookahead_buffer, .lookahead_capacity = sizeof(s_ctx.lookahead_buffer),
        .file_buffers = s_ctx.file_buffers, .file_buffers_capacity = sizeof(s_ctx.file_buffers)};
}
void tearDown(void) {}

void test_littlefs_requires_static_resources_and_rejects_overlapping_or_invalid_geometry(void) {
    const esy_fs_littlefs_config_t config = {.device = &s_ctx.device, .static_resources = &s_ctx.resources};
    s_ctx.resources.file_buffers_capacity = TEST_CACHE_SIZE;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_fs_littlefs_init(&s_ctx.backend, &config));
    s_ctx.resources.file_buffers_capacity = sizeof(s_ctx.file_buffers);
    s_ctx.resources.program_buffer = s_ctx.read_buffer;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_littlefs_init(&s_ctx.backend, &config));
    s_ctx.resources.program_buffer = s_ctx.program_buffer;
    s_ctx.device.cache_size = 17u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_littlefs_init(&s_ctx.backend, &config));
    s_ctx.device.cache_size = TEST_CACHE_SIZE;
    s_ctx.device.block_cycles = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_littlefs_init(&s_ctx.backend, &config));
    s_ctx.device.block_cycles = INT32_MAX;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_littlefs_init(&s_ctx.backend, &config));
    s_ctx.device.block_cycles = 100;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_init(&s_ctx.backend, &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_littlefs_init(&s_ctx.backend, &config));
    esy_fs_littlefs_t copy = s_ctx.backend;
    esy_fs_ops_t ops;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_littlefs_get_ops(&copy, &ops));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_littlefs_deinit(&copy));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_fs_littlefs_get_ops(&s_ctx.backend, (esy_fs_ops_t *)s_ctx.file_buffers));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_deinit(&s_ctx.backend));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_deinit(&s_ctx.backend));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_init(&s_ctx.backend, &config));
    TEST_ASSERT_EQUAL(0u, s_ctx.steps);
}

void test_littlefs_mount_failure_never_formats_and_can_be_retried(void) {
    initialize_backend();
    TEST_ASSERT_EQUAL(ESY_FS_ERR_CORRUPT, esy_fs_mount(&s_ctx.volume));
    TEST_ASSERT_EQUAL(0u, s_ctx.steps);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_format(&s_ctx.volume));
    s_ctx.powered = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_mount(&s_ctx.volume));
    s_ctx.powered = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_littlefs_deinit(&s_ctx.backend));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_deinit(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_deinit(&s_ctx.backend));
}

void test_littlefs_round_trip_append_seek_directory_and_remount(void) {
    format_mount();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mkdir(&s_ctx.volume, "/logs"));
    esy_fs_file_t file = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &file, "/logs/run", ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    size_t count = 99u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&file, "abc", 3u, &count));
    TEST_ASSERT_EQUAL(3u, count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_sync(&file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &file, "/logs/run", ESY_FS_OPEN_WRITE | ESY_FS_OPEN_APPEND));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&file, "de", 2u, &count));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_rename(&s_ctx.volume, "/logs/run", "/logs/done"));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &file, "/logs/done", ESY_FS_OPEN_READ));
    uint8_t readback[8] = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&file, readback, sizeof(readback), &count));
    TEST_ASSERT_EQUAL(5u, count);
    TEST_ASSERT_EQUAL_MEMORY("abcde", readback, count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&file, readback, sizeof(readback), &count));
    TEST_ASSERT_EQUAL(0u, count);
    uint64_t position = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_seek(&file, -2, ESY_FS_SEEK_END, &position));
    TEST_ASSERT_EQUAL(3u, position);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&file));
    esy_fs_dir_t dir = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.volume, &dir, "/logs"));
    esy_fs_entry_t entry;
    bool end = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_read(&dir, &entry, &end));
    TEST_ASSERT_FALSE(end);
    TEST_ASSERT_EQUAL_STRING("done", entry.name);
    TEST_ASSERT_EQUAL(5u, entry.stat.size);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_read(&dir, &entry, &end));
    TEST_ASSERT_TRUE(end);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_close(&dir));
    esy_fs_info_t info;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_get_info(&s_ctx.volume, &info));
    TEST_ASSERT_EQUAL(sizeof(s_ctx.image), info.capacity_bytes);
    TEST_ASSERT_TRUE(info.free_bytes < info.capacity_bytes);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_remove(&s_ctx.volume, "/logs/done"));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_remove(&s_ctx.volume, "/logs"));
}

void test_littlefs_pool_exhaustion_open_failure_and_close_error_release_slots(void) {
    format_mount();
    esy_fs_file_t first = {0}, second = {0}, third = {0};
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NOT_FOUND, esy_fs_open(&s_ctx.volume, &first, "/absent", ESY_FS_OPEN_READ));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &first, "/a", ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &second, "/b", ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_fs_open(&s_ctx.volume, &third, "/c", ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.volume));
    size_t count;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&first, "pending", 7u, &count));
    s_ctx.powered = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_close(&first));
    TEST_ASSERT_NULL(first.volume);
    s_ctx.powered = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&second));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &third, "/c", ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&third));
    esy_fs_dir_t dir = {0}, another = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.volume, &dir, "/"));
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_fs_dir_open(&s_ctx.volume, &another, "/"));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_close(&dir));
}

void test_littlefs_atomic_replace_overwrites_reserved_stale_staging_on_retry(void) {
    format_mount();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_replace(&s_ctx.volume, "/target", "/stage", "old", 3u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_replace(&s_ctx.volume, "/reserved", "/stage", "keep", 4u));
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_fs_replace(&s_ctx.volume, "/target", "/reserved", "new", 3u));
    esy_fs_file_t file = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &file, "/target", ESY_FS_OPEN_READ));
    char data[4] = {0};
    size_t count;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&file, data, 3u, &count));
    TEST_ASSERT_EQUAL_MEMORY("new", data, 3u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&file));
}

void test_littlefs_full_volume_reports_no_space_and_retains_old_target(void) {
    format_mount();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_replace(&s_ctx.volume, "/target", "/stage", "saved", 5u));
    esy_fs_file_t filler = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &filler, "/filler",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    uint8_t data[TEST_BLOCK_SIZE];
    memset(data, 0xa5, sizeof(data));
    esy_err_t result = ESY_OK;
    for (size_t i = 0u; i < TEST_BLOCK_COUNT * 2u && result == ESY_OK; ++i) {
        size_t count = 123u;
        result = esy_fs_write(&filler, data, sizeof(data), &count);
        if (result != ESY_OK) TEST_ASSERT_EQUAL(123u, count);
    }
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NO_SPACE, result);
    (void)esy_fs_close(&filler);
    esy_fs_file_t file = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &file, "/target", ESY_FS_OPEN_READ));
    size_t count;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&file, data, sizeof(data), &count));
    TEST_ASSERT_EQUAL(5u, count);
    TEST_ASSERT_EQUAL_MEMORY("saved", data, count);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&file));
}

static void reboot_preserving_image(void) {
    /* A simulated power reset discards all RAM ownership, preserving only media. */
    memset(&s_ctx.backend, 0, sizeof(s_ctx.backend));
    memset(&s_ctx.volume, 0, sizeof(s_ctx.volume));
    s_ctx.powered = true;
    s_ctx.cut_step = 0u;
    s_ctx.steps = 0u;
    initialize_backend();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volume));
}

void test_littlefs_atomic_replace_survives_each_program_erase_and_sync_power_cut(void) {
    uint8_t old_data[TEST_PAYLOAD_SIZE], new_data[TEST_PAYLOAD_SIZE];
    uint8_t baseline[sizeof(s_ctx.image)], readback[TEST_PAYLOAD_SIZE];
    memset(old_data, 0x35, sizeof(old_data));
    memset(new_data, 0xa7, sizeof(new_data));
    format_mount();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_replace(&s_ctx.volume, "/target", "/stage", old_data, sizeof(old_data)));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volume));
    memcpy(baseline, s_ctx.image, sizeof(baseline));
    reboot_preserving_image();
    s_ctx.steps = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_replace(&s_ctx.volume, "/target", "/stage", new_data, sizeof(new_data)));
    const size_t total_steps = s_ctx.steps;
    TEST_ASSERT_GREATER_THAN(0u, total_steps);
    const size_t prefixes[] = {0u, 7u, SIZE_MAX};
    for (size_t prefix = 0u; prefix < sizeof(prefixes) / sizeof(prefixes[0]); ++prefix) {
        for (size_t step = 1u; step <= total_steps; ++step) {
            memcpy(s_ctx.image, baseline, sizeof(baseline));
            reboot_preserving_image();
            s_ctx.steps = 0u;
            s_ctx.cut_step = step;
            s_ctx.cut_prefix = prefixes[prefix];
            TEST_ASSERT_NOT_EQUAL(ESY_OK,
                esy_fs_replace(&s_ctx.volume, "/target", "/stage", new_data, sizeof(new_data)));
            TEST_ASSERT_FALSE(s_ctx.powered);
            reboot_preserving_image();
            esy_fs_file_t file = {0};
            TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volume, &file, "/target", ESY_FS_OPEN_READ));
            size_t count;
            TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&file, readback, sizeof(readback), &count));
            TEST_ASSERT_EQUAL(sizeof(readback), count);
            TEST_ASSERT_TRUE(memcmp(old_data, readback, count) == 0 || memcmp(new_data, readback, count) == 0);
            TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&file));
        }
    }
}
