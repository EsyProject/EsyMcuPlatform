/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/time/esy_sntp.h"
#include "esy_middleware/time/esy_time.h"

#include <string.h>

typedef struct {
    esy_time_t time;
    esy_sntp_t client;
    uint64_t now_us;
    esy_err_t clock_error;
    esy_err_t send_error;
    esy_err_t receive_error;
    uint8_t sent[ESY_SNTP_PACKET_BYTES];
    uint8_t response[ESY_SNTP_PACKET_BYTES];
    size_t response_length;
    unsigned send_calls;
    unsigned receive_calls;
    unsigned clock_calls;
    unsigned fail_clock_call;
    unsigned advance_clock_call;
    uint64_t clock_elapsed_us;
    uint64_t receive_elapsed_us;
} fixture_ctx_t;

static fixture_ctx_t s_ctx;

static esy_err_t read_clock(void *context, uint64_t *value)
{
    fixture_ctx_t *ctx = context;
    ctx->clock_calls++;
    if (ctx->clock_error != ESY_OK) {
        return ctx->clock_error;
    }
    if (ctx->fail_clock_call == ctx->clock_calls) {
        return ESY_ERR_IO;
    }
    if (ctx->advance_clock_call == ctx->clock_calls) {
        ctx->now_us += ctx->clock_elapsed_us;
    }
    *value = ctx->now_us;
    return ESY_OK;
}

static esy_err_t send_packet(void *context, const uint8_t *packet, size_t length)
{
    fixture_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_UINT(ESY_SNTP_PACKET_BYTES, length);
    memcpy(ctx->sent, packet, sizeof(ctx->sent));
    ctx->send_calls++;
    return ctx->send_error;
}

static esy_err_t receive_packet(void *context, uint8_t *packet,
                                 size_t capacity, size_t *length)
{
    fixture_ctx_t *ctx = context;
    TEST_ASSERT_EQUAL_UINT(ESY_SNTP_PACKET_BYTES, capacity);
    memcpy(packet, ctx->response, capacity);
    *length = ctx->response_length;
    ctx->receive_calls++;
    ctx->now_us += ctx->receive_elapsed_us;
    return ctx->receive_error;
}

static esy_sntp_config_t config(void)
{
    const esy_sntp_config_t value = {
        &s_ctx.time, read_clock, &s_ctx, send_packet, receive_packet, &s_ctx
    };
    return value;
}

void setUp(void)
{
    const esy_time_config_t clock = {read_clock, &s_ctx};
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.receive_error = ESY_ERR_BUSY;
    s_ctx.response_length = ESY_SNTP_PACKET_BYTES;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_init(&s_ctx.time, &clock));
    const esy_sntp_config_t cfg = config();
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_init(&s_ctx.client, &cfg));
}

void tearDown(void)
{
}

static void put_timestamp(uint8_t *destination, uint32_t unix_seconds,
                           uint32_t fraction)
{
    uint64_t wire = ((uint64_t)(uint32_t)(unix_seconds + UINT32_C(2208988800)) << 32u) |
                    fraction;
    for (size_t i = 0; i < sizeof(wire); ++i) {
        destination[sizeof(wire) - 1u - i] = (uint8_t)wire;
        wire >>= 8u;
    }
}

static void prepare_response(uint32_t unix_seconds, uint32_t receive_fraction,
                               uint32_t transmit_fraction)
{
    memset(s_ctx.response, 0, sizeof(s_ctx.response));
    s_ctx.response[0] = (4u << 3u) | 4u;
    s_ctx.response[1] = 2;
    memcpy(s_ctx.response + 24, s_ctx.sent + 40, 8);
    put_timestamp(s_ctx.response + 32, unix_seconds, receive_fraction);
    put_timestamp(s_ctx.response + 40, unix_seconds, transmit_fraction);
    s_ctx.receive_error = ESY_OK;
}

static esy_sntp_status_t status(void)
{
    esy_sntp_status_t value;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_get_status(&s_ctx.client, &value));
    return value;
}

