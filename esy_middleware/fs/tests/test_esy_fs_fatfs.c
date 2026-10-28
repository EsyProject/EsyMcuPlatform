/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_fs.h"
#include "esy_fs_fatfs.h"
#include "diskio.h"
#include <string.h>

#define TEST_SECTOR_SIZE 512u
#define TEST_SECTOR_COUNT 2048u
#define TEST_FILE_COUNT 2u
#define TEST_DIR_COUNT 1u
#define TEST_FORMAT_BYTES 4096u
#define TEST_FAT32_SECTOR_COUNT 131072u
#define TEST_MBR_PARTITION_LBA_OFFSET 454u

typedef struct {
    uint8_t bytes[TEST_SECTOR_SIZE * TEST_SECTOR_COUNT];
    uint8_t *image;
    uint64_t sector_count;
    bool ready;
    bool read_only;
    bool fail_read;
    bool fail_write;
    bool fail_sync;
    bool fail_fsinfo_write;
    bool fsinfo_failed;
    bool sync_after_fsinfo_failure;
    uint64_t fsinfo_sector;
    size_t writes;
    size_t syncs;
} test_disk_t;

typedef struct {
    test_disk_t disks[FF_VOLUMES];
    uint8_t fat32_image[TEST_SECTOR_SIZE * TEST_FAT32_SECTOR_COUNT];
    esy_fs_fatfs_t backends[FF_VOLUMES];
    esy_fs_t volumes[FF_VOLUMES];
    FATFS natives[FF_VOLUMES];
    esy_fs_fatfs_file_slot_t native_files[FF_VOLUMES][TEST_FILE_COUNT];
    esy_fs_fatfs_dir_slot_t native_dirs[FF_VOLUMES][TEST_DIR_COUNT];
    esy_fs_fatfs_static_resources_t resources[FF_VOLUMES];
    uint8_t work[FF_VOLUMES][TEST_FORMAT_BYTES];
    esy_fs_file_t files[FF_VOLUMES][TEST_FILE_COUNT];
    esy_fs_dir_t dirs[FF_VOLUMES][TEST_DIR_COUNT];
} test_fatfs_ctx_t;

static test_fatfs_ctx_t s_ctx;

static esy_err_t disk_state(void *context, esy_fs_block_status_t *status)
{
    test_disk_t *disk = context;
    *status = (esy_fs_block_status_t){ .ready = disk->ready, .read_only = disk->read_only };
    return ESY_OK;
}

static esy_err_t read_sectors(void *context, uint64_t sector, uint32_t count, void *buffer)
{
    test_disk_t *disk = context;
    if (disk->fail_read) return ESY_ERR_IO;
    if (sector >= disk->sector_count || count > disk->sector_count - sector) return ESY_ERR_INVALID_ARG;
    memcpy(buffer, disk->image + sector * TEST_SECTOR_SIZE, (size_t)count * TEST_SECTOR_SIZE);
    return ESY_OK;
}

static esy_err_t write_sectors(void *context, uint64_t sector, uint32_t count, const void *buffer)
{
    test_disk_t *disk = context;
    ++disk->writes;
    if (disk->fail_write) return ESY_ERR_IO;
    if (sector >= disk->sector_count || count > disk->sector_count - sector) return ESY_ERR_INVALID_ARG;
    if (disk->fail_fsinfo_write && sector <= disk->fsinfo_sector && disk->fsinfo_sector - sector < count) {
        disk->fsinfo_failed = true;
        return ESY_ERR_IO;
    }
    memcpy(disk->image + sector * TEST_SECTOR_SIZE, buffer, (size_t)count * TEST_SECTOR_SIZE);
    return ESY_OK;
}

static esy_err_t sync_sectors(void *context)
{
    test_disk_t *disk = context;
    ++disk->syncs;
    if (disk->fsinfo_failed) disk->sync_after_fsinfo_failure = true;
    return disk->fail_sync ? ESY_ERR_IO : ESY_OK;
}

