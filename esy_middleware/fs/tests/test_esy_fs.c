/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "unity.h"
#include "esy_fs.h"
#include <string.h>

#define TEST_SENTINEL 0xA5u
#define TEST_CALLS 19u

typedef enum {
    CALL_MOUNT, CALL_UNMOUNT, CALL_FORMAT, CALL_INFO, CALL_OPEN, CALL_CLOSE,
    CALL_READ, CALL_WRITE, CALL_SEEK, CALL_SYNC, CALL_STAT, CALL_REMOVE,
    CALL_RENAME, CALL_MKDIR, CALL_DIR_OPEN, CALL_DIR_READ, CALL_DIR_CLOSE,
    CALL_REPLACE
} callback_id_t;

typedef struct {
    esy_fs_t fs;
    esy_fs_ops_t ops;
    esy_fs_file_t file;
    esy_fs_dir_t dir;
    unsigned calls[TEST_CALLS];
    esy_err_t result;
    bool released;
    bool end;
    bool reenter;
    bool invalid_info;
    bool invalid_stat;
    bool unterminated_name;
    const char *entry_name;
    size_t transferred;
    uint64_t position;
    unsigned file_object;
    unsigned dir_object;
} fs_test_ctx_t;
static fs_test_ctx_t s_ctx;

static esy_err_t called(void *context, callback_id_t id) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx, context);
    ++s_ctx.calls[id];
    return s_ctx.result;
}
static esy_err_t source_mount(void *context) { return called(context, CALL_MOUNT); }
static esy_err_t source_unmount(void *context) { return called(context, CALL_UNMOUNT); }
static esy_err_t source_format(void *context) { return called(context, CALL_FORMAT); }
static esy_err_t source_info(void *context, esy_fs_info_t *info) {
    esy_err_t result = called(context, CALL_INFO);
    *info = (esy_fs_info_t){.capacity_bytes = 512u, .free_bytes = 128u};
    if (s_ctx.invalid_info) info->free_bytes = info->capacity_bytes + 1u;
    if (s_ctx.reenter) {
        esy_fs_stat_t stat = {.type = ESY_FS_TYPE_DIR, .size = TEST_SENTINEL};
        bool supported = false;
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_stat(&s_ctx.fs, "/entry", &stat));
        TEST_ASSERT_EQUAL_UINT64(TEST_SENTINEL, stat.size);
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.fs));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_deinit(&s_ctx.fs));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_init(&s_ctx.fs, &s_ctx.ops));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_retain(&s_ctx.fs));
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_supports_atomic_replace(&s_ctx.fs, &supported));
        TEST_ASSERT_FALSE(supported);
    }
    return result;
}
static esy_err_t source_open(void *context, const char *path, uint32_t flags, void **object) {
    (void)path; (void)flags;
    *object = &s_ctx.file_object;
    return called(context, CALL_OPEN);
}
static esy_err_t source_close(void *context, void *object, bool *released) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx.file_object, object);
    *released = s_ctx.released;
    return called(context, CALL_CLOSE);
}
static esy_err_t source_read(void *context, void *object, void *data, size_t size, size_t *transferred) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx.file_object, object);
    if (size > 0u) ((unsigned char *)data)[0] = 'x';
    *transferred = s_ctx.transferred;
    return called(context, CALL_READ);
}
static esy_err_t source_write(void *context, void *object, const void *data, size_t size, size_t *transferred) {
    (void)data; (void)size;
    TEST_ASSERT_EQUAL_PTR(&s_ctx.file_object, object);
    *transferred = s_ctx.transferred;
    return called(context, CALL_WRITE);
}
static esy_err_t source_seek(void *context, void *object, int64_t offset, esy_fs_whence_t whence, uint64_t *position) {
    (void)offset; (void)whence;
    TEST_ASSERT_EQUAL_PTR(&s_ctx.file_object, object);
    *position = s_ctx.position;
    return called(context, CALL_SEEK);
}
static esy_err_t source_sync(void *context, void *object) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx.file_object, object);
    return called(context, CALL_SYNC);
}
static esy_err_t source_stat(void *context, const char *path, esy_fs_stat_t *stat) {
    (void)path;
    *stat = (esy_fs_stat_t){.type = ESY_FS_TYPE_FILE, .size = 9u};
    if (s_ctx.invalid_stat) stat->type = (esy_fs_type_t)99;
    return called(context, CALL_STAT);
}
static esy_err_t source_remove(void *context, const char *path) { (void)path; return called(context, CALL_REMOVE); }
static esy_err_t source_rename(void *context, const char *from, const char *to) {
    (void)from; (void)to; return called(context, CALL_RENAME);
}
static esy_err_t source_mkdir(void *context, const char *path) { (void)path; return called(context, CALL_MKDIR); }
static esy_err_t source_dir_open(void *context, const char *path, void **object) {
    (void)path; *object = &s_ctx.dir_object;
    return called(context, CALL_DIR_OPEN);
}
static esy_err_t source_dir_read(void *context, void *object, esy_fs_entry_t *entry, bool *end) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx.dir_object, object);
    memset(entry, 0, sizeof(*entry));
    if (s_ctx.unterminated_name) memset(entry->name, 'x', sizeof(entry->name));
    else strcpy(entry->name, s_ctx.entry_name);
    entry->stat = (esy_fs_stat_t){.type = ESY_FS_TYPE_FILE, .size = 9u};
    if (s_ctx.invalid_stat) entry->stat.type = (esy_fs_type_t)99;
    *end = s_ctx.end;
    return called(context, CALL_DIR_READ);
}
static esy_err_t source_dir_close(void *context, void *object, bool *released) {
    TEST_ASSERT_EQUAL_PTR(&s_ctx.dir_object, object);
    *released = s_ctx.released;
    return called(context, CALL_DIR_CLOSE);
}
static esy_err_t source_replace(void *context, const char *path, const char *temporary_path, const void *data, size_t size) {
    (void)path; (void)temporary_path; (void)data; (void)size;
    return called(context, CALL_REPLACE);
}
static void bind(void) { TEST_ASSERT_EQUAL(ESY_OK, esy_fs_init(&s_ctx.fs, &s_ctx.ops)); }
static void mount_volume(void) { bind(); TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.fs)); }
static void open_file(uint32_t flags) {
    mount_volume();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.fs, &s_ctx.file, "/entry", flags));
}
void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.released = true;
    s_ctx.entry_name = "entry";
    s_ctx.ops = (esy_fs_ops_t){
        .context = &s_ctx, .mount = source_mount, .unmount = source_unmount,
        .format = source_format, .info = source_info, .open = source_open,
        .close = source_close, .read = source_read, .write = source_write,
        .seek = source_seek, .sync = source_sync, .stat = source_stat,
        .remove = source_remove, .rename = source_rename, .mkdir = source_mkdir,
        .dir_open = source_dir_open, .dir_read = source_dir_read,
        .dir_close = source_dir_close, .replace = source_replace};
}
void tearDown(void) { }

