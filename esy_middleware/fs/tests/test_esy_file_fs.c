/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_fs_littlefs.h"
#include "esy_file_fs.h"
#include "esy_file.h"
#include <string.h>

#define TEST_BLOCK_SIZE 256u
#define TEST_BLOCK_COUNT 32u
#define TEST_CACHE_SIZE 64u

typedef struct {
    uint8_t media[TEST_BLOCK_SIZE * TEST_BLOCK_COUNT];
    uint8_t read_buffer[TEST_CACHE_SIZE];
    uint8_t program_buffer[TEST_CACHE_SIZE];
    uint8_t lookahead[8];
    uint8_t file_buffers[TEST_CACHE_SIZE * 2u];
    esy_fs_littlefs_file_t files[2];
    esy_fs_littlefs_dir_t directories[1];
    esy_fs_littlefs_t backend;
    esy_fs_t volume;
    esy_file_fs_t binding;
    esy_file_t file;
    esy_fs_ops_t backend_ops;
    esy_err_t (*real_close)(void *, void *, bool *);
    bool fail_close;
    size_t mutations;
} file_fs_test_ctx_t;

static file_fs_test_ctx_t s_ctx;

static int media_read(const struct lfs_config *config, lfs_block_t block,
                       lfs_off_t offset, void *data, lfs_size_t size)
{
    (void)config;
    if (block >= TEST_BLOCK_COUNT || offset > TEST_BLOCK_SIZE || size > TEST_BLOCK_SIZE - offset)
        return LFS_ERR_IO;
    memcpy(data, s_ctx.media + block * TEST_BLOCK_SIZE + offset, size);
    return 0;
}

static int media_program(const struct lfs_config *config, lfs_block_t block,
                          lfs_off_t offset, const void *data, lfs_size_t size)
{
    (void)config;
    if (block >= TEST_BLOCK_COUNT || offset > TEST_BLOCK_SIZE || size > TEST_BLOCK_SIZE - offset)
        return LFS_ERR_IO;
    uint8_t *destination = s_ctx.media + block * TEST_BLOCK_SIZE + offset;
    const uint8_t *source = data;
    for (lfs_size_t i = 0; i < size; ++i) destination[i] &= source[i];
    ++s_ctx.mutations;
    return 0;
}

static int media_erase(const struct lfs_config *config, lfs_block_t block)
{
    (void)config;
    if (block >= TEST_BLOCK_COUNT) return LFS_ERR_IO;
    memset(s_ctx.media + block * TEST_BLOCK_SIZE, 0xff, TEST_BLOCK_SIZE);
    ++s_ctx.mutations;
    return 0;
}

static int media_sync(const struct lfs_config *config)
{
    (void)config;
    return 0;
}

static esy_err_t intercepted_close(void *context, void *object, bool *released)
{
    if (s_ctx.fail_close) { *released = false; return ESY_ERR_IO; }
    return s_ctx.real_close(context, object, released);
}

static void bind_file(const char *temporary_path)
{
    esy_file_fs_config_t config = {0};
    config.volume = &s_ctx.volume;
    config.path = "/settings.bin";
    config.temporary_path = temporary_path;
    config.capacity = 256u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_fs_init(&s_ctx.binding, &config));
    esy_file_ops_t ops = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_fs_get_ops(&s_ctx.binding, &ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_init(&s_ctx.file, &ops));
}

void setUp(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(s_ctx.media, 0xff, sizeof(s_ctx.media));
    struct lfs_config device = {0};
    device.read = media_read;
    device.prog = media_program;
    device.erase = media_erase;
    device.sync = media_sync;
    device.read_size = 4u;
    device.prog_size = 16u;
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.cache_size = TEST_CACHE_SIZE;
    device.lookahead_size = sizeof(s_ctx.lookahead);
    device.block_cycles = 100;
    esy_fs_littlefs_static_resources_t resources = {0};
    resources.files = s_ctx.files;
    resources.file_count = 2u;
    resources.directories = s_ctx.directories;
    resources.directory_count = 1u;
    resources.read_buffer = s_ctx.read_buffer;
    resources.read_capacity = sizeof(s_ctx.read_buffer);
    resources.program_buffer = s_ctx.program_buffer;
    resources.program_capacity = sizeof(s_ctx.program_buffer);
    resources.lookahead_buffer = s_ctx.lookahead;
    resources.lookahead_capacity = sizeof(s_ctx.lookahead);
    resources.file_buffers = s_ctx.file_buffers;
    resources.file_buffers_capacity = sizeof(s_ctx.file_buffers);
    esy_fs_littlefs_config_t config = {0};
    config.device = &device;
    config.static_resources = &resources;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_init(&s_ctx.backend, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_get_ops(&s_ctx.backend, &s_ctx.backend_ops));
    s_ctx.real_close = s_ctx.backend_ops.close;
    s_ctx.backend_ops.close = intercepted_close;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_init(&s_ctx.volume, &s_ctx.backend_ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_format(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volume));
}