static esy_fs_fatfs_config_t configuration(size_t index)
{
    s_ctx.resources[index] = (esy_fs_fatfs_static_resources_t){
        .volume = &s_ctx.natives[index],
        .files = s_ctx.native_files[index], .file_count = TEST_FILE_COUNT,
        .dirs = s_ctx.native_dirs[index], .dir_count = TEST_DIR_COUNT,
        .format_buffer = s_ctx.work[index], .format_buffer_size = TEST_FORMAT_BYTES,
    };
    esy_fs_fatfs_config_t config = {
        .block = {
            .context = &s_ctx.disks[index], .status = disk_state,
            .read = read_sectors, .write = write_sectors, .sync = sync_sectors,
            .sector_count = TEST_SECTOR_COUNT, .sector_size = TEST_SECTOR_SIZE,
            .erase_block_sectors = 1,
        },
        .static_resources = &s_ctx.resources[index],
    };
    return config;
}

static void initialize(size_t index)
{
    esy_fs_fatfs_config_t config = configuration(index);
    esy_fs_ops_t ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_init(&s_ctx.backends[index], &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_get_ops(&s_ctx.backends[index], &ops));
    TEST_ASSERT_NULL(ops.replace);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_init(&s_ctx.volumes[index], &ops));
}

static void mount_new(size_t index)
{
    initialize(index);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_format(&s_ctx.volumes[index]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[index]));
}

static void write_file(size_t volume, size_t file, const char *path, const char *text)
{
    size_t transferred = 0;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[volume], &s_ctx.files[volume][file], path,
                                        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.files[volume][file], text, strlen(text), &transferred));
    TEST_ASSERT_EQUAL(strlen(text), transferred);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.files[volume][file]));
}

void setUp(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    for (size_t i = 0; i < FF_VOLUMES; ++i) {
        s_ctx.disks[i].ready = true;
        s_ctx.disks[i].image = s_ctx.disks[i].bytes;
        s_ctx.disks[i].sector_count = TEST_SECTOR_COUNT;
    }
}

void tearDown(void)
{
    for (size_t i = 0; i < FF_VOLUMES; ++i) {
        for (size_t j = 0; j < TEST_FILE_COUNT; ++j)
            if (s_ctx.files[i][j].volume != NULL) (void)esy_fs_close(&s_ctx.files[i][j]);
        for (size_t j = 0; j < TEST_DIR_COUNT; ++j)
            if (s_ctx.dirs[i][j].volume != NULL) (void)esy_fs_dir_close(&s_ctx.dirs[i][j]);
        if (s_ctx.volumes[i].mounted) {
            (void)esy_fs_unmount(&s_ctx.volumes[i]);
            if (s_ctx.volumes[i].mounted) (void)esy_fs_unmount(&s_ctx.volumes[i]);
        }
        (void)esy_fs_deinit(&s_ctx.volumes[i]);
        (void)esy_fs_fatfs_deinit(&s_ctx.backends[i]);
    }
}

void test_fatfs_mount_never_formats_uninitialized_media(void)
{
    initialize(0);
    TEST_ASSERT_EQUAL(ESY_FS_ERR_CORRUPT, esy_fs_mount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(0, s_ctx.disks[0].writes);
    TEST_ASSERT_FALSE(s_ctx.volumes[0].mounted);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_format(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_format(&s_ctx.volumes[0]));
}

void test_fatfs_validates_geometry_resources_copies_and_reinitialization(void)
{
    esy_fs_fatfs_config_t config = configuration(0);
    config.block.sector_size = 1024;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_fatfs_init(&s_ctx.backends[0], &config));
    config = configuration(0);
    s_ctx.resources[0].format_buffer_size = TEST_SECTOR_SIZE - 1u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_fatfs_init(&s_ctx.backends[0], &config));
    config = configuration(0);
    s_ctx.resources[0].format_buffer = s_ctx.resources[0].volume;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_fatfs_init(&s_ctx.backends[0], &config));
    config = configuration(0);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_init(&s_ctx.backends[0], &config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_fatfs_init(&s_ctx.backends[0], &config));
    esy_fs_fatfs_t copied = s_ctx.backends[0];
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_fatfs_deinit(&copied));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_fatfs_init(&s_ctx.backends[1], &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_deinit(&s_ctx.backends[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_deinit(&s_ctx.backends[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_init(&s_ctx.backends[0], &config));
}

void test_fatfs_real_provider_roundtrip_survives_unmount_and_preserves_missing_outputs(void)
{
    mount_new(0);
    esy_fs_stat_t stat = { .type = ESY_FS_TYPE_DIR, .size = 123 };
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NOT_FOUND, esy_fs_stat(&s_ctx.volumes[0], "/missing", &stat));
    TEST_ASSERT_EQUAL(123, stat.size);
    write_file(0, 0, "/device settings.bin", "persistent content");
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.volumes[0], "/device settings.bin", &stat));
    TEST_ASSERT_EQUAL(ESY_FS_TYPE_FILE, stat.type);
    TEST_ASSERT_EQUAL(18, stat.size);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0],
                                        "/device settings.bin", ESY_FS_OPEN_READ));
    char data[32] = {0};
    size_t transferred;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&s_ctx.files[0][0], data, sizeof(data), &transferred));
    TEST_ASSERT_EQUAL(18, transferred);
    TEST_ASSERT_EQUAL_STRING("persistent content", data);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&s_ctx.files[0][0], data, sizeof(data), &transferred));
    TEST_ASSERT_EQUAL(0, transferred);
}

