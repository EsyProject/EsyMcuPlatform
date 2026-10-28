/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/diagnostics/esy_diagnostics.h"

#include <string.h>

#define TEST_SOURCE_COUNT 3u
#define TEST_REENTRY_COUNT 5u

typedef struct {
    esy_diagnostics_result_t result;
    esy_err_t read_status, run_status;
    unsigned reads, runs;
    bool reenter;
    esy_err_t nested_status[TEST_REENTRY_COUNT];
    esy_diagnostics_snapshot_t nested_output;
} diagnostic_source_t;

typedef struct {
    esy_diagnostics_t diagnostics;
    esy_diagnostics_config_t config;
    esy_diagnostics_static_resources_t resources;
    esy_diagnostics_entry_t entries[TEST_SOURCE_COUNT];
    char names[TEST_SOURCE_COUNT][ESY_DIAGNOSTICS_NAME_MAX_BYTES + 1u];
    diagnostic_source_t sources[TEST_SOURCE_COUNT];
    esy_diagnostics_snapshot_t output, before;
} diagnostics_test_ctx_t;

static diagnostics_test_ctx_t s_ctx;

static void attempt_reentry(diagnostic_source_t *source) {
    if (!source->reenter) return;
    memset(&source->nested_output, 0x5a, sizeof(source->nested_output));
    source->nested_status[0] = esy_diagnostics_init(&s_ctx.diagnostics, &s_ctx.config);
    source->nested_status[1] = esy_diagnostics_deinit(&s_ctx.diagnostics);
    source->nested_status[2] = esy_diagnostics_enumerate(&s_ctx.diagnostics, 0u, &source->nested_output);
    source->nested_status[3] = esy_diagnostics_read(&s_ctx.diagnostics,
        "system.health", sizeof("system.health") - 1u, &source->nested_output);
    source->nested_status[4] = esy_diagnostics_run(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &source->nested_output);
}

static esy_err_t read_source(void *context, esy_diagnostics_result_t *result) {
    diagnostic_source_t *source = context;
    ++source->reads;
    attempt_reentry(source);
    *result = source->result;
    return source->read_status;
}

static esy_err_t run_source(void *context, esy_diagnostics_result_t *result) {
    diagnostic_source_t *source = context;
    ++source->runs;
    attempt_reentry(source);
    *result = source->result;
    return source->run_status;
}

static void initialize(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_init(&s_ctx.diagnostics, &s_ctx.config));
}

static void sentinel(void) {
    memset(&s_ctx.output, 0xa5, sizeof(s_ctx.output));
    memcpy(&s_ctx.before, &s_ctx.output, sizeof(s_ctx.before));
}

static void unchanged(void) {
    TEST_ASSERT_EQUAL_MEMORY(&s_ctx.before, &s_ctx.output, sizeof(s_ctx.output));
}

static void no_callbacks(void) {
    for (size_t i = 0u; i < TEST_SOURCE_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sources[i].reads);
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sources[i].runs);
    }
}

static void expect_invalid_init(void) {
    const esy_diagnostics_t before = s_ctx.diagnostics;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_init(&s_ctx.diagnostics, &s_ctx.config));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s_ctx.diagnostics, sizeof(before));
    no_callbacks();
}

static void expect_invalid_lookup(const char *name, size_t length) {
    sentinel();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_diagnostics_read(&s_ctx.diagnostics, name, length, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_diagnostics_run(&s_ctx.diagnostics, name, length, &s_ctx.output));
    unchanged();
    no_callbacks();
}

static void expect_invalid_result(void) {
    sentinel();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    unchanged();
}

static void set_detail(diagnostic_source_t *source, const char *text) {
    source->result.detail_length = strlen(text);
    memcpy(source->result.detail, text, source->result.detail_length + 1u);
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    memcpy(s_ctx.names[0], "system.health", sizeof("system.health"));
    memcpy(s_ctx.names[1], "storage.media", sizeof("storage.media"));
    memcpy(s_ctx.names[2], "Sensor/adc_0.test-1", sizeof("Sensor/adc_0.test-1"));
    for (size_t i = 0u; i < TEST_SOURCE_COUNT; ++i) {
        s_ctx.entries[i] = (esy_diagnostics_entry_t){
            .name = s_ctx.names[i], .read = read_source,
            .run = i == 0u ? NULL : run_source, .context = &s_ctx.sources[i]};
        s_ctx.sources[i].result.health = ESY_DIAGNOSTICS_HEALTH_OK;
    }
    s_ctx.resources = (esy_diagnostics_static_resources_t){s_ctx.entries, TEST_SOURCE_COUNT};
    s_ctx.config.static_resources = &s_ctx.resources;
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_deinit(&s_ctx.diagnostics));
}

