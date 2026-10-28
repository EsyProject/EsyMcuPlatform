/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <string.h>
#include "esy_middleware/ota/esy_ota.h"

#define TEST_CHUNK_BYTES 4u
#define TEST_IMAGE_BYTES 8u
#define TEST_TIMEOUT_MS 10u

typedef struct {
    esy_ota_t ota;
    esy_ota_config_t config;
    esy_ota_static_resources_t resources;
    esy_activity_guard_t guard;
    uint8_t last[TEST_CHUNK_BYTES];
    uint8_t image[TEST_IMAGE_BYTES];
    uint8_t stored[TEST_IMAGE_BYTES];
    esy_image_manifest_t manifest;
    esy_image_status_t status;
    uint32_t now;
    uint32_t next_id;
    uint32_t forced_id;
    uint32_t begin_delay, write_delay, finish_delay, activate_delay;
    unsigned begin_calls, write_calls, finish_calls, activate_calls, abort_calls;
    unsigned guard_acquires, guard_releases, clock_calls, id_calls;
    esy_err_t begin_result, write_result, finish_result, activate_result, abort_result;
    esy_err_t acquire_result, release_result, id_result;
    bool force_id, reenter;
    esy_err_t reentrant_status, reentrant_init, reentrant_deinit, reentrant_poll;
} upload_test_ctx_t;

static upload_test_ctx_t s_ctx;

static esy_err_t provider_begin(void *context, const esy_image_manifest_t *manifest) {
    upload_test_ctx_t *ctx = context;
    ++ctx->begin_calls;
    ctx->now += ctx->begin_delay;
    TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_BYTES, manifest->image_size);
    if (ctx->reenter) {
        esy_image_status_t status;
        ctx->reentrant_status = esy_ota_status(&ctx->ota, &status);
        ctx->reentrant_init = esy_ota_init(&ctx->ota, &ctx->config);
        ctx->reentrant_deinit = esy_ota_deinit(&ctx->ota);
        ctx->reentrant_poll = esy_ota_poll(&ctx->ota);
    }
    return ctx->begin_result;
}
static esy_err_t provider_write(void *context, uint32_t offset,
                                const uint8_t *data, size_t length) {
    upload_test_ctx_t *ctx = context;
    ++ctx->write_calls;
    ctx->now += ctx->write_delay;
    memcpy(ctx->stored + offset, data, ctx->write_result == ESY_OK ? length : 1u);
    return ctx->write_result;
}
static esy_err_t provider_finish(void *context) {
    upload_test_ctx_t *ctx = context;
    ++ctx->finish_calls;
    ctx->now += ctx->finish_delay;
    return ctx->finish_result;
}
static esy_err_t provider_activate(void *context) {
    upload_test_ctx_t *ctx = context;
    ++ctx->activate_calls;
    ctx->now += ctx->activate_delay;
    return ctx->activate_result;
}
static esy_err_t provider_abort(void *context) {
    upload_test_ctx_t *ctx = context;
    ++ctx->abort_calls;
    return ctx->abort_result;
}
static esy_err_t acquire_guard(void *context) {
    upload_test_ctx_t *ctx = context;
    ++ctx->guard_acquires;
    return ctx->acquire_result;
}
static esy_err_t release_guard(void *context) {
    upload_test_ctx_t *ctx = context;
    ++ctx->guard_releases;
    return ctx->release_result;
}
static uint32_t clock_ms(void *context) {
    upload_test_ctx_t *ctx = context;
    ++ctx->clock_calls;
    return ctx->now;
}
static esy_err_t new_id(void *context, uint32_t *id) {
    upload_test_ctx_t *ctx = context;
    ++ctx->id_calls;
    *id = ctx->force_id ? ctx->forced_id : ++ctx->next_id;
    return ctx->id_result;
}
static uint32_t begin_upload(void) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    return s_ctx.status.session_id;
}
static void write_image(uint32_t id) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image,
                                              TEST_CHUNK_BYTES, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_write(&s_ctx.ota, id, TEST_CHUNK_BYTES,
        s_ctx.image + TEST_CHUNK_BYTES, TEST_CHUNK_BYTES, &s_ctx.status));
}
static uint32_t ready_image(void) {
    const uint32_t id = begin_upload();
    write_image(id);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    return id;
}