void test_fatfs_unicode_directory_enumeration_rename_and_remove(void)
{
    mount_new(0);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mkdir(&s_ctx.volumes[0], "/logs"));
    write_file(0, 0, "/logs/\xe8\xae\xbe\xe5\xa4\x87.txt", "data");
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.volumes[0], &s_ctx.dirs[0][0], "/logs"));
    esy_fs_entry_t entry;
    bool end = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_read(&s_ctx.dirs[0][0], &entry, &end));
    TEST_ASSERT_FALSE(end);
    TEST_ASSERT_EQUAL_STRING("\xe8\xae\xbe\xe5\xa4\x87.txt", entry.name);
    TEST_ASSERT_EQUAL(4, entry.stat.size);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_read(&s_ctx.dirs[0][0], &entry, &end));
    TEST_ASSERT_TRUE(end);
    TEST_ASSERT_EQUAL_STRING("\xe8\xae\xbe\xe5\xa4\x87.txt", entry.name);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_close(&s_ctx.dirs[0][0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_rename(&s_ctx.volumes[0], "/logs/\xe8\xae\xbe\xe5\xa4\x87.txt", "/logs/new.txt"));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_remove(&s_ctx.volumes[0], "/logs/new.txt"));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_remove(&s_ctx.volumes[0], "/logs"));
}

void test_fatfs_exclusive_truncate_and_directory_type_behavior(void)
{
    mount_new(0);
    write_file(0, 0, "/file", "12345");
    TEST_ASSERT_EQUAL(ESY_FS_ERR_EXISTS, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE | ESY_FS_OPEN_EXCLUSIVE));
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NOT_FOUND, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/absent",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_TRUNCATE));
    TEST_ASSERT_EQUAL(ESY_FS_ERR_IS_DIR, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/", ESY_FS_OPEN_READ));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_TRUNCATE));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.files[0][0]));
    esy_fs_stat_t stat;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.volumes[0], "/file", &stat));
    TEST_ASSERT_EQUAL(0, stat.size);
}

void test_fatfs_append_reseeks_before_writing_and_seek_cannot_extend_file(void)
{
    mount_new(0);
    write_file(0, 0, "/file", "abc");
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_APPEND));
    uint64_t position = 99;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_seek(&s_ctx.files[0][0], 0, ESY_FS_SEEK_SET, &position));
    size_t transferred;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.files[0][0], "d", 1, &transferred));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_seek(&s_ctx.files[0][0], 1, ESY_FS_SEEK_END, &position));
    TEST_ASSERT_EQUAL(0, position);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_seek(&s_ctx.files[0][0], INT64_MIN, ESY_FS_SEEK_CUR, &position));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.files[0][0]));
    esy_fs_stat_t stat;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.volumes[0], "/file", &stat));
    TEST_ASSERT_EQUAL(4, stat.size);
}