void test_diagnostics_enumeration_copies_only_metadata_without_querying_sources(void) {
    s_ctx.sources[1].result.health = ESY_DIAGNOSTICS_HEALTH_ERROR;
    s_ctx.sources[1].result.code = 404u;
    set_detail(&s_ctx.sources[1], "not queried by discovery");
    initialize();
    const char empty_detail[ESY_DIAGNOSTICS_DETAIL_MAX_BYTES + 1u] = {0};
    for (uint32_t i = 0u; i < TEST_SOURCE_COUNT; ++i) {
        TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_enumerate(&s_ctx.diagnostics, i, &s_ctx.output));
        TEST_ASSERT_EQUAL_UINT32(TEST_SOURCE_COUNT, s_ctx.output.count);
        TEST_ASSERT_EQUAL_UINT32(i, s_ctx.output.index);
        TEST_ASSERT_EQUAL_STRING(s_ctx.entries[i].name, s_ctx.output.name);
        TEST_ASSERT_EQUAL_UINT(strlen(s_ctx.entries[i].name), s_ctx.output.name_length);
        TEST_ASSERT_EQUAL_UINT32(ESY_DIAGNOSTICS_CAP_READ |
            (i == 0u ? 0u : ESY_DIAGNOSTICS_CAP_RUN), s_ctx.output.capabilities);
        TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_UNKNOWN, s_ctx.output.result.health);
        TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.output.result.code);
        TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output.result.detail_length);
        TEST_ASSERT_EQUAL_MEMORY(empty_detail, s_ctx.output.result.detail, sizeof(empty_detail));
    }
    no_callbacks();
}

void test_diagnostics_read_reports_health_code_and_detail_from_exactly_one_context(void) {
    initialize();
    s_ctx.sources[1].result.health = ESY_DIAGNOSTICS_HEALTH_WARNING;
    s_ctx.sources[1].result.code = UINT32_C(0xfedcba98);
    set_detail(&s_ctx.sources[1], "storage nearly full");
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.output.index);
    TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_WARNING, s_ctx.output.result.health);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0xfedcba98), s_ctx.output.result.code);
    TEST_ASSERT_EQUAL_STRING("storage nearly full", s_ctx.output.result.detail);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sources[1].reads);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sources[1].runs);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sources[0].reads + s_ctx.sources[2].reads);
}

void test_diagnostics_run_returns_a_completed_error_finding_as_a_successful_operation(void) {
    initialize();
    s_ctx.sources[1].result.health = ESY_DIAGNOSTICS_HEALTH_ERROR;
    s_ctx.sources[1].result.code = 7u;
    set_detail(&s_ctx.sources[1], "self-test detected a fault");
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_run(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_ERROR, s_ctx.output.result.health);
    TEST_ASSERT_EQUAL_UINT32(7u, s_ctx.output.result.code);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sources[1].runs);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sources[1].reads);
}

void test_diagnostics_unknown_health_is_permitted_for_read_but_not_for_completed_run(void) {
    initialize();
    s_ctx.sources[1].result.health = ESY_DIAGNOSTICS_HEALTH_UNKNOWN;
    s_ctx.sources[1].result.code = 2u;
    set_detail(&s_ctx.sources[1], "source has not sampled yet");
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_UNKNOWN, s_ctx.output.result.health);
    sentinel();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_diagnostics_run(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sources[1].reads);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sources[1].runs);
}

void test_diagnostics_lookup_uses_exact_spans_and_may_borrow_a_registered_name(void) {
    const char bounded[] = {'s', 't', 'o', 'r', 'a', 'g', 'e', '.', 'm', 'e', 'd', 'i', 'a'};
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_diagnostics_read(&s_ctx.diagnostics, bounded, sizeof(bounded), &s_ctx.output));
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_read(&s_ctx.diagnostics,
        s_ctx.entries[1].name, sizeof(bounded), &s_ctx.output));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sources[1].reads);
}

void test_diagnostics_name_lookup_is_case_sensitive_and_never_falls_through_to_another_source(void) {
    initialize();
    sentinel();
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_diagnostics_read(&s_ctx.diagnostics,
        "Storage.media", sizeof("Storage.media") - 1u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage", sizeof("storage") - 1u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_diagnostics_run(&s_ctx.diagnostics,
        "unknown.test", sizeof("unknown.test") - 1u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_diagnostics_run(&s_ctx.diagnostics,
        "system.health", sizeof("system.health") - 1u, &s_ctx.output));
    unchanged();
    no_callbacks();
}

