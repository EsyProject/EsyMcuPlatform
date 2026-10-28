/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/time/esy_time.h"

#include <string.h>

#include "esy_time_internal.h"

#define ESY_TIME_MAGIC UINT32_C(0x4554494d)
#define MICROSECONDS_PER_SECOND UINT64_C(1000000)
#define NANOSECONDS_PER_MICROSECOND UINT64_C(1000)

typedef struct {
    uint32_t magic;
    const esy_time_t *owner;
    esy_time_config_t config;
    esy_timestamp_t anchor;
    uint64_t anchor_us;
    bool valid;
} time_ctx_t;

_Static_assert(sizeof(time_ctx_t) <= ESY_TIME_STORAGE_BYTES,
               "Time handle storage is too small");

static esy_err_t read_context(const esy_time_t *time, time_ctx_t *ctx)
{
    if (time == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(ctx, time->storage.bytes, sizeof(*ctx));
    if (ctx->magic != ESY_TIME_MAGIC || ctx->owner != time) {
        return ESY_ERR_STATE;
    }
    return ESY_OK;
}

static bool timestamp_valid(const esy_timestamp_t *timestamp)
{
    return timestamp != NULL && timestamp->seconds >= 0 &&
           timestamp->seconds <= ESY_TIME_MAX_SECONDS &&
           timestamp->nanoseconds < ESY_TIME_NANOSECONDS_PER_SECOND;
}

esy_err_t esy_time_init(esy_time_t *time, const esy_time_config_t *config)
{
    time_ctx_t ctx;
    if (time == NULL || config == NULL || config->monotonic_us == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(time->storage.bytes); ++i) {
        if (time->storage.bytes[i] != 0u) {
            return ESY_ERR_STATE;
        }
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.magic = ESY_TIME_MAGIC;
    ctx.owner = time;
    ctx.config = *config;
    memcpy(time->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_time_deinit(esy_time_t *time)
{
    time_ctx_t ctx;
    esy_err_t result;
    if (time == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    bool zero = true;
    for (size_t i = 0; i < sizeof(time->storage.bytes); ++i) {
        zero = zero && time->storage.bytes[i] == 0u;
    }
    if (zero) {
        return ESY_OK;
    }
    result = read_context(time, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    memset(time, 0, sizeof(*time));
    return ESY_OK;
}

static esy_err_t sample_anchor(esy_time_t *time,
                               const esy_timestamp_t *timestamp, time_ctx_t *ctx)
{
    esy_err_t result;
    if (!timestamp_valid(timestamp)) {
        return ESY_ERR_INVALID_ARG;
    }
    result = read_context(time, ctx);
    if (result != ESY_OK) {
        return result;
    }
    result = ctx->config.monotonic_us(ctx->config.provider_ctx, &ctx->anchor_us);
    if (result != ESY_OK) {
        return result;
    }
    ctx->anchor = *timestamp;
    ctx->valid = true;
    return ESY_OK;
}

esy_err_t esy_time_set(esy_time_t *time, const esy_timestamp_t *timestamp)
{
    time_ctx_t ctx;
    esy_err_t result = sample_anchor(time, timestamp, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    memcpy(time->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_time_set_before(esy_time_t *time, const esy_timestamp_t *timestamp,
                              uint64_t started_us, uint32_t timeout_us)
{
    time_ctx_t ctx;
    esy_err_t result;
    if (timeout_us == 0u) {
        return ESY_ERR_INVALID_ARG;
    }
    result = sample_anchor(time, timestamp, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.anchor_us - started_us >= timeout_us) {
        return ESY_ERR_TIMEOUT;
    }
    memcpy(time->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_time_now(const esy_time_t *time, esy_timestamp_t *timestamp)
{
    time_ctx_t ctx;
    uint64_t now_us;
    uint64_t elapsed;
    uint64_t seconds;
    uint64_t nanoseconds;
    esy_timestamp_t value;
    esy_err_t result;
    if (timestamp == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    result = read_context(time, &ctx);
    if (result != ESY_OK || !ctx.valid) {
        return result == ESY_OK ? ESY_ERR_STATE : result;
    }
    result = ctx.config.monotonic_us(ctx.config.provider_ctx, &now_us);
    if (result != ESY_OK) {
        return result;
    }
    elapsed = now_us - ctx.anchor_us;
    if (elapsed > UINT64_MAX / 2u) {
        return ESY_ERR_STATE;
    }
    nanoseconds = ctx.anchor.nanoseconds +
                  (elapsed % MICROSECONDS_PER_SECOND) * NANOSECONDS_PER_MICROSECOND;
    seconds = (uint64_t)ctx.anchor.seconds + elapsed / MICROSECONDS_PER_SECOND +
              nanoseconds / ESY_TIME_NANOSECONDS_PER_SECOND;
    if (seconds > (uint64_t)ESY_TIME_MAX_SECONDS) {
        return ESY_ERR_STATE;
    }
    value.seconds = (int64_t)seconds;
    value.nanoseconds = (uint32_t)(nanoseconds % ESY_TIME_NANOSECONDS_PER_SECOND);
    *timestamp = value;
    return ESY_OK;
}

esy_err_t esy_time_invalidate(esy_time_t *time)
{
    time_ctx_t ctx;
    esy_err_t result = read_context(time, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    ctx.valid = false;
    memcpy(time->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_time_get_status(const esy_time_t *time, esy_time_status_t *status)
{
    time_ctx_t ctx;
    esy_err_t result;
    if (status == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    result = read_context(time, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    status->valid = ctx.valid;
    return ESY_OK;
}
