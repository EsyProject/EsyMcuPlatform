/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/remote_manager/esy_remote_manager.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_stats_table.h"

#include <string.h>

#define TEST_METRIC_COUNT 4u

typedef struct {
    esy_remote_manager_stats_table_t table;
    esy_remote_manager_stats_table_config_t config;
    esy_remote_manager_stats_ops_t ops;
    esy_remote_manager_stats_entry_t entries[TEST_METRIC_COUNT];
    esy_remote_manager_stats_sample_t samples[TEST_METRIC_COUNT];
    unsigned authorization_calls;
} remote_manager_stats_table_test_ctx_t;

static remote_manager_stats_table_test_ctx_t s_ctx;

static esy_err_t authorize(void *context,
                           const esy_remote_manager_request_t *request) {
    remote_manager_stats_table_test_ctx_t *ctx = context;
    (void)request;
    ++ctx->authorization_calls;
    return ESY_OK;
}

static void initialize(void) {
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_remote_manager_stats_table_init(&s_ctx.table, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_remote_manager_stats_table_get_ops(&s_ctx.table, &s_ctx.ops));
}

static void assert_invalid_binding(void) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_stats_table_init(&s_ctx.table, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_stats_table_get_ops(&s_ctx.table, &s_ctx.ops));
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.samples[0] = (esy_remote_manager_stats_sample_t){
        .value = UINT64_C(0x1122334455667788), .epoch = 1u, .available = true};
    s_ctx.samples[1] = (esy_remote_manager_stats_sample_t){
        .value = UINT64_MAX, .epoch = 2u, .available = true};
    s_ctx.samples[2] = (esy_remote_manager_stats_sample_t){
        .value = 4096u, .epoch = 3u, .available = true};
    s_ctx.samples[3] = (esy_remote_manager_stats_sample_t){
        .value = 2048u, .epoch = 0u, .available = true};
    s_ctx.entries[0] = (esy_remote_manager_stats_entry_t){
        .name = "system.uptime_ms", .name_length = sizeof("system.uptime_ms") - 1u,
        .kind = ESY_REMOTE_MANAGER_STATS_COUNTER,
        .unit = ESY_REMOTE_MANAGER_STATS_UNIT_MILLISECONDS, .sample = &s_ctx.samples[0]};
    s_ctx.entries[1] = (esy_remote_manager_stats_entry_t){
        .name = "comms.rx_bytes", .name_length = sizeof("comms.rx_bytes") - 1u,
        .kind = ESY_REMOTE_MANAGER_STATS_COUNTER,
        .unit = ESY_REMOTE_MANAGER_STATS_UNIT_BYTES, .sample = &s_ctx.samples[1]};
    s_ctx.entries[2] = (esy_remote_manager_stats_entry_t){
        .name = "ota.received_bytes", .name_length = sizeof("ota.received_bytes") - 1u,
        .kind = ESY_REMOTE_MANAGER_STATS_COUNTER,
        .unit = ESY_REMOTE_MANAGER_STATS_UNIT_BYTES, .sample = &s_ctx.samples[2]};
    s_ctx.entries[3] = (esy_remote_manager_stats_entry_t){
        .name = "resources.stack_words", .name_length = sizeof("resources.stack_words") - 1u,
        .kind = ESY_REMOTE_MANAGER_STATS_GAUGE,
        .unit = ESY_REMOTE_MANAGER_STATS_UNIT_WORDS, .sample = &s_ctx.samples[3]};
    s_ctx.config = (esy_remote_manager_stats_table_config_t){
        .entries = s_ctx.entries, .count = TEST_METRIC_COUNT};
}

void tearDown(void) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_stats_table_deinit(&s_ctx.table));
}