void test_fatfs_multiple_volumes_route_to_separate_block_devices(void)
{
    mount_new(0);
    mount_new(1);
    write_file(0, 0, "/first.txt", "one");
    write_file(1, 0, "/second.txt", "two");
    esy_fs_stat_t stat;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NOT_FOUND, esy_fs_stat(&s_ctx.volumes[1], "/first.txt", &stat));
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NOT_FOUND, esy_fs_stat(&s_ctx.volumes[0], "/second.txt", &stat));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.volumes[0], "/first.txt", &stat));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.volumes[1], "/second.txt", &stat));
}

void test_fatfs_native_share_checks_prevent_conflicting_opens_remove_and_rename(void)
{
    mount_new(0);
    write_file(0, 0, "/file", "data");
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file", ESY_FS_OPEN_WRITE));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][1], "/file", ESY_FS_OPEN_READ));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_remove(&s_ctx.volumes[0], "/file"));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_rename(&s_ctx.volumes[0], "/file", "/other"));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_fatfs_deinit(&s_ctx.backends[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.files[0][0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_remove(&s_ctx.volumes[0], "/file"));
}

void test_fatfs_removed_media_faults_until_all_objects_closed_and_volume_remounted(void)
{
    mount_new(0);
    write_file(0, 0, "/file", "data");
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file", ESY_FS_OPEN_READ));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.volumes[0], &s_ctx.dirs[0][0], "/"));
    s_ctx.disks[0].ready = false;
    char byte;
    size_t transferred = 99;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NO_MEDIA, esy_fs_read(&s_ctx.files[0][0], &byte, 1, &transferred));
    TEST_ASSERT_EQUAL(99, transferred);
    s_ctx.disks[0].ready = true;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NO_MEDIA, esy_fs_read(&s_ctx.files[0][0], &byte, 1, &transferred));
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NO_MEDIA, esy_fs_close(&s_ctx.files[0][0]));
    TEST_ASSERT_NULL(s_ctx.files[0][0].volume);
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NO_MEDIA, esy_fs_dir_close(&s_ctx.dirs[0][0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file", ESY_FS_OPEN_WRITE));
}

void test_fatfs_sync_failure_consumes_close_and_cleans_native_locks_at_unmount(void)
{
    mount_new(0);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/new",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    size_t transferred;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.files[0][0], "pending", 7, &transferred));
    s_ctx.disks[0].fail_sync = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_close(&s_ctx.files[0][0]));
    TEST_ASSERT_NULL(s_ctx.files[0][0].volume);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/new", ESY_FS_OPEN_READ));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volumes[0]));
    s_ctx.disks[0].fail_sync = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/new", ESY_FS_OPEN_WRITE));
}

void test_fatfs_block_write_failure_faults_volume_and_preserves_transfer_output(void)
{
    mount_new(0);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    s_ctx.disks[0].fail_write = true;
    uint8_t data[TEST_SECTOR_SIZE] = {0};
    size_t transferred = 123;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_write(&s_ctx.files[0][0], data, sizeof(data), &transferred));
    TEST_ASSERT_EQUAL(123, transferred);
    s_ctx.disks[0].fail_write = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_close(&s_ctx.files[0][0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
}

void test_fatfs_full_volume_reports_short_writes_without_claiming_all_bytes_committed(void)
{
    mount_new(0);
    esy_fs_info_t before;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_get_info(&s_ctx.volumes[0], &before));
    TEST_ASSERT_TRUE(before.capacity_bytes > 0);
    TEST_ASSERT_TRUE(before.free_bytes <= before.capacity_bytes);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/full",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    uint8_t chunk[TEST_FORMAT_BYTES] = {0};
    bool full = false;
    for (size_t i = 0; i <= TEST_SECTOR_COUNT * TEST_SECTOR_SIZE / sizeof(chunk); ++i) {
        size_t transferred = 0;
        TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.files[0][0], chunk, sizeof(chunk), &transferred));
        if (transferred < sizeof(chunk)) { full = true; break; }
    }
    TEST_ASSERT_TRUE(full);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.files[0][0]));
    esy_fs_info_t after;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_get_info(&s_ctx.volumes[0], &after));
    TEST_ASSERT_EQUAL_UINT64(0, after.free_bytes);
}