void tearDown(void)
{
    s_ctx.fail_close = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_fs_deinit(&s_ctx.binding));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_deinit(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_littlefs_deinit(&s_ctx.backend));
}

void test_absence_empty_creation_exact_read_and_atomic_replacement(void)
{
    bind_file("/settings.tmp");
    esy_file_info_t info = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_info(&s_ctx.file, &info));
    TEST_ASSERT_FALSE(info.exists);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_read(&s_ctx.file, 0u, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_create(&s_ctx.file, NULL, 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_info(&s_ctx.file, &info));
    TEST_ASSERT_TRUE(info.exists);
    TEST_ASSERT_EQUAL_UINT32(0u, info.length);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_file_create(&s_ctx.file, "x", 1u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, "config", 6u));
    char output[4] = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_read(&s_ctx.file, 1u, output, 4u));
    TEST_ASSERT_EQUAL_MEMORY("onfi", output, 4u);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_read(&s_ctx.file, 5u, output, 2u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_remove(&s_ctx.file));
}

void test_capability_query_and_capacity_rejection_do_not_mutate_media(void)
{
    bind_file("/settings.tmp");
    size_t before = s_ctx.mutations;
    uint32_t capabilities = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_capabilities(&s_ctx.file, &capabilities));
    TEST_ASSERT_BITS_HIGH(ESY_FILE_CAP_ATOMIC_REPLACE | ESY_FILE_CAP_CREATE, capabilities);
    char large[257] = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_file_replace(&s_ctx.file, large, sizeof(large)));
    TEST_ASSERT_EQUAL(before, s_ctx.mutations);
}

void test_binding_lease_prevents_unmount_until_detached(void)
{
    bind_file("/settings.tmp");
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_fs_init(&s_ctx.binding, &s_ctx.binding.config));
    esy_file_fs_t copied = s_ctx.binding;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_file_fs_deinit(&copied));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_fs_deinit(&s_ctx.binding));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.volume.leases);
}

void test_missing_staging_path_provides_read_remove_without_atomic_writes(void)
{
    bind_file(NULL);
    uint32_t capabilities = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_capabilities(&s_ctx.file, &capabilities));
    TEST_ASSERT_EQUAL_UINT32(ESY_FILE_CAP_READ | ESY_FILE_CAP_REMOVE, capabilities);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_replace(&s_ctx.file, "x", 1u));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_file_create(&s_ctx.file, "x", 1u));
}

void test_backend_without_atomic_capability_does_not_advertise_it(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volume));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_deinit(&s_ctx.volume));
    s_ctx.backend_ops.replace = NULL;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_init(&s_ctx.volume, &s_ctx.backend_ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volume));
    bind_file("/settings.tmp");
    uint32_t capabilities = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_get_capabilities(&s_ctx.file, &capabilities));
    TEST_ASSERT_EQUAL_UINT32(ESY_FILE_CAP_READ | ESY_FILE_CAP_REMOVE, capabilities);
}

void test_close_failure_preserves_resources_for_explicit_cleanup_retry(void)
{
    bind_file("/settings.tmp");
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_replace(&s_ctx.file, "x", 1u));
    s_ctx.fail_close = true;
    char output = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_file_read(&s_ctx.file, 0u, &output, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.volume.open_files);
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_deinit(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_file_fs_deinit(&s_ctx.binding));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.volume.leases);
    s_ctx.fail_close = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_file_fs_deinit(&s_ctx.binding));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.volume.open_files);
}

void test_invalid_binding_paths_never_acquire_a_lease(void)
{
    const char *paths[] = {"relative", "/", "/a/../b", "/a/./b", "/a//b", "/a/", "/a:b", "/a\\b"};
    esy_file_fs_config_t config = {0};
    config.volume = &s_ctx.volume;
    config.capacity = 100u;
    for (size_t i = 0u; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        config.path = paths[i];
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_file_fs_init(&s_ctx.binding, &config));
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.volume.leases);
    }
}