void test_stats_table_enumerates_owner_selected_system_comms_ota_and_resource_names(void) {
    initialize();
    TEST_ASSERT_EQUAL_PTR(&s_ctx.table, s_ctx.ops.context);
    TEST_ASSERT_NOT_NULL(s_ctx.ops.enumerate);
    TEST_ASSERT_NOT_NULL(s_ctx.ops.read);
    for (uint32_t index = 0u; index < TEST_METRIC_COUNT; ++index) {
        esy_remote_manager_stats_snapshot_t snapshot;
        TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.enumerate(s_ctx.ops.context, index, &snapshot));
        TEST_ASSERT_EQUAL_UINT32(TEST_METRIC_COUNT, snapshot.count);
        TEST_ASSERT_EQUAL_UINT32(index, snapshot.index);
        TEST_ASSERT_EQUAL_UINT(s_ctx.entries[index].name_length, snapshot.name_length);
        TEST_ASSERT_EQUAL_MEMORY(s_ctx.entries[index].name, snapshot.name, snapshot.name_length);
        TEST_ASSERT_EQUAL_UINT8(0u, snapshot.name[snapshot.name_length]);
        TEST_ASSERT_EQUAL(s_ctx.entries[index].kind, snapshot.kind);
        TEST_ASSERT_EQUAL(s_ctx.entries[index].unit, snapshot.unit);
        TEST_ASSERT_EQUAL_UINT64(s_ctx.samples[index].value, snapshot.sample.value);
        TEST_ASSERT_EQUAL_UINT64(s_ctx.samples[index].epoch, snapshot.sample.epoch);
        TEST_ASSERT_TRUE(snapshot.sample.available);
    }
    esy_remote_manager_stats_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.enumerate(s_ctx.ops.context, TEST_METRIC_COUNT, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.enumerate(s_ctx.ops.context, UINT32_MAX, &snapshot));
}

void test_stats_table_copies_live_ram_samples_without_resetting_or_retaining_outputs(void) {
    initialize();
    esy_remote_manager_stats_snapshot_t before;
    esy_remote_manager_stats_snapshot_t after;
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.enumerate(s_ctx.ops.context, 1u, &before));
    s_ctx.samples[1] = (esy_remote_manager_stats_sample_t){
        .value = 7u, .epoch = UINT64_C(0xaabbccddeeff0011), .available = true};
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.read(s_ctx.ops.context,
        "comms.rx_bytes", sizeof("comms.rx_bytes") - 1u, &after));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, before.sample.value);
    TEST_ASSERT_EQUAL_UINT64(2u, before.sample.epoch);
    TEST_ASSERT_EQUAL_UINT64(7u, after.sample.value);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0xaabbccddeeff0011), after.sample.epoch);
    TEST_ASSERT_EQUAL_UINT64(7u, s_ctx.samples[1].value);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0xaabbccddeeff0011), s_ctx.samples[1].epoch);
    TEST_ASSERT_EQUAL_UINT64(4096u, s_ctx.samples[2].value);
}

void test_stats_table_masks_unavailable_values_without_modifying_the_owner_sample(void) {
    s_ctx.samples[0].available = false;
    initialize();
    esy_remote_manager_stats_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    TEST_ASSERT_FALSE(snapshot.sample.available);
    TEST_ASSERT_EQUAL_UINT64(0u, snapshot.sample.value);
    TEST_ASSERT_EQUAL_UINT64(1u, snapshot.sample.epoch);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x1122334455667788), s_ctx.samples[0].value);
    s_ctx.samples[0].value = 0u;
    s_ctx.samples[0].available = true;
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    TEST_ASSERT_TRUE(snapshot.sample.available);
    TEST_ASSERT_EQUAL_UINT64(0u, snapshot.sample.value);
}

void test_stats_table_exact_name_search_distinguishes_unknown_prefixes_and_case(void) {
    initialize();
    esy_remote_manager_stats_snapshot_t snapshot;
    const char bounded_name[] = {'c', 'o', 'm', 'm', 's', '.', 'r', 'x', '_',
                                 'b', 'y', 't', 'e', 's'};
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.read(s_ctx.ops.context,
        bounded_name, sizeof(bounded_name), &snapshot));
    TEST_ASSERT_EQUAL_UINT32(1u, snapshot.index);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, s_ctx.ops.read(s_ctx.ops.context,
        "comms.rx", sizeof("comms.rx") - 1u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, s_ctx.ops.read(s_ctx.ops.context,
        "Comms.rx_bytes", sizeof("Comms.rx_bytes") - 1u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, s_ctx.ops.read(s_ctx.ops.context,
        "comms.rx_bytes_extra", sizeof("comms.rx_bytes_extra") - 1u, &snapshot));
}