void test_fatfs_read_only_media_and_invalid_fat_names_do_not_fault_read_access(void)
{
    mount_new(0);
    write_file(0, 0, "/file", "data");
    s_ctx.disks[0].read_only = true;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_READ_ONLY, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file", ESY_FS_OPEN_WRITE));
    esy_fs_stat_t stat;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.volumes[0], "/file", &stat));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_stat(&s_ctx.volumes[0], "/file.", &stat));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_stat(&s_ctx.volumes[0], "/wild*card", &stat));
    bool atomic = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_supports_atomic_replace(&s_ctx.volumes[0], &atomic));
    TEST_ASSERT_FALSE(atomic);
}

void test_fatfs_diskio_validates_unregistered_drives_and_sector_ranges(void)
{
    initialize(0);
    uint8_t buffer[TEST_SECTOR_SIZE] = {0};
    TEST_ASSERT_EQUAL(RES_NOTRDY, disk_read(FF_VOLUMES, buffer, 0, 1));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_read(s_ctx.backends[0].drive, buffer, TEST_SECTOR_COUNT, 1));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_write(s_ctx.backends[0].drive, buffer, TEST_SECTOR_COUNT - 1u, 2));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_ioctl(s_ctx.backends[0].drive, GET_BLOCK_SIZE, NULL));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_ioctl(s_ctx.backends[0].drive, 255, buffer));
    s_ctx.disks[0].ready = false;
    TEST_ASSERT_TRUE(disk_status(s_ctx.backends[0].drive) & STA_NODISK);
}

void test_fatfs_media_not_ready_does_not_register_a_native_mount(void)
{
    initialize(0);
    s_ctx.disks[0].ready = false;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NO_MEDIA, esy_fs_mount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NO_MEDIA, esy_fs_format(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_deinit(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_deinit(&s_ctx.backends[0]));
}

void test_fatfs_pool_exhaustion_preserves_existing_objects(void)
{
    mount_new(0);
    write_file(0, 0, "/file", "data");
    for (size_t i = 0; i < TEST_FILE_COUNT; ++i)
        TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][i], "/file", ESY_FS_OPEN_READ));
    esy_fs_file_t extra = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_fs_open(&s_ctx.volumes[0], &extra, "/file", ESY_FS_OPEN_READ));
    TEST_ASSERT_NULL(extra.volume);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.volumes[0], &s_ctx.dirs[0][0], "/"));
    esy_fs_dir_t extra_dir = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_fs_dir_open(&s_ctx.volumes[0], &extra_dir, "/"));
    TEST_ASSERT_NULL(extra_dir.volume);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.files[0][0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file", ESY_FS_OPEN_READ));
}

void test_fatfs_registry_exhaustion_releases_drive_for_explicit_reinitialization(void)
{
    for (size_t i = 0; i < FF_VOLUMES; ++i) initialize(i);
    FATFS native;
    uint8_t work[TEST_SECTOR_SIZE];
    esy_fs_fatfs_t extra = {0};
    esy_fs_fatfs_config_t config = configuration(0);
    const esy_fs_fatfs_static_resources_t resources = {
        .volume = &native, .format_buffer = work, .format_buffer_size = sizeof(work),
    };
    config.static_resources = &resources;
    TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_fs_fatfs_init(&extra, &config));
    TEST_ASSERT_NULL(extra.self);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_deinit(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_deinit(&s_ctx.backends[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_init(&extra, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_deinit(&extra));
}

void test_fatfs_wrong_directory_type_and_nonempty_removal_have_distinct_errors(void)
{
    mount_new(0);
    TEST_ASSERT_EQUAL(ESY_FS_ERR_EXISTS, esy_fs_mkdir(&s_ctx.volumes[0], "/"));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mkdir(&s_ctx.volumes[0], "/dir"));
    write_file(0, 0, "/dir/file", "data");
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_remove(&s_ctx.volumes[0], "/dir"));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_remove(&s_ctx.volumes[0], "/"));
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NOT_DIR, esy_fs_dir_open(&s_ctx.volumes[0], &s_ctx.dirs[0][0], "/dir/file"));
    TEST_ASSERT_EQUAL(ESY_FS_ERR_IS_DIR, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/dir", ESY_FS_OPEN_READ));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_remove(&s_ctx.volumes[0], "/dir/file"));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_remove(&s_ctx.volumes[0], "/dir"));
}