void setUp(void) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    for (size_t i = 0u; i < sizeof(s_ctx.image); ++i) s_ctx.image[i] = (uint8_t)i;
    s_ctx.manifest.version = 3u;
    s_ctx.manifest.image_size = TEST_IMAGE_BYTES;
    s_ctx.resources = (esy_ota_static_resources_t){s_ctx.last, sizeof(s_ctx.last)};
    s_ctx.guard = (esy_activity_guard_t){&s_ctx, acquire_guard, release_guard};
    s_ctx.config = (esy_ota_config_t){
        .provider = {provider_begin, provider_write, provider_finish,
                      provider_activate, provider_abort, &s_ctx},
        .max_image_bytes = TEST_IMAGE_BYTES, .max_chunk_bytes = TEST_CHUNK_BYTES,
        .static_resources = &s_ctx.resources, .clock_ms = clock_ms,
        .clock_context = &s_ctx, .new_session_id = new_id,
        .session_id_context = &s_ctx, .progress_timeout_ms = TEST_TIMEOUT_MS,
        .activity_guard = &s_ctx.guard};
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_init(&s_ctx.ota, &s_ctx.config));
}
void tearDown(void) {
    s_ctx.abort_result = ESY_OK;
    s_ctx.release_result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_deinit(&s_ctx.ota));
}

void test_begin_write_finish_activate_have_distinct_cached_results(void) {
    const uint32_t id = begin_upload();
    TEST_ASSERT_EQUAL(ESY_IMAGE_RECEIVING, s_ctx.status.phase);
    TEST_ASSERT_FALSE(s_ctx.status.result_valid);
    write_image(id);
    TEST_ASSERT_EQUAL_MEMORY(s_ctx.image, s_ctx.stored, sizeof(s_ctx.image));
    TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_BYTES, s_ctx.status.next_offset);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_READY, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.activate_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.guard_releases);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.finish_calls);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_ACTIVATED, s_ctx.status.phase);
    TEST_ASSERT_TRUE(s_ctx.status.result_valid);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.activate_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.guard_releases);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_abort(&s_ctx.ota, id, &s_ctx.status));
}

void test_repeat_begin_compares_manifest_fields_without_reclaim_or_deadline_refresh(void) {
    s_ctx.now = 100u;
    const uint32_t id = begin_upload();
    s_ctx.now = 109u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT32(id, s_ctx.status.session_id);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.begin_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.id_calls);
    esy_image_manifest_t other = s_ctx.manifest;
    other.signature[0] = 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ota_begin(&s_ctx.ota, &other, &s_ctx.status));
    other = s_ctx.manifest;
    other.image_sha256[0] = 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ota_begin(&s_ctx.ota, &other, &s_ctx.status));
    other = s_ctx.manifest;
    other.version++;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ota_begin(&s_ctx.ota, &other, &s_ctx.status));
    s_ctx.now = 110u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT, esy_ota_poll(&s_ctx.ota));
}

void test_exact_last_block_repeat_is_checked_but_not_written_or_kept_alive(void) {
    const uint32_t id = begin_upload();
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 4u, &s_ctx.status));
    s_ctx.now = 9u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 4u, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.write_calls);
    uint8_t changed[TEST_CHUNK_BYTES] = {0xffu};
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_write(&s_ctx.ota, id, 0u, changed, 4u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 3u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_write(&s_ctx.ota, id, 5u, s_ctx.image, 3u, &s_ctx.status));
    s_ctx.now = 10u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 4u, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_FAILED, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.write_calls);
}

void test_only_previous_block_is_repeatable_and_old_tokens_do_not_affect_upload(void) {
    const uint32_t id = begin_upload();
    write_image(id);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 4u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_finish(&s_ctx.ota, id + 1u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_activate(&s_ctx.ota, id + 1u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_abort(&s_ctx.ota, id + 1u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_write(&s_ctx.ota, id, 4u, s_ctx.image + 4u, 4u, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(2u, s_ctx.write_calls);
}

void test_incomplete_finish_and_preverification_activation_do_not_touch_provider(void) {
    const uint32_t id = begin_upload();
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.finish_calls);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.activate_calls);
    TEST_ASSERT_EQUAL(ESY_IMAGE_RECEIVING, s_ctx.status.phase);
}

void test_partial_provider_write_failure_ends_session_and_is_not_replayed(void) {
    const uint32_t id = begin_upload();
    s_ctx.write_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 4u, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_FAILED, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.status.next_offset);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.abort_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.guard_releases);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 4u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.write_calls);
}

void test_begin_failure_and_guard_failure_do_not_leave_an_active_deadline(void) {
    s_ctx.begin_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.abort_calls);
    TEST_ASSERT_EQUAL(ESY_IMAGE_FAILED, s_ctx.status.phase);
    s_ctx.begin_result = ESY_OK;
    s_ctx.acquire_result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.begin_calls);
    s_ctx.now = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
}

