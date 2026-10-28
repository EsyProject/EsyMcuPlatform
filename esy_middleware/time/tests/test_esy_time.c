/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/time/esy_time.h"

#include <string.h>

typedef struct {
    esy_time_t time;
    uint64_t now_us;
    esy_err_t clock_error;
    unsigned clock_calls;
} fixture_ctx_t;

static fixture_ctx_t s_ctx;

static esy_err_t read_clock(void *context, uint64_t *value)
{
    fixture_ctx_t *ctx = context;
    ctx->clock_calls++;
    if (ctx->clock_error == ESY_OK) {
        *value = ctx->now_us;
    }
    return ctx->clock_error;
}

void setUp(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
}

void tearDown(void)
{
}

static void initialize(void)
{
    const esy_time_config_t config = {read_clock, &s_ctx};
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_init(&s_ctx.time, &config));
}

void test_time_lifecycle_requires_zero_handle_and_explicit_utc(void)
{
    const esy_time_config_t config = {read_clock, &s_ctx};
    esy_time_status_t status = {true};
    esy_timestamp_t output = {42, 1};
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_time_now(&s_ctx.time, &output));
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_time_init(&s_ctx.time, &config));
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_get_status(&s_ctx.time, &status));
    TEST_ASSERT_FALSE(status.valid);
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_time_now(&s_ctx.time, &output));
    TEST_ASSERT_EQUAL_INT64(42, output.seconds);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_deinit(&s_ctx.time));
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_deinit(&s_ctx.time));
    initialize();
}

void test_time_adds_elapsed_microseconds_and_normalizes_nanoseconds(void)
{
    const esy_timestamp_t anchor = {1700000000, 999999500};
    esy_timestamp_t output;
    initialize();
    s_ctx.now_us = 17;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &anchor));
    s_ctx.now_us += 1000001;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &output));
    TEST_ASSERT_EQUAL_INT64(1700000002, output.seconds);
    TEST_ASSERT_EQUAL_UINT32(500, output.nanoseconds);
}

void test_time_elapsed_crosses_uint64_wrap_and_invalidation_stays_invalid(void)
{
    const esy_timestamp_t anchor = {0, 0};
    esy_timestamp_t output;
    initialize();
    s_ctx.now_us = UINT64_MAX - 9u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &anchor));
    s_ctx.now_us = 10;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &output));
    TEST_ASSERT_EQUAL_UINT32(20000, output.nanoseconds);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_invalidate(&s_ctx.time));
    s_ctx.now_us = UINT64_MAX - 9u;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_time_now(&s_ctx.time, &output));
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &anchor));
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &output));
}

void test_time_rejects_reset_clock_and_range_exhaustion_without_output_changes(void)
{
    const esy_timestamp_t anchor = {ESY_TIME_MAX_SECONDS, 999999999};
    esy_timestamp_t output = {12, 34};
    initialize();
    s_ctx.now_us = 100;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &anchor));
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &output));
    output.seconds = 12;
    s_ctx.now_us = 101;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_time_now(&s_ctx.time, &output));
    TEST_ASSERT_EQUAL_INT64(12, output.seconds);
    s_ctx.now_us = 99;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_time_now(&s_ctx.time, &output));
}

void test_time_rejects_invalid_arguments_and_copied_handle(void)
{
    esy_time_config_t invalid = {0};
    esy_timestamp_t value = {-1, 0};
    esy_timestamp_t output;
    esy_time_status_t status;
    esy_time_t copy;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_init(NULL, &invalid));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_init(&s_ctx.time, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_init(&s_ctx.time, &invalid));
    initialize();
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_set(&s_ctx.time, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_set(&s_ctx.time, &value));
    value.seconds = ESY_TIME_MAX_SECONDS + 1;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_set(&s_ctx.time, &value));
    value.seconds = 0;
    value.nanoseconds = ESY_TIME_NANOSECONDS_PER_SECOND;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_set(&s_ctx.time, &value));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_now(&s_ctx.time, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_get_status(&s_ctx.time, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_get_status(NULL, &status));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_invalidate(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_time_deinit(NULL));
    copy = s_ctx.time;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_time_now(&copy, &output));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_time_deinit(&copy));
}

void test_time_provider_errors_preserve_anchor_and_output(void)
{
    const esy_timestamp_t first = {100, 0};
    const esy_timestamp_t second = {200, 0};
    esy_timestamp_t output = {77, 33};
    initialize();
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &first));
    s_ctx.clock_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_time_set(&s_ctx.time, &second));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_time_now(&s_ctx.time, &output));
    TEST_ASSERT_EQUAL_INT64(77, output.seconds);
    TEST_ASSERT_EQUAL_UINT32(33, output.nanoseconds);
    s_ctx.clock_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &output));
    TEST_ASSERT_EQUAL_INT64(100, output.seconds);
}