void test_stats_table_accepts_an_empty_catalogue_without_any_borrowed_array(void) {
    s_ctx.config = (esy_remote_manager_stats_table_config_t){0};
    initialize();
    esy_remote_manager_stats_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(0u, snapshot.count);
    TEST_ASSERT_EQUAL_UINT32(0u, snapshot.index);
    TEST_ASSERT_EQUAL_UINT(0u, snapshot.name_length);
    TEST_ASSERT_EQUAL_UINT8(0u, snapshot.name[0]);
    TEST_ASSERT_EQUAL_UINT64(0u, snapshot.sample.value);
    TEST_ASSERT_EQUAL_UINT64(0u, snapshot.sample.epoch);
    TEST_ASSERT_FALSE(snapshot.sample.available);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.enumerate(s_ctx.ops.context, 1u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, s_ctx.ops.read(s_ctx.ops.context,
        "system.uptime_ms", sizeof("system.uptime_ms") - 1u, &snapshot));
}

void test_stats_table_rejects_missing_storage_and_duplicate_metric_names(void) {
    s_ctx.config.entries = NULL;
    assert_invalid_binding();
    s_ctx.config.entries = s_ctx.entries;
    s_ctx.entries[1].name = s_ctx.entries[0].name;
    s_ctx.entries[1].name_length = s_ctx.entries[0].name_length;
    assert_invalid_binding();
    s_ctx.entries[1].name = "system.uptime_ms";
    assert_invalid_binding();
}

void test_stats_table_rejects_invalid_descriptor_names_kinds_units_and_sample_pointers(void) {
    s_ctx.config.count = 1u;
    const esy_remote_manager_stats_entry_t original = s_ctx.entries[0];
    s_ctx.entries[0].name = NULL;
    assert_invalid_binding();
    s_ctx.entries[0] = original;
    s_ctx.entries[0].name_length = 0u;
    assert_invalid_binding();
    s_ctx.entries[0] = original;
    s_ctx.entries[0].name_length = ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES + 1u;
    assert_invalid_binding();
    s_ctx.entries[0] = original;
    s_ctx.entries[0].name = "space here";
    s_ctx.entries[0].name_length = sizeof("space here") - 1u;
    assert_invalid_binding();
    s_ctx.entries[0] = original;
    s_ctx.entries[0].name = "embedded\0nul";
    s_ctx.entries[0].name_length = sizeof("embedded\0nul") - 1u;
    assert_invalid_binding();
    s_ctx.entries[0] = original;
    s_ctx.entries[0].kind = (esy_remote_manager_stats_kind_t)0;
    assert_invalid_binding();
    s_ctx.entries[0] = original;
    s_ctx.entries[0].unit = (esy_remote_manager_stats_unit_t)6;
    assert_invalid_binding();
    s_ctx.entries[0] = original;
    s_ctx.entries[0].sample = NULL;
    assert_invalid_binding();
    s_ctx.entries[0] = original;
    initialize();
}

void test_stats_table_lifecycle_preserves_live_bindings_and_detaches_without_resetting_samples(void) {
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_stats_table_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_stats_table_init(&s_ctx.table, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_stats_table_get_ops(&s_ctx.table, &s_ctx.ops));
    initialize();
    const esy_remote_manager_stats_table_config_t empty = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_stats_table_init(&s_ctx.table, &empty));
    esy_remote_manager_stats_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(TEST_METRIC_COUNT, snapshot.count);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_stats_table_get_ops(NULL, &s_ctx.ops));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        esy_remote_manager_stats_table_get_ops(&s_ctx.table, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_remote_manager_stats_table_deinit(NULL));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_stats_table_deinit(&s_ctx.table));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, s_ctx.samples[1].value);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        esy_remote_manager_stats_table_get_ops(&s_ctx.table, &s_ctx.ops));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        s_ctx.ops.read(s_ctx.ops.context, "comms.rx_bytes", sizeof("comms.rx_bytes") - 1u,
                       &snapshot));
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_stats_table_deinit(&s_ctx.table));
    s_ctx.config = empty;
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(0u, snapshot.count);
}