void test_diagnostics_maximum_names_and_printable_detail_lengths_are_supported(void) {
    memset(s_ctx.names[1], 'Z', ESY_DIAGNOSTICS_NAME_MAX_BYTES);
    memcpy(s_ctx.names[1], "Aa09_./-", sizeof("Aa09_./-") - 1u);
    s_ctx.names[1][ESY_DIAGNOSTICS_NAME_MAX_BYTES] = '\0';
    memset(s_ctx.sources[1].result.detail, '~', ESY_DIAGNOSTICS_DETAIL_MAX_BYTES);
    s_ctx.sources[1].result.detail[0] = ' ';
    s_ctx.sources[1].result.detail[ESY_DIAGNOSTICS_DETAIL_MAX_BYTES] = '\0';
    s_ctx.sources[1].result.detail_length = ESY_DIAGNOSTICS_DETAIL_MAX_BYTES;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_run(&s_ctx.diagnostics,
        s_ctx.names[1], ESY_DIAGNOSTICS_NAME_MAX_BYTES, &s_ctx.output));
    TEST_ASSERT_EQUAL_UINT(ESY_DIAGNOSTICS_NAME_MAX_BYTES, s_ctx.output.name_length);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.names[1], s_ctx.output.name, sizeof(s_ctx.names[1]));
    TEST_ASSERT_EQUAL_UINT(ESY_DIAGNOSTICS_DETAIL_MAX_BYTES, s_ctx.output.result.detail_length);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.sources[1].result.detail,
        s_ctx.output.result.detail, sizeof(s_ctx.output.result.detail));
}

void test_diagnostics_trailing_detail_storage_is_ignored_after_the_declared_terminator(void) {
    set_detail(&s_ctx.sources[1], "ok");
    s_ctx.sources[1].result.detail[ESY_DIAGNOSTICS_DETAIL_MAX_BYTES] = (char)0xff;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    TEST_ASSERT_EQUAL_STRING("ok", s_ctx.output.result.detail);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.output.result.detail_length);
}

void test_diagnostics_invalid_lookup_names_do_not_invoke_a_callback_or_change_output(void) {
    initialize();
    expect_invalid_lookup(NULL, 1u);
    expect_invalid_lookup("", 0u);
    expect_invalid_lookup(s_ctx.names[1], ESY_DIAGNOSTICS_NAME_MAX_BYTES + 1u);
    expect_invalid_lookup("invalid name", sizeof("invalid name") - 1u);
    expect_invalid_lookup("embedded\0nul", sizeof("embedded\0nul") - 1u);
    expect_invalid_lookup("line\n", sizeof("line\n") - 1u);
    const char high[] = {(char)0x80};
    expect_invalid_lookup(high, sizeof(high));
    expect_invalid_lookup((const char *)(uintptr_t)(UINTPTR_MAX - 3u), 8u);
}

void test_diagnostics_provider_errors_and_positive_returns_preserve_output_without_retry(void) {
    initialize();
    s_ctx.sources[1].read_status = ESY_ERR_IO;
    sentinel();
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    unchanged();
    s_ctx.sources[1].run_status = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_diagnostics_run(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    unchanged();
    s_ctx.sources[1].read_status = (esy_err_t)1;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    unchanged();
    s_ctx.sources[1].run_status = (esy_err_t)42;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_diagnostics_run(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sources[1].reads);
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.sources[1].runs);
    s_ctx.sources[1].read_status = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
}

void test_diagnostics_malformed_health_detail_and_terminator_are_not_published(void) {
    initialize();
    s_ctx.sources[1].result.health = (esy_diagnostics_health_t)4;
    expect_invalid_result();
    s_ctx.sources[1].result.health = (esy_diagnostics_health_t)-1;
    expect_invalid_result();
    s_ctx.sources[1].result.health = ESY_DIAGNOSTICS_HEALTH_OK;
    s_ctx.sources[1].result.detail_length = ESY_DIAGNOSTICS_DETAIL_MAX_BYTES + 1u;
    expect_invalid_result();
    s_ctx.sources[1].result.detail_length = SIZE_MAX;
    expect_invalid_result();
    s_ctx.sources[1].result.detail_length = 2u;
    memcpy(s_ctx.sources[1].result.detail, "okx", sizeof("okx"));
    expect_invalid_result();
    s_ctx.sources[1].result.detail[2] = '\0';
    const unsigned char invalid[] = {0u, 0x1fu, 0x7fu, 0x80u, 0xffu};
    for (size_t i = 0u; i < sizeof(invalid); ++i) {
        s_ctx.sources[1].result.detail[0] = (char)invalid[i];
        expect_invalid_result();
    }
}