void test_fatfs_block_read_failure_requires_explicit_unmount_before_recovery(void)
{
    mount_new(0);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    uint8_t buffer[TEST_SECTOR_SIZE] = {0};
    size_t transferred;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.files[0][0], buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.files[0][0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file", ESY_FS_OPEN_READ));
    s_ctx.disks[0].fail_read = true;
    transferred = 99;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_read(&s_ctx.files[0][0], buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL(99, transferred);
    s_ctx.disks[0].fail_read = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_close(&s_ctx.files[0][0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file", ESY_FS_OPEN_READ));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&s_ctx.files[0][0], buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL(sizeof(buffer), transferred);
}

void test_fatfs_copies_resource_descriptor_instead_of_borrowing_configuration(void)
{
    initialize(0);
    memset(&s_ctx.resources[0], 0, sizeof(s_ctx.resources[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_format(&s_ctx.volumes[0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
    write_file(0, 0, "/owned.txt", "resource lifetime");
    esy_fs_stat_t stat;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.volumes[0], "/owned.txt", &stat));
    TEST_ASSERT_EQUAL(17, stat.size);
}

void test_fatfs_propagates_auxiliary_fsinfo_error_even_when_native_sync_returns_success(void)
{
    s_ctx.disks[0].image = s_ctx.fat32_image;
    s_ctx.disks[0].sector_count = TEST_FAT32_SECTOR_COUNT;
    esy_fs_fatfs_config_t config = configuration(0);
    config.block.sector_count = TEST_FAT32_SECTOR_COUNT;
    esy_fs_ops_t ops;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_init(&s_ctx.backends[0], &config));
    /* Prepare interoperable FAT32 media before binding any facade or mounting.
     * This exclusive fixture phase models a card formatted by an external host. */
    char drive[] = { (char)('0' + s_ctx.backends[0].drive), ':', '\0' };
    const MKFS_PARM parameters = { .fmt = FM_FAT32 };
    TEST_ASSERT_EQUAL(FR_OK, f_mkfs(drive, &parameters, s_ctx.work[0], sizeof(s_ctx.work[0])));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_fatfs_get_ops(&s_ctx.backends[0], &ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_init(&s_ctx.volumes[0], &ops));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
    const uint8_t *partition_lba = s_ctx.fat32_image + TEST_MBR_PARTITION_LBA_OFFSET;
    uint32_t start = (uint32_t)partition_lba[0] | ((uint32_t)partition_lba[1] << 8) |
                     ((uint32_t)partition_lba[2] << 16) | ((uint32_t)partition_lba[3] << 24);
    TEST_ASSERT_TRUE(start > 0);
    TEST_ASSERT_EQUAL_MEMORY("FAT32   ", s_ctx.fat32_image + (size_t)start * TEST_SECTOR_SIZE + 82u, 8);
    s_ctx.disks[0].fsinfo_sector = (uint64_t)start + 1u;
    /* The provider updates FSInfo after its free cluster count becomes known. */
    esy_fs_info_t info;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_get_info(&s_ctx.volumes[0], &info));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.volumes[0], &s_ctx.files[0][0], "/file",
        ESY_FS_OPEN_WRITE | ESY_FS_OPEN_CREATE));
    uint8_t buffer[TEST_SECTOR_SIZE] = {0};
    size_t transferred;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.files[0][0], buffer, sizeof(buffer), &transferred));
    s_ctx.disks[0].fail_fsinfo_write = true;
    esy_err_t sync_error = esy_fs_sync(&s_ctx.files[0][0]);
    TEST_ASSERT_TRUE(s_ctx.disks[0].fsinfo_failed);
    TEST_ASSERT_TRUE(s_ctx.disks[0].sync_after_fsinfo_failure);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, sync_error);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_close(&s_ctx.files[0][0]));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.volumes[0]));
    s_ctx.disks[0].fail_fsinfo_write = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.volumes[0]));
    esy_fs_stat_t stat;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.volumes[0], "/file", &stat));
    TEST_ASSERT_EQUAL(sizeof(buffer), stat.size);
}