void test_stats_table_callbacks_validate_output_and_name_spans_without_sampling(void) {
    initialize();
    esy_remote_manager_stats_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.enumerate(s_ctx.ops.context, 0u, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.read(s_ctx.ops.context, "comms.rx_bytes", sizeof("comms.rx_bytes") - 1u, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.read(s_ctx.ops.context, NULL, 1u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.read(s_ctx.ops.context, "", 0u, &snapshot));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,
        s_ctx.ops.read(s_ctx.ops.context, "invalid name", sizeof("invalid name") - 1u, &snapshot));
}

void test_stats_table_percent_samples_enforce_bounds_at_read_time(void) {
    s_ctx.config.count = 1u;
    s_ctx.entries[0].kind = ESY_REMOTE_MANAGER_STATS_GAUGE;
    s_ctx.entries[0].unit = ESY_REMOTE_MANAGER_STATS_UNIT_PERCENT;
    s_ctx.samples[0].value = 101u;
    initialize();
    esy_remote_manager_stats_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE,
        s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    s_ctx.samples[0].value = 100u;
    TEST_ASSERT_EQUAL(ESY_OK,
        s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    TEST_ASSERT_EQUAL_UINT64(100u, snapshot.sample.value);
    s_ctx.samples[0].available = false;
    s_ctx.samples[0].value = UINT64_MAX;
    TEST_ASSERT_EQUAL(ESY_OK,
        s_ctx.ops.enumerate(s_ctx.ops.context, 0u, &snapshot));
    TEST_ASSERT_FALSE(snapshot.sample.available);
    TEST_ASSERT_EQUAL_UINT64(0u, snapshot.sample.value);
}

void test_stats_table_accepts_bounded_nonterminated_names_and_prefix_distinct_metrics(void) {
    const char short_name[] = {'a', '/', '_', '-', '.', '0', 'Z'};
    const char longer_name[] = {'a', '/', '_', '-', '.', '0', 'Z', '1'};
    s_ctx.config.count = 2u;
    s_ctx.entries[0].name = short_name;
    s_ctx.entries[0].name_length = sizeof(short_name);
    s_ctx.entries[1].name = longer_name;
    s_ctx.entries[1].name_length = sizeof(longer_name);
    initialize();
    esy_remote_manager_stats_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.read(s_ctx.ops.context,
        short_name, sizeof(short_name), &snapshot));
    TEST_ASSERT_EQUAL_UINT32(0u, snapshot.index);
    TEST_ASSERT_EQUAL_MEMORY(short_name, snapshot.name, sizeof(short_name));
    TEST_ASSERT_EQUAL_UINT8(0u, snapshot.name[sizeof(short_name)]);
    TEST_ASSERT_EQUAL(ESY_OK, s_ctx.ops.read(s_ctx.ops.context,
        longer_name, sizeof(longer_name), &snapshot));
    TEST_ASSERT_EQUAL_UINT32(1u, snapshot.index);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_stats_table_deinit(&s_ctx.table));
}

void test_stats_table_supplies_the_real_manager_without_hal_or_opaque_output_storage(void) {
    initialize();
    esy_remote_manager_t manager = {0};
    const esy_remote_manager_config_t config = {
        .max_key_length = ESY_REMOTE_MANAGER_STATS_NAME_MAX_BYTES,
        .max_request_value_length = ESY_REMOTE_MANAGER_STATS_INDEX_BYTES,
        .max_response_value_length = 0u,
        .authorize = authorize, .authorize_context = &s_ctx, .stats_ops = s_ctx.ops};
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_init(&manager, &config));
    const esy_remote_manager_request_t request = {
        .group = ESY_REMOTE_MANAGER_GROUP_STATS, .command = ESY_REMOTE_MANAGER_STATS_READ,
        .key = "ota.received_bytes", .key_length = sizeof("ota.received_bytes") - 1u};
    esy_remote_manager_response_t response;
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_remote_manager_dispatch(&manager, &request, NULL, 0u, &response));
    TEST_ASSERT_EQUAL_UINT32(2u, response.stats.index);
    TEST_ASSERT_EQUAL_UINT64(4096u, response.stats.sample.value);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.authorization_calls);
    TEST_ASSERT_EQUAL_UINT64(4096u, s_ctx.samples[2].value);
    TEST_ASSERT_EQUAL(ESY_OK, esy_remote_manager_deinit(&manager));
}

void test_stats_table_rejects_catalogues_that_cannot_be_indexed_by_the_public_contract(void) {
    s_ctx.config.count = SIZE_MAX / sizeof(s_ctx.entries[0]) + 1u;
    assert_invalid_binding();
#if SIZE_MAX > UINT32_MAX
    s_ctx.config.count = (size_t)UINT32_MAX + 1u;
    assert_invalid_binding();
#endif
}