void test_diagnostics_callback_reentry_is_rejected_for_every_api_without_nested_source_work(void) {
    s_ctx.sources[1].reenter = true;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_read(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    for (size_t i = 0u; i < TEST_REENTRY_COUNT; ++i)
        TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.sources[1].nested_status[i]);
    const unsigned char *bytes = (const unsigned char *)&s_ctx.sources[1].nested_output;
    for (size_t i = 0u; i < sizeof(s_ctx.sources[1].nested_output); ++i)
        TEST_ASSERT_EQUAL_HEX8(0x5au, bytes[i]);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.sources[1].reads);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sources[1].runs);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.sources[0].reads);
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_enumerate(&s_ctx.diagnostics, 1u, &s_ctx.output));
}

void test_diagnostics_empty_inventory_has_explicit_zero_metadata_without_placeholder_callbacks(void) {
    s_ctx.resources = (esy_diagnostics_static_resources_t){0};
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_enumerate(&s_ctx.diagnostics, 0u, &s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.output.count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.output.index);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.output.capabilities);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.output.name_length);
    TEST_ASSERT_EQUAL_UINT8(0u, s_ctx.output.name[0]);
    TEST_ASSERT_EQUAL(ESY_DIAGNOSTICS_HEALTH_UNKNOWN, s_ctx.output.result.health);
    sentinel();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_diagnostics_enumerate(&s_ctx.diagnostics, 1u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_diagnostics_read(&s_ctx.diagnostics,
        "unknown", sizeof("unknown") - 1u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_diagnostics_run(&s_ctx.diagnostics,
        "unknown", sizeof("unknown") - 1u, &s_ctx.output));
    unchanged();
    no_callbacks();
}

void test_diagnostics_lifecycle_copies_the_binding_without_querying_or_resetting_sources(void) {
    esy_diagnostics_static_resources_t resources = s_ctx.resources;
    const esy_diagnostics_config_t config = {.static_resources = &resources};
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_init(&s_ctx.diagnostics, &config));
    resources = (esy_diagnostics_static_resources_t){0};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_diagnostics_init(&s_ctx.diagnostics, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_enumerate(&s_ctx.diagnostics, 2u, &s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(TEST_SOURCE_COUNT, s_ctx.output.count);
    no_callbacks();
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_deinit(&s_ctx.diagnostics));
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_deinit(&s_ctx.diagnostics));
    sentinel();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_diagnostics_enumerate(&s_ctx.diagnostics, 0u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_diagnostics_read(&s_ctx.diagnostics,
        "system.health", sizeof("system.health") - 1u, &s_ctx.output));
    unchanged();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_diagnostics_run(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u, &s_ctx.output));
    unchanged();
    initialize();
}

void test_diagnostics_missing_invalid_duplicate_or_unterminated_registrations_are_rejected(void) {
    s_ctx.resources.entries = NULL;
    expect_invalid_init();
    s_ctx.resources.entries = s_ctx.entries;
    const esy_diagnostics_entry_t original = s_ctx.entries[1];
    s_ctx.entries[1].read = NULL;
    expect_invalid_init();
    s_ctx.entries[1] = original;
    s_ctx.entries[1].name = NULL;
    expect_invalid_init();
    s_ctx.entries[1].name = "";
    expect_invalid_init();
    s_ctx.entries[1].name = "invalid name";
    expect_invalid_init();
    s_ctx.entries[1].name = s_ctx.entries[0].name;
    expect_invalid_init();
    s_ctx.entries[1].name = "system.health";
    expect_invalid_init();
    memset(s_ctx.names[1], 'a', sizeof(s_ctx.names[1]));
    s_ctx.entries[1] = original;
    expect_invalid_init();
    memcpy(s_ctx.names[1], "storage.media", sizeof("storage.media"));
    initialize();
}

void test_diagnostics_prefix_distinct_names_are_valid_stable_identities(void) {
    memcpy(s_ctx.names[0], "a", sizeof("a"));
    memcpy(s_ctx.names[1], "ab", sizeof("ab"));
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_read(&s_ctx.diagnostics, "a", 1u, &s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.output.index);
    TEST_ASSERT_EQUAL(ESY_OK, esy_diagnostics_read(&s_ctx.diagnostics, "ab", 2u, &s_ctx.output));
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.output.index);
}

