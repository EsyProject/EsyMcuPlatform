/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/time/esy_sntp.h"

#include <string.h>

#include "esy_time_internal.h"

#define SNTP_MAGIC UINT32_C(0x534e5450)
#define NTP_UNIX_OFFSET UINT64_C(2208988800)
#define NTP_ERA_SECONDS UINT64_C(4294967296)
#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)
#define NANOSECONDS_PER_MICROSECOND UINT64_C(1000)
#define ORIGINATE_OFFSET 24u
#define RECEIVE_OFFSET 32u
#define TRANSMIT_OFFSET 40u
#define NTP_VERSION_SHIFT 3u
#define NTP_LI_SHIFT 6u
#define NTP_VERSION_4 4u
#define NTP_CLIENT_MODE 3u
#define NTP_SERVER_MODE 4u
#define NTP_UNSYNCHRONIZED 3u
#define NTP_MAX_STRATUM 15u
#define NTP_FIELD_MASK 7u
#define NTP_PRECISION_OFFSET 3u
#define NTP_MINIMIZED_PRECISION 0x20u
#define OCTET_BITS 8u

typedef struct {
    uint32_t magic;
    const esy_sntp_t *owner;
    esy_sntp_config_t config;
    esy_sntp_status_t status;
    uint64_t token;
    uint64_t started_us;
    uint64_t initial_utc_ns;
    uint32_t timeout_us;
    bool initial_valid;
} sntp_ctx_t;

_Static_assert(sizeof(sntp_ctx_t) <= ESY_SNTP_STORAGE_BYTES,
               "SNTP handle storage is too small");

static esy_err_t read_context(const esy_sntp_t *client, sntp_ctx_t *ctx)
{
    if (client == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(ctx, client->storage.bytes, sizeof(*ctx));
    return ctx->magic == SNTP_MAGIC && ctx->owner == client ? ESY_OK : ESY_ERR_STATE;
}

static uint64_t read_be64(const uint8_t *bytes)
{
    uint64_t value = 0u;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value = (value << OCTET_BITS) | bytes[i];
    }
    return value;
}

static void write_be64(uint8_t *bytes, uint64_t value)
{
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[sizeof(value) - 1u - i] = (uint8_t)value;
        value >>= OCTET_BITS;
    }
}

static uint64_t decode_timestamp(uint64_t wire)
{
    uint64_t seconds = wire >> 32u;
    if (seconds < NTP_UNIX_OFFSET) {
        seconds += NTP_ERA_SECONDS;
    }
    seconds -= NTP_UNIX_OFFSET;
    return seconds * NANOSECONDS_PER_SECOND +
           ((wire & UINT32_MAX) * NANOSECONDS_PER_SECOND >> 32u);
}

static esy_err_t finish_error(esy_sntp_t *client, sntp_ctx_t *ctx,
                              esy_err_t error)
{
    ctx->status.state = ESY_SNTP_FAILED;
    ctx->status.last_error = error;
    memcpy(client->storage.bytes, ctx, sizeof(*ctx));
    return error;
}