void test_init_validates_every_required_callback_without_touching_media(void) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_init(NULL, &s_ctx.ops));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_init(&s_ctx.fs, NULL));
#define CHECK_REQUIRED(member) do { \
    esy_fs_ops_t incomplete = s_ctx.ops; incomplete.member = NULL; \
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_init(&s_ctx.fs, &incomplete)); \
    TEST_ASSERT_NULL(s_ctx.fs.self); \
} while (0)
    CHECK_REQUIRED(mount); CHECK_REQUIRED(unmount); CHECK_REQUIRED(format);
    CHECK_REQUIRED(info); CHECK_REQUIRED(open); CHECK_REQUIRED(close);
    CHECK_REQUIRED(read); CHECK_REQUIRED(write); CHECK_REQUIRED(seek);
    CHECK_REQUIRED(sync); CHECK_REQUIRED(stat); CHECK_REQUIRED(remove);
    CHECK_REQUIRED(rename); CHECK_REQUIRED(mkdir); CHECK_REQUIRED(dir_open);
    CHECK_REQUIRED(dir_read); CHECK_REQUIRED(dir_close);
#undef CHECK_REQUIRED
    s_ctx.ops.replace = NULL;
    bind();
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_MOUNT]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_FORMAT]);
    bool supported = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_supports_atomic_replace(&s_ctx.fs, &supported));
    TEST_ASSERT_FALSE(supported);
}
void test_lifecycle_requires_explicit_mount_and_preserves_state_after_failures(void) {
    bind();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_init(&s_ctx.fs, &s_ctx.ops));
    s_ctx.result = ESY_FS_ERR_CORRUPT;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_CORRUPT, esy_fs_mount(&s_ctx.fs));
    TEST_ASSERT_FALSE(s_ctx.fs.mounted);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_FORMAT]);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_format(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.fs));
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_format(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_deinit(&s_ctx.fs));
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_TRUE(s_ctx.fs.mounted);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_deinit(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_deinit(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_mount(&s_ctx.fs));
    bind();
}
void test_ops_are_copied_and_optional_capability_does_not_do_storage_io(void) {
    bind();
    s_ctx.ops.mount = NULL;
    s_ctx.ops.replace = NULL;
    bool supported = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_supports_atomic_replace(&s_ctx.fs, &supported));
    TEST_ASSERT_TRUE(supported);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.fs));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_MOUNT]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_REPLACE]);
}
void test_lease_blocks_unmount_until_every_borrower_releases(void) {
    bind();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_retain(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_fs_release(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_retain(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_retain(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_release(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_release(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_UNMOUNT]);
}
void test_errors_preserve_info_stat_and_seek_snapshots(void) {
    open_file(ESY_FS_OPEN_READ | ESY_FS_OPEN_WRITE);
    esy_fs_info_t info = {.capacity_bytes = TEST_SENTINEL, .free_bytes = TEST_SENTINEL};
    esy_fs_stat_t stat = {.type = ESY_FS_TYPE_DIR, .size = TEST_SENTINEL};
    uint64_t position = TEST_SENTINEL;
    s_ctx.result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_get_info(&s_ctx.fs, &info));
    TEST_ASSERT_EQUAL_UINT64(TEST_SENTINEL, info.capacity_bytes);
    TEST_ASSERT_EQUAL_UINT64(TEST_SENTINEL, info.free_bytes);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_stat(&s_ctx.fs, "/entry", &stat));
    TEST_ASSERT_EQUAL(ESY_FS_TYPE_DIR, stat.type);
    TEST_ASSERT_EQUAL_UINT64(TEST_SENTINEL, stat.size);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_seek(&s_ctx.file, -1, ESY_FS_SEEK_END, &position));
    TEST_ASSERT_EQUAL_UINT64(TEST_SENTINEL, position);
}
void test_reentry_is_busy_without_mutating_or_forwarding_nested_operations(void) {
    mount_volume();
    s_ctx.reenter = true;
    esy_fs_info_t info = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_get_info(&s_ctx.fs, &info));
    TEST_ASSERT_EQUAL_UINT64(512u, info.capacity_bytes);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_STAT]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_UNMOUNT]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.fs.leases);
    s_ctx.reenter = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
}
void test_invalid_canonical_paths_never_reach_the_backend(void) {
    static const char *const k_bad_paths[] = {
        "", "relative", "//entry", "/entry/", "/./entry", "/entry/../other",
        "/entry//other", "/one\\two", "/sd:entry", "/line\nfeed", "/tab\tentry"};
    mount_volume();
    for (size_t index = 0u; index < sizeof(k_bad_paths) / sizeof(k_bad_paths[0]); ++index) {
        const char *path = k_bad_paths[index];
        esy_fs_stat_t stat = {0};
        TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.fs, path, &stat));
        TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.fs, &s_ctx.file, path, ESY_FS_OPEN_READ));
        TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_remove(&s_ctx.fs, path));
        TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_rename(&s_ctx.fs, "/valid", path));
        TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_rename(&s_ctx.fs, path, "/valid"));
        TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_mkdir(&s_ctx.fs, path));
        TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.fs, &s_ctx.dir, path));
        TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_replace(&s_ctx.fs, "/valid", path, NULL, 0u));
    }
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_STAT]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_OPEN]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_REPLACE]);
}
void test_root_and_maximum_length_canonical_paths_are_accepted(void) {
    mount_volume();
    esy_fs_stat_t stat = {0};
    char path[ESY_FS_PATH_MAX + 2u];
    memset(path, 'a', sizeof(path)); path[0] = '/'; path[ESY_FS_PATH_MAX] = '\0';
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.fs, "/", &stat));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.fs, path, &stat));
    path[ESY_FS_PATH_MAX] = 'a'; path[ESY_FS_PATH_MAX + 1u] = '\0';
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_stat(&s_ctx.fs, path, &stat));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_STAT]);
}
void test_invalid_open_flag_combinations_never_lend_a_native_object(void) {
    static const uint32_t k_flags[] = {0u, ESY_FS_OPEN_CREATE, ESY_FS_OPEN_EXCLUSIVE,
        ESY_FS_OPEN_READ | ESY_FS_OPEN_CREATE, ESY_FS_OPEN_READ | ESY_FS_OPEN_TRUNCATE,
        ESY_FS_OPEN_READ | ESY_FS_OPEN_APPEND, ESY_FS_OPEN_WRITE | ESY_FS_OPEN_EXCLUSIVE,
        UINT32_C(0x80000000)};
    mount_volume();
    for (size_t index = 0u; index < sizeof(k_flags) / sizeof(k_flags[0]); ++index)
        TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_open(&s_ctx.fs, &s_ctx.file, "/entry", k_flags[index]));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_OPEN]);
    TEST_ASSERT_NULL(s_ctx.file.volume);
}
void test_failed_open_leaves_zero_handle_and_permits_retry(void) {
    mount_volume();
    s_ctx.result = ESY_FS_ERR_NOT_FOUND;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NOT_FOUND, esy_fs_open(&s_ctx.fs, &s_ctx.file, "/entry", ESY_FS_OPEN_READ));
    esy_fs_file_t zero = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.file, sizeof(zero));
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_mount(&s_ctx.fs));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.fs, &s_ctx.file, "/entry", ESY_FS_OPEN_READ));
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.fs, &s_ctx.file, "/other", ESY_FS_OPEN_READ));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_OPEN]);
}
void test_file_close_failure_can_retain_native_object_for_retry(void) {
    open_file(ESY_FS_OPEN_READ);
    s_ctx.result = ESY_ERR_IO; s_ctx.released = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_close(&s_ctx.file));
    TEST_ASSERT_NOT_NULL(s_ctx.file.object);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.fs));
    s_ctx.result = ESY_OK; s_ctx.released = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_CLOSE]);
}
void test_file_close_failure_can_consume_native_object_without_leaking_a_borrow(void) {
    open_file(ESY_FS_OPEN_WRITE);
    s_ctx.result = ESY_ERR_IO; s_ctx.released = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_close(&s_ctx.file));
    TEST_ASSERT_NULL(s_ctx.file.volume);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_CLOSE]);
}
void test_read_and_write_enforce_access_before_calling_the_backend(void) {
    open_file(ESY_FS_OPEN_READ);
    size_t transferred = TEST_SENTINEL;
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.file, "x", 1u, &transferred));
    TEST_ASSERT_EQUAL_UINT(TEST_SENTINEL, transferred);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_WRITE]);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_open(&s_ctx.fs, &s_ctx.file, "/entry", ESY_FS_OPEN_WRITE));
    char buffer[3];
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_read(&s_ctx.file, buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL_UINT(TEST_SENTINEL, transferred);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_READ]);
}
void test_short_reads_eof_and_short_writes_are_successful_and_explicit(void) {
    open_file(ESY_FS_OPEN_READ | ESY_FS_OPEN_WRITE);
    char buffer[8] = {0};
    size_t transferred = TEST_SENTINEL;
    s_ctx.transferred = 3u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&s_ctx.file, buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL_UINT(3u, transferred);
    s_ctx.transferred = 0u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&s_ctx.file, buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL_UINT(0u, transferred);
    s_ctx.transferred = 2u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.file, buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL_UINT(2u, transferred);
}
void test_failed_io_preserves_transfer_count_and_rejects_impossible_success_counts(void) {
    open_file(ESY_FS_OPEN_READ | ESY_FS_OPEN_WRITE);
    char buffer[8] = {0};
    size_t transferred = TEST_SENTINEL;
    s_ctx.result = ESY_ERR_IO; s_ctx.transferred = 2u;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_read(&s_ctx.file, buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL_UINT(TEST_SENTINEL, transferred);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_write(&s_ctx.file, buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL_UINT(TEST_SENTINEL, transferred);
    s_ctx.result = ESY_OK; s_ctx.transferred = sizeof(buffer) + 1u;
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_read(&s_ctx.file, buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL_UINT(TEST_SENTINEL, transferred);
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.file, buffer, sizeof(buffer), &transferred));
    TEST_ASSERT_EQUAL_UINT(TEST_SENTINEL, transferred);
}
void test_io_argument_validation_and_zero_length_spans(void) {
    open_file(ESY_FS_OPEN_READ | ESY_FS_OPEN_WRITE);
    size_t transferred = TEST_SENTINEL;
    char buffer[2];
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_read(&s_ctx.file, NULL, 1u, &transferred));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_write(&s_ctx.file, NULL, 1u, &transferred));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_read(&s_ctx.file, buffer, 1u, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_write(&s_ctx.file, buffer, 1u, NULL));
    TEST_ASSERT_EQUAL_UINT(TEST_SENTINEL, transferred);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_read(&s_ctx.file, NULL, 0u, &transferred));
    TEST_ASSERT_EQUAL_UINT(0u, transferred);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_write(&s_ctx.file, NULL, 0u, &transferred));
}
void test_moved_or_copied_live_handles_are_rejected_without_backend_io(void) {
    open_file(ESY_FS_OPEN_READ);
    esy_fs_file_t file_copy = s_ctx.file;
    esy_fs_t volume_copy = s_ctx.fs;
    esy_fs_stat_t stat = {0};
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_close(&file_copy));
    TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_fs_stat(&volume_copy, "/entry", &stat));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_CLOSE]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_STAT]);
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.file));
}
void test_directory_end_and_errors_preserve_entry_snapshots(void) {
    mount_volume();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.fs, &s_ctx.dir, "/"));
    esy_fs_entry_t entry, saved;
    memset(&entry, TEST_SENTINEL, sizeof(entry)); saved = entry;
    bool end = false;
    s_ctx.end = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_read(&s_ctx.dir, &entry, &end));
    TEST_ASSERT_TRUE(end);
    TEST_ASSERT_EQUAL_MEMORY(&saved, &entry, sizeof(entry));
    s_ctx.result = ESY_ERR_IO; s_ctx.end = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_dir_read(&s_ctx.dir, &entry, &end));
    TEST_ASSERT_TRUE(end);
    TEST_ASSERT_EQUAL_MEMORY(&saved, &entry, sizeof(entry));
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_read(&s_ctx.dir, &entry, &end));
    TEST_ASSERT_FALSE(end);
    TEST_ASSERT_EQUAL_STRING("entry", entry.name);
    TEST_ASSERT_EQUAL_UINT64(9u, entry.stat.size);
}
void test_directory_close_observes_retained_and_consumed_failure_semantics(void) {
    mount_volume();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.fs, &s_ctx.dir, "/"));
    s_ctx.result = ESY_ERR_IO; s_ctx.released = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_dir_close(&s_ctx.dir));
    TEST_ASSERT_NOT_NULL(s_ctx.dir.object);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.fs));
    s_ctx.released = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_dir_close(&s_ctx.dir));
    TEST_ASSERT_NULL(s_ctx.dir.volume);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_DIR_CLOSE]);
}
void test_failed_directory_open_does_not_hold_the_volume(void) {
    mount_volume();
    s_ctx.result = ESY_FS_ERR_NOT_DIR;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NOT_DIR, esy_fs_dir_open(&s_ctx.fs, &s_ctx.dir, "/file"));
    esy_fs_dir_t zero = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zero, &s_ctx.dir, sizeof(zero));
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
}
void test_replace_checks_reserved_temporary_path_and_forwards_backend_failures(void) {
    mount_volume();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_replace(&s_ctx.fs, "/file", "/file", "data", 4u));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_fs_replace(&s_ctx.fs, "/file", "/tmp", NULL, 4u));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_REPLACE]);
    s_ctx.result = ESY_FS_ERR_NO_SPACE;
    TEST_ASSERT_EQUAL(ESY_FS_ERR_NO_SPACE, esy_fs_replace(&s_ctx.fs, "/file", "/tmp", "data", 4u));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.calls[CALL_REPLACE]);
    s_ctx.result = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_replace(&s_ctx.fs, "/file", "/tmp", NULL, 0u));
}
void test_missing_replace_is_reported_without_attempting_generic_rename_transaction(void) {
    s_ctx.ops.replace = NULL;
    mount_volume();
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_fs_replace(&s_ctx.fs, "/file", "/tmp", "new", 3u));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_OPEN]);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.calls[CALL_RENAME]);
}