void test_diagnostics_invalid_pointer_ranges_and_inventory_sizes_fail_before_access(void) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_init(&s_ctx.diagnostics, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_init(&s_ctx.diagnostics,
        (const esy_diagnostics_config_t *)(uintptr_t)(UINTPTR_MAX - 1u)));
    s_ctx.config.static_resources = NULL;
    expect_invalid_init();
    s_ctx.config.static_resources = (const esy_diagnostics_static_resources_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    expect_invalid_init();
    s_ctx.config.static_resources = &s_ctx.resources;
    s_ctx.resources.count = SIZE_MAX / sizeof(s_ctx.entries[0]) + 1u;
    expect_invalid_init();
#if SIZE_MAX > UINT32_MAX
    s_ctx.resources.count = (size_t)UINT32_MAX + 1u;
    expect_invalid_init();
#endif
    s_ctx.resources.count = 2u;
    s_ctx.resources.entries = (const esy_diagnostics_entry_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    expect_invalid_init();
    s_ctx.resources.entries = s_ctx.entries;
    s_ctx.entries[1].name = (const char *)(uintptr_t)(UINTPTR_MAX - 1u);
    expect_invalid_init();
    s_ctx.entries[1].name = s_ctx.names[1];
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_enumerate(&s_ctx.diagnostics,
        0u, (esy_diagnostics_snapshot_t *)(uintptr_t)(UINTPTR_MAX - 1u)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_enumerate(&s_ctx.diagnostics, 0u, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_read(&s_ctx.diagnostics,
        "system.health", sizeof("system.health") - 1u, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_deinit(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_diagnostics_deinit((esy_diagnostics_t *)(uintptr_t)(UINTPTR_MAX - 1u)));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_enumerate(NULL, 0u, &s_ctx.output));
    no_callbacks();
}

void test_diagnostics_output_cannot_overwrite_handles_inventory_or_registered_names(void) {
    initialize();
    const esy_diagnostics_t handle_before = s_ctx.diagnostics;
    esy_diagnostics_entry_t entries_before[TEST_SOURCE_COUNT];
    memcpy(entries_before, s_ctx.entries, sizeof(entries_before));
    char names_before[sizeof(s_ctx.names)];
    memcpy(names_before, s_ctx.names, sizeof(names_before));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_enumerate(&s_ctx.diagnostics, 0u,
        (esy_diagnostics_snapshot_t *)(void *)&s_ctx.diagnostics));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_read(&s_ctx.diagnostics,
        "system.health", sizeof("system.health") - 1u,
        (esy_diagnostics_snapshot_t *)(void *)s_ctx.entries));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_run(&s_ctx.diagnostics,
        "storage.media", sizeof("storage.media") - 1u,
        (esy_diagnostics_snapshot_t *)(void *)s_ctx.names[2]));
    TEST_ASSERT_EQUAL_MEMORY(&handle_before, &s_ctx.diagnostics, sizeof(handle_before));
    TEST_ASSERT_EQUAL_MEMORY(entries_before, s_ctx.entries, sizeof(entries_before));
    TEST_ASSERT_EQUAL_MEMORY(names_before, s_ctx.names, sizeof(names_before));
    no_callbacks();
}

void test_diagnostics_output_cannot_overwrite_a_bounded_lookup_name(void) {
    union {
        esy_diagnostics_snapshot_t snapshot;
        char bytes[sizeof(esy_diagnostics_snapshot_t)];
    } shared = {0};
    memcpy(shared.bytes, "storage.media", sizeof("storage.media"));
    char before[sizeof(shared.bytes)];
    memcpy(before, shared.bytes, sizeof(before));
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_diagnostics_read(&s_ctx.diagnostics,
        shared.bytes, sizeof("storage.media") - 1u, &shared.snapshot));
    TEST_ASSERT_EQUAL_MEMORY(before, shared.bytes, sizeof(before));
    no_callbacks();
}

void test_diagnostics_init_rejects_inventory_storage_overlapping_the_output_handle(void) {
    s_ctx.resources.entries = (const esy_diagnostics_entry_t *)(const void *)&s_ctx.diagnostics;
    s_ctx.resources.count = 1u;
    expect_invalid_init();
    s_ctx.resources.entries = s_ctx.entries;
    initialize();
}