static esy_err_t accept_response(sntp_ctx_t *ctx, const uint8_t *packet,
                                 size_t length, uint64_t elapsed_us)
{
    uint64_t receive_wire;
    uint64_t transmit_wire;
    uint64_t receive_ns;
    uint64_t transmit_ns;
    uint64_t processing_ns;
    uint64_t network_ns;
    uint64_t corrected_ns;
    if (length != ESY_SNTP_PACKET_BYTES ||
        ((packet[0] >> NTP_VERSION_SHIFT) & NTP_FIELD_MASK) != NTP_VERSION_4 ||
        (packet[0] & NTP_FIELD_MASK) != NTP_SERVER_MODE ||
        read_be64(packet + ORIGINATE_OFFSET) != ctx->token) {
        return ESY_ERR_CRC_FAIL;
    }
    if (packet[1] == 0u) {
        return ESY_ERR_NOT_SUPPORTED;
    }
    receive_wire = read_be64(packet + RECEIVE_OFFSET);
    transmit_wire = read_be64(packet + TRANSMIT_OFFSET);
    if (packet[0] >> NTP_LI_SHIFT == NTP_UNSYNCHRONIZED ||
        packet[1] > NTP_MAX_STRATUM || receive_wire == 0u || transmit_wire == 0u) {
        return ESY_ERR_CRC_FAIL;
    }
    receive_ns = decode_timestamp(receive_wire);
    transmit_ns = decode_timestamp(transmit_wire);
    if (transmit_ns < receive_ns) {
        return ESY_ERR_CRC_FAIL;
    }
    processing_ns = transmit_ns - receive_ns;
    if (processing_ns > elapsed_us * NANOSECONDS_PER_MICROSECOND) {
        return ESY_ERR_CRC_FAIL;
    }
    network_ns = elapsed_us * NANOSECONDS_PER_MICROSECOND - processing_ns;
    corrected_ns = transmit_ns + network_ns / 2u;
    if (corrected_ns / NANOSECONDS_PER_SECOND > UINT32_MAX) {
        return ESY_ERR_CRC_FAIL;
    }
    ctx->status.synchronized_utc.seconds = (int64_t)(corrected_ns / NANOSECONDS_PER_SECOND);
    ctx->status.synchronized_utc.nanoseconds = (uint32_t)(corrected_ns % NANOSECONDS_PER_SECOND);
    ctx->status.round_trip_ns = network_ns;
    ctx->status.offset_valid = ctx->initial_valid;
    ctx->status.offset_ns = ctx->initial_valid ?
        (int64_t)corrected_ns - (int64_t)(ctx->initial_utc_ns +
            elapsed_us * NANOSECONDS_PER_MICROSECOND) : 0;
    ctx->status.stratum = packet[1];
    return ESY_OK;
}