void test_sntp_bootstraps_utc_once_and_completed_state_never_rearms(void)
{
    esy_timestamp_t utc;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 2000000, 1));
    TEST_ASSERT_EQUAL_UINT8(0x23, s_ctx.sent[0]);
    TEST_ASSERT_EQUAL_UINT8(0x20, s_ctx.sent[3]);
    TEST_ASSERT_EQUAL_UINT8(1, s_ctx.sent[47]);
    prepare_response(1700000000, UINT32_C(0x80000000), UINT32_C(0x80000000));
    s_ctx.now_us = 1000000;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    esy_sntp_status_t info = status();
    TEST_ASSERT_EQUAL(ESY_SNTP_COMPLETE, info.state);
    TEST_ASSERT_FALSE(info.offset_valid);
    TEST_ASSERT_EQUAL_UINT64(1000000000, info.round_trip_ns);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &utc));
    TEST_ASSERT_EQUAL_INT64(1700000001, utc.seconds);
    TEST_ASSERT_EQUAL_UINT32(0, utc.nanoseconds);
    const unsigned calls = s_ctx.clock_calls;
    s_ctx.now_us = 0;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_UINT(calls, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.receive_calls);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_start(&s_ctx.client, 100, 1));
}

void test_sntp_computes_four_timestamp_offset_and_server_processing_delay(void)
{
    const esy_timestamp_t initial = {1700000000, 0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &initial));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 2000000, 123));
    prepare_response(1700000100, UINT32_C(0x40000000), UINT32_C(0x80000000));
    s_ctx.now_us = 1000000;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    const esy_sntp_status_t info = status();
    TEST_ASSERT_TRUE(info.offset_valid);
    TEST_ASSERT_EQUAL_INT64(1700000100, info.synchronized_utc.seconds);
    TEST_ASSERT_EQUAL_UINT32(875000000, info.synchronized_utc.nanoseconds);
    TEST_ASSERT_EQUAL_UINT64(750000000, info.round_trip_ns);
    TEST_ASSERT_EQUAL_INT64(99875000000LL, info.offset_ns);
}

void test_sntp_decodes_2036_rollover_and_fixed_era_endpoints(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    prepare_response(UINT32_C(2085978496), 1, 1);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_INT64(2085978496, status().synchronized_utc.seconds);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_invalidate(&s_ctx.time));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 2));
    prepare_response(0, 0, 0);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_INT64(0, status().synchronized_utc.seconds);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 3));
    prepare_response(UINT32_MAX, UINT32_MAX, UINT32_MAX);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_INT64(UINT32_MAX, status().synchronized_utc.seconds);
    TEST_ASSERT_EQUAL_UINT32(999999999, status().synchronized_utc.nanoseconds);
}

void test_sntp_response_can_span_the_2036_ntp_era_boundary(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 2000000, 1));
    prepare_response(UINT32_C(2085978495), UINT32_C(0xc0000000), 0);
    put_timestamp(s_ctx.response + 40, UINT32_C(2085978496), UINT32_C(0x40000000));
    s_ctx.now_us = 1000000;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    const esy_sntp_status_t info = status();
    TEST_ASSERT_EQUAL_INT64(2085978496, info.synchronized_utc.seconds);
    TEST_ASSERT_EQUAL_UINT32(500000000, info.synchronized_utc.nanoseconds);
    TEST_ASSERT_EQUAL_UINT64(500000000, info.round_trip_ns);
}

void test_sntp_wait_crosses_wrap_and_expires_exactly_at_timeout(void)
{
    s_ctx.now_us = UINT64_MAX - 5u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 10, 1));
    s_ctx.now_us = 3;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_WAITING, status().state);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.receive_calls);
    s_ctx.now_us = 4;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.receive_calls);
    s_ctx.now_us = UINT64_MAX - 5u;
    const unsigned clock_calls = s_ctx.clock_calls;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_UINT(clock_calls, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 10, 2));
    s_ctx.now_us = 5;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
}

void test_sntp_cancel_stays_disarmed_across_full_cycle_then_explicit_restart(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_cancel(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_IDLE, status().state);
    s_ctx.now_us = 123;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, UINT32_MAX, 1));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_cancel(&s_ctx.client));
    s_ctx.now_us = UINT64_MAX;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    s_ctx.now_us = 123;
    const unsigned clock_calls = s_ctx.clock_calls;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_CANCELLED, status().state);
    TEST_ASSERT_EQUAL_UINT(clock_calls, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.receive_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, UINT32_MAX, 2));
    s_ctx.now_us += (uint64_t)UINT32_MAX - 1u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    s_ctx.now_us++;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
}