void test_malformed_backend_metadata_never_replaces_valid_caller_snapshots(void) {
    mount_volume();
    esy_fs_info_t info = {.capacity_bytes = TEST_SENTINEL, .free_bytes = TEST_SENTINEL};
    esy_fs_stat_t stat = {.type = ESY_FS_TYPE_DIR, .size = TEST_SENTINEL};
    s_ctx.invalid_info = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_get_info(&s_ctx.fs, &info));
    TEST_ASSERT_EQUAL_UINT64(TEST_SENTINEL, info.capacity_bytes);
    TEST_ASSERT_EQUAL_UINT64(TEST_SENTINEL, info.free_bytes);
    s_ctx.invalid_stat = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_stat(&s_ctx.fs, "/entry", &stat));
    TEST_ASSERT_EQUAL(ESY_FS_TYPE_DIR, stat.type);
    TEST_ASSERT_EQUAL_UINT64(TEST_SENTINEL, stat.size);
}

void test_malformed_directory_entries_preserve_entry_and_end_outputs(void) {
    static const char *const k_bad_names[] = {"", ".", "..", "part/name", "part\\name",
                                             "disk:name", "line\nname", "tab\tname"};
    mount_volume();
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.fs, &s_ctx.dir, "/"));
    esy_fs_entry_t entry, saved;
    memset(&entry, TEST_SENTINEL, sizeof(entry)); saved = entry;
    bool end = true;
    for (size_t index = 0u; index < sizeof(k_bad_names) / sizeof(k_bad_names[0]); ++index) {
        s_ctx.entry_name = k_bad_names[index];
        TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_dir_read(&s_ctx.dir, &entry, &end));
        TEST_ASSERT_TRUE(end);
        TEST_ASSERT_EQUAL_MEMORY(&saved, &entry, sizeof(entry));
    }
    s_ctx.entry_name = "valid"; s_ctx.unterminated_name = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_dir_read(&s_ctx.dir, &entry, &end));
    TEST_ASSERT_TRUE(end);
    TEST_ASSERT_EQUAL_MEMORY(&saved, &entry, sizeof(entry));
    s_ctx.unterminated_name = false; s_ctx.invalid_stat = true;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_dir_read(&s_ctx.dir, &entry, &end));
    TEST_ASSERT_TRUE(end);
    TEST_ASSERT_EQUAL_MEMORY(&saved, &entry, sizeof(entry));
}

void test_successful_backend_close_must_actually_release_the_borrowed_object(void) {
    open_file(ESY_FS_OPEN_READ);
    s_ctx.released = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_close(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.fs));
    s_ctx.released = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_close(&s_ctx.file));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_open(&s_ctx.fs, &s_ctx.dir, "/"));
    s_ctx.released = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_fs_dir_close(&s_ctx.dir));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_fs_unmount(&s_ctx.fs));
    s_ctx.released = true;
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_close(&s_ctx.dir));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_dir_close(&s_ctx.dir));
    TEST_ASSERT_EQUAL(ESY_OK, esy_fs_unmount(&s_ctx.fs));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_CLOSE]);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.calls[CALL_DIR_CLOSE]);
}