esy_err_t esy_sntp_init(esy_sntp_t *client, const esy_sntp_config_t *config)
{
    sntp_ctx_t ctx;
    esy_time_status_t time_status;
    esy_err_t result;
    if (client == NULL || config == NULL || config->time == NULL ||
        config->monotonic_us == NULL || config->send == NULL || config->receive == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(client->storage.bytes); ++i) {
        if (client->storage.bytes[i] != 0u) {
            return ESY_ERR_STATE;
        }
    }
    result = esy_time_get_status(config->time, &time_status);
    if (result != ESY_OK) {
        return result;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.magic = SNTP_MAGIC;
    ctx.owner = client;
    ctx.config = *config;
    ctx.status.state = ESY_SNTP_IDLE;
    memcpy(client->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_sntp_deinit(esy_sntp_t *client)
{
    sntp_ctx_t ctx;
    bool zero = true;
    esy_err_t result;
    if (client == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(client->storage.bytes); ++i) {
        zero = zero && client->storage.bytes[i] == 0u;
    }
    if (zero) {
        return ESY_OK;
    }
    result = read_context(client, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    memset(client, 0, sizeof(*client));
    return ESY_OK;
}

esy_err_t esy_sntp_start(esy_sntp_t *client, uint32_t timeout_us,
                         uint64_t request_token)
{
    sntp_ctx_t ctx;
    esy_timestamp_t initial_utc;
    esy_time_status_t time_status;
    uint8_t packet[ESY_SNTP_PACKET_BYTES] = {0};
    esy_err_t result = read_context(client, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (timeout_us == 0u || request_token == 0u || request_token == ctx.token) {
        return ESY_ERR_INVALID_ARG;
    }
    if (ctx.status.state == ESY_SNTP_WAITING) {
        return ESY_ERR_BUSY;
    }
    memset(&ctx.status, 0, sizeof(ctx.status));
    ctx.token = request_token;
    ctx.timeout_us = timeout_us;
    ctx.initial_valid = false;
    result = esy_time_get_status(ctx.config.time, &time_status);
    if (result != ESY_OK) {
        return finish_error(client, &ctx, result);
    }
    if (time_status.valid) {
        result = esy_time_now(ctx.config.time, &initial_utc);
        if (result != ESY_OK) {
            return finish_error(client, &ctx, result);
        }
        if (initial_utc.seconds <= UINT32_MAX) {
            ctx.initial_valid = true;
            ctx.initial_utc_ns = (uint64_t)initial_utc.seconds * NANOSECONDS_PER_SECOND +
                                 initial_utc.nanoseconds;
        }
    }
    result = ctx.config.monotonic_us(ctx.config.clock_ctx, &ctx.started_us);
    if (result != ESY_OK) {
        return finish_error(client, &ctx, result);
    }
    packet[0] = (NTP_VERSION_4 << NTP_VERSION_SHIFT) | NTP_CLIENT_MODE;
    packet[NTP_PRECISION_OFFSET] = NTP_MINIMIZED_PRECISION;
    write_be64(packet + TRANSMIT_OFFSET, request_token);
    result = ctx.config.send(ctx.config.transport_ctx, packet, sizeof(packet));
    if (result != ESY_OK) {
        return finish_error(client, &ctx, result);
    }
    ctx.status.state = ESY_SNTP_WAITING;
    memcpy(client->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_sntp_process(esy_sntp_t *client)
{
    sntp_ctx_t ctx;
    uint8_t packet[ESY_SNTP_PACKET_BYTES];
    size_t length = 0u;
    uint64_t now_us;
    esy_err_t receive_result;
    esy_err_t result = read_context(client, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.status.state != ESY_SNTP_WAITING) {
        if (ctx.status.state == ESY_SNTP_IDLE) {
            return ESY_ERR_STATE;
        }
        return ctx.status.state == ESY_SNTP_FAILED ? ctx.status.last_error : ESY_OK;
    }
    result = ctx.config.monotonic_us(ctx.config.clock_ctx, &now_us);
    if (result != ESY_OK) {
        return finish_error(client, &ctx, result);
    }
    if (now_us - ctx.started_us >= ctx.timeout_us) {
        return finish_error(client, &ctx, ESY_ERR_TIMEOUT);
    }
    receive_result = ctx.config.receive(ctx.config.transport_ctx, packet,
                                        sizeof(packet), &length);
    result = ctx.config.monotonic_us(ctx.config.clock_ctx, &now_us);
    if (result != ESY_OK) {
        return finish_error(client, &ctx, result);
    }
    if (now_us - ctx.started_us >= ctx.timeout_us) {
        return finish_error(client, &ctx, ESY_ERR_TIMEOUT);
    }
    if (receive_result == ESY_ERR_BUSY) {
        return ESY_OK;
    }
    if (receive_result != ESY_OK) {
        return finish_error(client, &ctx, receive_result);
    }
    result = accept_response(&ctx, packet, length, now_us - ctx.started_us);
    if (result == ESY_ERR_CRC_FAIL) {
        ctx.status.last_error = result;
        memcpy(client->storage.bytes, &ctx, sizeof(ctx));
        return result;
    }
    if (result == ESY_OK) {
        result = esy_time_set_before(ctx.config.time, &ctx.status.synchronized_utc,
                                     ctx.started_us, ctx.timeout_us);
    }
    if (result != ESY_OK) {
        return finish_error(client, &ctx, result);
    }
    ctx.status.state = ESY_SNTP_COMPLETE;
    ctx.status.last_error = ESY_OK;
    memcpy(client->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_sntp_cancel(esy_sntp_t *client)
{
    sntp_ctx_t ctx;
    esy_err_t result = read_context(client, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.status.state == ESY_SNTP_WAITING) {
        ctx.status.state = ESY_SNTP_CANCELLED;
        ctx.status.last_error = ESY_OK;
        memcpy(client->storage.bytes, &ctx, sizeof(ctx));
    }
    return ESY_OK;
}

esy_err_t esy_sntp_get_status(const esy_sntp_t *client, esy_sntp_status_t *status)
{
    sntp_ctx_t ctx;
    esy_err_t result;
    if (status == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    result = read_context(client, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    *status = ctx.status;
    return ESY_OK;
}