void test_sntp_discards_malformed_and_unmatched_packets_without_setting_utc(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    prepare_response(1700000000, 0, 0);
    s_ctx.response_length = 47;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    s_ctx.response_length = 49;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    s_ctx.response_length = 48;
    s_ctx.response[31] ^= 1;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    prepare_response(1700000000, 0, 0);
    s_ctx.response[0] = 0xe4;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    s_ctx.response[0] = 0x1c;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    s_ctx.response[0] = 0x25;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    prepare_response(1700000000, 0, 0);
    s_ctx.response[1] = 16;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    prepare_response(1700000000, 0, 0);
    memset(s_ctx.response + 32, 0, 8);
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    prepare_response(1700000000, 0, 0);
    memset(s_ctx.response + 40, 0, 8);
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_WAITING, status().state);
    esy_time_status_t time_status;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_get_status(&s_ctx.time, &time_status));
    TEST_ASSERT_FALSE(time_status.valid);
    prepare_response(1700000000, 0, 0);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
}

void test_sntp_rejects_inconsistent_server_intervals_and_result_outside_window(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    prepare_response(1700000000, UINT32_C(0x80000000), UINT32_C(0x40000000));
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    prepare_response(1700000000, 0, UINT32_MAX);
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    prepare_response(UINT32_MAX, UINT32_MAX, UINT32_MAX);
    s_ctx.now_us = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
}

void test_sntp_kiss_of_death_finishes_without_retry(void)
{
    esy_time_status_t time_status;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    prepare_response(1700000000, 0, 0);
    s_ctx.response[0] = 0xe4;
    s_ctx.response[1] = 0;
    memcpy(s_ctx.response + 12, "RATE", 4);
    memcpy(s_ctx.response + 32, s_ctx.sent + 40, 8);
    memcpy(s_ctx.response + 40, s_ctx.sent + 40, 8);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, status().last_error);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_get_status(&s_ctx.time, &time_status));
    TEST_ASSERT_FALSE(time_status.valid);
    const unsigned clock_calls = s_ctx.clock_calls;
    s_ctx.clock_error = ESY_ERR_IO;
    s_ctx.receive_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_UINT(clock_calls, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.send_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.receive_calls);
}

void test_sntp_unmatched_or_wrong_mode_kiss_of_death_keeps_wait_active(void)
{
    esy_time_status_t time_status;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    prepare_response(1700000000, 0, 0);
    s_ctx.response[0] = 0xe4;
    s_ctx.response[1] = 0;
    memcpy(s_ctx.response + 12, "RATE", 4);
    s_ctx.response[31] ^= 1;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_WAITING, status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_get_status(&s_ctx.time, &time_status));
    TEST_ASSERT_FALSE(time_status.valid);
    memcpy(s_ctx.response + 24, s_ctx.sent + 40, 8);
    s_ctx.response[0] = 0xe5;
    TEST_ASSERT_EQUAL(ESY_ERR_CRC_FAIL, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_WAITING, status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_get_status(&s_ctx.time, &time_status));
    TEST_ASSERT_FALSE(time_status.valid);
    s_ctx.response[0] = 0xe4;
    TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.send_calls);
    TEST_ASSERT_EQUAL_UINT(3, s_ctx.receive_calls);
}

void test_sntp_timeout_includes_time_spent_inside_receive_provider(void)
{
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    prepare_response(1700000000, 0, 0);
    s_ctx.receive_elapsed_us = 100;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
}

void test_sntp_final_anchor_timeout_preserves_unset_utc_and_stays_disarmed(void)
{
    esy_time_status_t time_status;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    prepare_response(1700000000, 0, 0);
    s_ctx.now_us = 99;
    s_ctx.advance_clock_call = s_ctx.clock_calls + 3;
    s_ctx.clock_elapsed_us = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_UINT64(100, s_ctx.now_us);
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, status().last_error);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_get_status(&s_ctx.time, &time_status));
    TEST_ASSERT_FALSE(time_status.valid);
    const unsigned clock_calls = s_ctx.clock_calls;
    s_ctx.now_us = 0;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_UINT(clock_calls, s_ctx.clock_calls);
    TEST_ASSERT_EQUAL_UINT(1, s_ctx.receive_calls);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 2));
    prepare_response(1700000000, 0, 0);
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_COMPLETE, status().state);
}