void test_failed_cleanup_retains_borrows_and_blocks_reinit_until_deinit_retry(void) {
    const uint32_t id = begin_upload();
    s_ctx.abort_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_abort(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.guard_releases);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_init(&s_ctx.ota, &s_ctx.config));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_deinit(&s_ctx.ota));
    s_ctx.abort_result = ESY_OK;
    s_ctx.release_result = ESY_ERR_BUSY;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ota_deinit(&s_ctx.ota));
    TEST_ASSERT_EQUAL_UINT(3u, s_ctx.abort_calls);
    s_ctx.release_result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_deinit(&s_ctx.ota));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_deinit(&s_ctx.ota));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_init(&s_ctx.ota, &s_ctx.config));
}

void test_finish_failure_is_cached_and_activation_failure_never_claims_success(void) {
    uint32_t id = begin_upload();
    write_image(id);
    s_ctx.finish_result = ESY_ERR_HASH_FAIL;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_HASH_FAIL, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_HASH_FAIL, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.finish_calls);
    s_ctx.finish_result = ESY_OK;
    id = ready_image();
    s_ctx.activate_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_FAILED, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.activate_calls);
}

void test_activated_guard_release_failure_retains_activation_and_never_republishes(void) {
    const uint32_t id = ready_image();
    s_ctx.release_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_ACTIVATED, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.activate_calls);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_abort(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_deinit(&s_ctx.ota));
    s_ctx.release_result = ESY_OK;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_deinit(&s_ctx.ota));
}

void test_ready_phase_has_its_own_deadline_and_repeats_do_not_extend_it(void) {
    const uint32_t id = begin_upload();
    write_image(id);
    s_ctx.now = 9u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    s_ctx.now = 18u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_status(&s_ctx.ota, &s_ctx.status));
    s_ctx.now = 19u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.activate_calls);
}

void test_timeout_crosses_wrap_and_terminal_wait_does_not_reactivate_a_full_cycle_later(void) {
    s_ctx.now = UINT32_MAX - 5u;
    begin_upload();
    s_ctx.now = 3u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    s_ctx.now = 4u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT, esy_ota_poll(&s_ctx.ota));
    const unsigned clock_calls = s_ctx.clock_calls;
    s_ctx.now = 3u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    s_ctx.now = 4u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    TEST_ASSERT_EQUAL_UINT(clock_calls, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.abort_calls);
}

void test_abort_and_activation_disable_deadlines_until_explicit_new_begin(void) {
    uint32_t id = begin_upload();
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_abort(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_abort(&s_ctx.ota, id, &s_ctx.status));
    unsigned samples = s_ctx.clock_calls;
    s_ctx.now = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    s_ctx.now = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    TEST_ASSERT_EQUAL_UINT(samples, s_ctx.clock_calls);
    id = ready_image();
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    samples = s_ctx.clock_calls;
    s_ctx.now = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    s_ctx.now = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    TEST_ASSERT_EQUAL_UINT(samples, s_ctx.clock_calls);
}

void test_timeout_can_be_disabled_and_maximum_wait_is_explicitly_bounded(void) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_deinit(&s_ctx.ota));
    s_ctx.config.progress_timeout_ms = 0u;
    s_ctx.config.clock_ms = NULL;
    s_ctx.config.activity_guard = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_init(&s_ctx.ota, &s_ctx.config));
    begin_upload();
    s_ctx.now = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_deinit(&s_ctx.ota));
    s_ctx.config.progress_timeout_ms = INT32_MAX;
    s_ctx.config.clock_ms = clock_ms;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_init(&s_ctx.ota, &s_ctx.config));
    s_ctx.now = UINT32_MAX - 1u;
    begin_upload();
    s_ctx.now = (uint32_t)((UINT32_MAX - 1u) + (uint32_t)INT32_MAX - 1u);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    ++s_ctx.now;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT, esy_ota_poll(&s_ctx.ota));
}

void test_provider_begin_time_is_observed_before_any_deadline_renewal(void) {
    s_ctx.begin_delay = TEST_TIMEOUT_MS;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT,
        esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_FAILED, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.abort_calls);
}

void test_provider_write_and_finish_latency_cannot_erase_the_old_deadline(void) {
    uint32_t id = begin_upload();
    s_ctx.write_delay = TEST_TIMEOUT_MS;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT,
        esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 4u, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_FAILED, s_ctx.status.phase);
    s_ctx.write_delay = 0u;
    id = begin_upload();
    write_image(id);
    s_ctx.finish_delay = TEST_TIMEOUT_MS;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_TIMEOUT, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_FAILED, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.activate_calls);
}

void test_successful_persistent_activation_is_reported_even_when_completion_is_late(void) {
    const uint32_t id = ready_image();
    s_ctx.activate_delay = TEST_TIMEOUT_MS + 1u;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_ACTIVATED, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.abort_calls);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_poll(&s_ctx.ota));
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.activate_calls);
}

void test_id_source_must_succeed_and_supply_fresh_nonzero_identity(void) {
    s_ctx.id_result = ESY_ERR_IO;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    s_ctx.id_result = ESY_OK;
    s_ctx.force_id = true;
    s_ctx.forced_id = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    s_ctx.forced_id = 42u;
    const uint32_t id = begin_upload();
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_abort(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_begin(&s_ctx.ota, &s_ctx.manifest, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(1u, s_ctx.begin_calls);
}

void test_callbacks_cannot_reenter_lifecycle_status_or_poll(void) {
    s_ctx.reenter = true;
    begin_upload();
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.reentrant_status);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.reentrant_init);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.reentrant_deinit);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, s_ctx.reentrant_poll);
}

void test_invalid_input_does_not_consume_or_terminate_a_valid_session(void) {
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_begin(&s_ctx.ota, NULL, &s_ctx.status));
    esy_image_manifest_t bad = s_ctx.manifest;
    bad.version = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_begin(&s_ctx.ota, &bad, &s_ctx.status));
    bad = s_ctx.manifest; bad.image_size = 0u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_begin(&s_ctx.ota, &bad, &s_ctx.status));
    bad.image_size = TEST_IMAGE_BYTES + 1u;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_begin(&s_ctx.ota, &bad, &s_ctx.status));
    const uint32_t id = begin_upload();
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_write(&s_ctx.ota, 0u, 0u, s_ctx.image, 1u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_write(&s_ctx.ota, id, 0u, NULL, 1u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 0u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.image, 5u, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_write(&s_ctx.ota, id, 0u, s_ctx.last, 1u, &s_ctx.status));
    TEST_ASSERT_EQUAL_UINT(0u, s_ctx.write_calls);
    TEST_ASSERT_EQUAL(ESY_IMAGE_RECEIVING, s_ctx.status.phase);
}

void test_missing_provider_callbacks_and_invalid_memory_or_timeout_are_rejected(void) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_deinit(&s_ctx.ota));
    for (unsigned fault = 0u; fault < 11u; ++fault) {
        esy_ota_config_t bad = s_ctx.config;
        esy_ota_static_resources_t memory = s_ctx.resources;
        bad.static_resources = &memory;
        if (fault == 0u) bad.provider.begin = NULL;
        if (fault == 1u) bad.provider.write = NULL;
        if (fault == 2u) bad.provider.finish = NULL;
        if (fault == 3u) bad.provider.abort = NULL;
        if (fault == 4u) bad.new_session_id = NULL;
        if (fault == 5u) bad.max_image_bytes = 0u;
        if (fault == 6u) bad.max_chunk_bytes = 0u;
        if (fault == 7u) memory.last_chunk_capacity = 0u;
        if (fault == 8u) memory.last_chunk_buffer = NULL;
        if (fault == 9u) bad.clock_ms = NULL;
        if (fault == 10u) bad.progress_timeout_ms = (uint32_t)INT32_MAX + 1u;
        TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_init(&s_ctx.ota, &bad));
    }
    s_ctx.resources.last_chunk_buffer = (uint8_t *)&s_ctx.ota;
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_init(&s_ctx.ota, &s_ctx.config));
    s_ctx.resources.last_chunk_buffer = s_ctx.last;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_init(&s_ctx.ota, &s_ctx.config));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_init(&s_ctx.ota, &s_ctx.config));
}

void test_optional_activation_and_invalid_lifecycle_remain_explicit(void) {
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_deinit(&s_ctx.ota));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_status(&s_ctx.ota, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_poll(&s_ctx.ota));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_init(NULL, &s_ctx.config));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_init(&s_ctx.ota, NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_deinit(NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_poll(NULL));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_status(NULL, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ota_status(&s_ctx.ota, NULL));
    s_ctx.config.provider.activate = NULL;
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_init(&s_ctx.ota, &s_ctx.config));
    const uint32_t id = ready_image();
    TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, esy_ota_activate(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL(ESY_IMAGE_READY, s_ctx.status.phase);
    TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ota_abort(&s_ctx.ota, id, &s_ctx.status));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ota_finish(&s_ctx.ota, id, &s_ctx.status));
}