void test_sntp_final_anchor_after_timeout_preserves_previous_utc(void)
{
    const esy_timestamp_t initial = {100, 123000000};
    esy_timestamp_t utc;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &initial));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    prepare_response(1700000000, 0, 0);
    s_ctx.now_us = 99;
    s_ctx.advance_clock_call = s_ctx.clock_calls + 3;
    s_ctx.clock_elapsed_us = 2;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &utc));
    TEST_ASSERT_EQUAL_INT64(initial.seconds, utc.seconds);
    TEST_ASSERT_EQUAL_UINT32(123101000, utc.nanoseconds);
}

void test_sntp_final_anchor_timeout_preserves_previous_utc_across_wrap(void)
{
    const esy_timestamp_t initial = {100, 123000000};
    esy_timestamp_t utc;
    s_ctx.now_us = UINT64_MAX - 5u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &initial));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 10, 1));
    prepare_response(1700000000, 0, 0);
    s_ctx.now_us = 3;
    s_ctx.advance_clock_call = s_ctx.clock_calls + 3;
    s_ctx.clock_elapsed_us = 1;
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_UINT64(4, s_ctx.now_us);
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &utc));
    TEST_ASSERT_EQUAL_INT64(initial.seconds, utc.seconds);
    TEST_ASSERT_EQUAL_UINT32(123010000, utc.nanoseconds);
}

void test_sntp_final_anchor_before_timeout_can_complete_across_wrap(void)
{
    esy_timestamp_t utc;
    s_ctx.now_us = UINT64_MAX - 5u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 10, 1));
    prepare_response(1700000000, 0, 0);
    s_ctx.now_us = 2;
    s_ctx.advance_clock_call = s_ctx.clock_calls + 3;
    s_ctx.clock_elapsed_us = 1;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL_UINT64(3, s_ctx.now_us);
    TEST_ASSERT_EQUAL(ESY_SNTP_COMPLETE, status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_now(&s_ctx.time, &utc));
    TEST_ASSERT_EQUAL_INT64(1700000000, utc.seconds);
    TEST_ASSERT_EQUAL_UINT32(4000, utc.nanoseconds);
}

void test_sntp_transport_and_clock_errors_are_terminal_and_preserve_existing_utc(void)
{
    s_ctx.send_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sntp_start(&s_ctx.client, 100, 1));
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
    s_ctx.send_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 2));
    s_ctx.receive_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 3));
    s_ctx.clock_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sntp_start(&s_ctx.client, 100, 4));
    s_ctx.clock_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 5));
    prepare_response(1700000000, 0, 0);
    s_ctx.fail_clock_call = s_ctx.clock_calls + 3;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
}

void test_sntp_lifecycle_arguments_busy_and_copied_handles(void)
{
    esy_sntp_config_t cfg = config();
    esy_sntp_status_t info;
    esy_sntp_t copy = s_ctx.client;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sntp_init(&s_ctx.client, &cfg));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_init(NULL, &cfg));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_init(&s_ctx.client, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_process(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_start(NULL, 100, 1));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_cancel(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_get_status(&s_ctx.client, NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sntp_get_status(&copy, &info));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sntp_cancel(&copy));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sntp_deinit(&copy));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_deinit(NULL));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_start(&s_ctx.client, 0, 1));
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_start(&s_ctx.client, 100, 0));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 1));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_sntp_start(&s_ctx.client, 100, 2));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_deinit(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_deinit(&s_ctx.client));
    cfg.receive = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_sntp_init(&s_ctx.client, &cfg));
    cfg = config();
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_deinit(&s_ctx.time));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sntp_init(&s_ctx.client, &cfg));
}

void test_sntp_propagates_clock_failures_before_send_and_after_receive(void)
{
    const esy_timestamp_t initial = {100, 0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_set(&s_ctx.time, &initial));
    s_ctx.clock_error = ESY_ERR_IO;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sntp_start(&s_ctx.client, 100, 1));
    TEST_ASSERT_EQUAL_UINT(0, s_ctx.send_calls);
    s_ctx.clock_error = ESY_OK;
    TEST_ASSERT_EQUAL(ESY_OK, esy_sntp_start(&s_ctx.client, 100, 2));
    s_ctx.fail_clock_call = s_ctx.clock_calls + 2;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_sntp_process(&s_ctx.client));
    TEST_ASSERT_EQUAL(ESY_SNTP_FAILED, status().state);
    TEST_ASSERT_EQUAL(ESY_OK, esy_time_deinit(&s_ctx.time));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_sntp_start(&s_ctx.client, 100, 3));
}
