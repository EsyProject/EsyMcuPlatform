/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_json_tcp_channel.h"
#include "esy_proto_json_channel_internal.h"

#include <limits.h>
#include <string.h>

enum { JSON_TCP_MARKER = 0x4a544350 };

typedef struct {
    uint32_t marker;
    esy_network_connection_t connection;
    esy_proto_json_tcp_channel_static_resources_t resources;
    esy_proto_json_tcp_message_cb on_message;
    void *user_ctx;
    uint64_t session_id;
    size_t rx_byte_budget;
    size_t tx_byte_budget;
    size_t header_used;
    size_t rx_used;
    size_t tx_length;
    size_t tx_offset;
    uint32_t rx_expected;
    esy_err_t error;
    uint8_t header[ESY_PROTO_JSON_TCP_HEADER_BYTES];
    bool busy;
    bool callback_active;
    bool peer_drained;
} json_tcp_ctx_t;

_Static_assert(sizeof(json_tcp_ctx_t) <= ESY_PROTO_JSON_TCP_CHANNEL_STORAGE_BYTES,
               "JSON TCP channel storage is too small");
_Static_assert(ESY_PROTO_JSON_TCP_HEADER_BYTES == JSON_CHANNEL_HEADER_BYTES,
               "JSON TCP framing must match the shared wire contract");

static void store(esy_proto_json_tcp_channel_t *channel, const json_tcp_ctx_t *ctx)
{
    memcpy(channel->storage.bytes, ctx, sizeof(*ctx));
}

static esy_err_t load(const esy_proto_json_tcp_channel_t *channel, json_tcp_ctx_t *ctx)
{
    if (channel == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(ctx, channel->storage.bytes, sizeof(*ctx));
    return ctx->marker == JSON_TCP_MARKER ? ESY_OK : ESY_ERR_STATE;
}

static esy_err_t fail(json_tcp_ctx_t *ctx, esy_err_t error)
{
    if (ctx->error == ESY_OK) {
        ctx->error = error < ESY_OK ? error : ESY_ERR_IO;
    }
    ctx->peer_drained = false;
    return ctx->error;
}

static esy_err_t observe(json_tcp_ctx_t *ctx,
                          esy_network_connection_status_t *status)
{
    esy_err_t result = ctx->connection.get_status(ctx->connection.stream.context, status);
    if (result == ESY_ERR_BUSY) {
        return result;
    }
    if (result != ESY_OK) {
        return fail(ctx, result);
    }
    if ((status->state != ESY_NETWORK_CONNECTION_OPEN &&
         status->state != ESY_NETWORK_CONNECTION_EOF) ||
        status->session_id != ctx->session_id || status->session_id == 0u) {
        return fail(ctx, ESY_ERR_STATE);
    }
    return ESY_OK;
}

static bool valid_resources(const esy_proto_json_tcp_channel_t *channel,
                             const esy_proto_json_tcp_channel_config_t *config)
{
    const esy_proto_json_tcp_channel_static_resources_t *resources = config->static_resources;
    if (!json_channel_range_valid(resources, sizeof(*resources)) ||
        resources->rx_buffer_size == 0u || resources->rx_buffer_size > UINT32_MAX ||
        resources->tx_buffer_size <= ESY_PROTO_JSON_TCP_HEADER_BYTES) {
        return false;
    }
    const json_channel_range_t ranges[] = {
        {channel, sizeof(*channel)}, {config, sizeof(*config)},
        {resources, sizeof(*resources)},
        {resources->rx_buffer, resources->rx_buffer_size},
        {resources->tx_buffer, resources->tx_buffer_size}
    };
    const size_t count = sizeof(ranges) / sizeof(ranges[0]);
    for (size_t i = 0u; i < count; ++i) {
        if (!json_channel_range_valid(ranges[i].data, ranges[i].size)) {
            return false;
        }
        for (size_t j = 0u; j < i; ++j) {
            if (json_channel_overlaps(ranges[i].data, ranges[i].size, ranges[j].data, ranges[j].size)) {
                return false;
            }
        }
    }
    return true;
}

esy_err_t esy_proto_json_tcp_channel_init(esy_proto_json_tcp_channel_t *channel,
                                         const esy_proto_json_tcp_channel_config_t *config)
{
    json_tcp_ctx_t ctx;
    esy_network_connection_status_t status;
    esy_err_t result;
    if (!json_channel_range_valid(channel, sizeof(*channel)) ||
        !json_channel_range_valid(config, sizeof(*config)) ||
        config->connection.stream.send == NULL || config->connection.stream.recv == NULL ||
        config->connection.get_status == NULL || config->on_message == NULL ||
        !valid_resources(channel, config)) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(&ctx, channel->storage.bytes, sizeof(ctx));
    if (ctx.marker == JSON_TCP_MARKER) {
        return ctx.busy ? ESY_ERR_BUSY : ESY_ERR_STATE;
    }
    if (ctx.marker != 0u) {
        return ESY_ERR_STATE;
    }
    result = config->connection.get_status(config->connection.stream.context, &status);
    if (result != ESY_OK) {
        return result < ESY_OK ? result : ESY_ERR_IO;
    }
    if ((status.state != ESY_NETWORK_CONNECTION_OPEN && status.state != ESY_NETWORK_CONNECTION_EOF) ||
        status.session_id == 0u) {
        return ESY_ERR_STATE;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.marker = JSON_TCP_MARKER;
    ctx.connection = config->connection;
    ctx.resources = *config->static_resources;
    ctx.on_message = config->on_message;
    ctx.user_ctx = config->user_ctx;
    ctx.session_id = status.session_id;
    ctx.rx_byte_budget = config->rx_byte_budget != 0u ? config->rx_byte_budget :
                         ESY_PROTO_JSON_TCP_DEFAULT_BYTE_BUDGET;
    ctx.tx_byte_budget = config->tx_byte_budget != 0u ? config->tx_byte_budget :
                         ESY_PROTO_JSON_TCP_DEFAULT_BYTE_BUDGET;
    memset(channel, 0, sizeof(*channel));
    store(channel, &ctx);
    return ESY_OK;
}

esy_err_t esy_proto_json_tcp_channel_send(esy_proto_json_tcp_channel_t *channel,
                                         const char *json, size_t length)
{
    json_tcp_ctx_t ctx;
    esy_json_value_t root;
    esy_network_connection_status_t status;
    esy_err_t result = load(channel, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.busy && !ctx.callback_active) {
        return ESY_ERR_BUSY;
    }
    if (ctx.error != ESY_OK) {
        return ctx.error;
    }
    if (ctx.tx_length != 0u) {
        return ESY_ERR_BUSY;
    }
    if (!json_channel_range_valid(json, length) || length > UINT32_MAX ||
        json_channel_overlaps(json, length, channel, sizeof(*channel)) ||
        json_channel_overlaps(json, length, ctx.resources.tx_buffer, ctx.resources.tx_buffer_size)) {
        return ESY_ERR_INVALID_ARG;
    }
    if (length > ctx.resources.tx_buffer_size - ESY_PROTO_JSON_TCP_HEADER_BYTES) {
        return ESY_ERR_NO_MEM;
    }
    result = esy_json_parse(json, length, &root);
    if (result != ESY_OK) {
        return result;
    }
    bool was_busy = ctx.busy;
    bool was_callback = ctx.callback_active;
    ctx.busy = true;
    ctx.callback_active = false;
    store(channel, &ctx);
    result = observe(&ctx, &status);
    if (result == ESY_OK) {
        json_channel_write_length(ctx.resources.tx_buffer, (uint32_t)length);
        memcpy(ctx.resources.tx_buffer + ESY_PROTO_JSON_TCP_HEADER_BYTES, json, length);
        ctx.tx_length = length + ESY_PROTO_JSON_TCP_HEADER_BYTES;
        ctx.tx_offset = 0u;
        ctx.peer_drained = false;
    }
    ctx.busy = was_busy;
    ctx.callback_active = was_callback;
    store(channel, &ctx);
    return result;
}

static esy_err_t transmit(json_tcp_ctx_t *ctx)
{
    size_t count;
    int32_t sent;
    if (ctx->tx_length == 0u) {
        return ESY_OK;
    }
    count = ctx->tx_length - ctx->tx_offset;
    if (count > ctx->tx_byte_budget) {
        count = ctx->tx_byte_budget;
    }
    if (count > INT32_MAX) {
        count = INT32_MAX;
    }
    sent = ctx->connection.stream.send(ctx->connection.stream.context,
                                       ctx->resources.tx_buffer + ctx->tx_offset, count);
    if (sent == 0 || sent == ESY_ERR_BUSY) {
        return ESY_ERR_BUSY;
    }
    if (sent < 0) {
        return fail(ctx, sent);
    }
    if ((size_t)sent > count) {
        return fail(ctx, ESY_ERR_IO);
    }
    ctx->tx_offset += (size_t)sent;
    if (ctx->tx_offset == ctx->tx_length) {
        ctx->tx_offset = 0u;
        ctx->tx_length = 0u;
    }
    return ESY_OK;
}

static esy_err_t deliver(esy_proto_json_tcp_channel_t *channel, json_tcp_ctx_t *ctx)
{
    esy_json_value_t root;
    esy_network_connection_status_t status;
    esy_err_t result = observe(ctx, &status);
    if (result != ESY_OK) {
        return result;
    }
    result = esy_json_parse((const char *)ctx->resources.rx_buffer, ctx->rx_expected, &root);
    if (result != ESY_OK) {
        return fail(ctx, result);
    }
    ctx->header_used = 0u;
    ctx->rx_used = 0u;
    ctx->rx_expected = 0u;
    ctx->callback_active = true;
    store(channel, ctx);
    ctx->on_message(channel, &root, ctx->user_ctx);
    memcpy(ctx, channel->storage.bytes, sizeof(*ctx));
    ctx->callback_active = false;
    return ctx->error;
}

static esy_err_t receive(esy_proto_json_tcp_channel_t *channel, json_tcp_ctx_t *ctx,
                          const esy_network_connection_status_t *initial)
{
    size_t budget = ctx->rx_byte_budget;
    size_t available = initial->rx_available;
    if (ctx->rx_expected != 0u && ctx->rx_used == ctx->rx_expected) {
        return deliver(channel, ctx);
    }
    while (budget != 0u && available != 0u) {
        bool reading_header = ctx->header_used < ESY_PROTO_JSON_TCP_HEADER_BYTES;
        uint8_t *destination = reading_header ? ctx->header + ctx->header_used :
                               ctx->resources.rx_buffer + ctx->rx_used;
        size_t count = reading_header ? ESY_PROTO_JSON_TCP_HEADER_BYTES - ctx->header_used :
                       (size_t)ctx->rx_expected - ctx->rx_used;
        if (count > budget) {
            count = budget;
        }
        if (count > available) {
            count = available;
        }
        if (count > INT32_MAX) {
            count = INT32_MAX;
        }
        int32_t received = ctx->connection.stream.recv(ctx->connection.stream.context,
                                                       destination, count);
        if (received == 0 || received == ESY_ERR_BUSY) {
            return received == 0 ? ESY_OK : ESY_ERR_BUSY;
        }
        if (received < 0) {
            if (received == ESY_ERR_IO) {
                esy_network_connection_status_t current;
                esy_err_t result = observe(ctx, &current);
                if (result != ESY_OK) {
                    return result;
                }
                if (current.state == ESY_NETWORK_CONNECTION_EOF && current.rx_available == 0u) {
                    return ESY_OK;
                }
            }
            return fail(ctx, received);
        }
        if ((size_t)received > count) {
            return fail(ctx, ESY_ERR_IO);
        }
        budget -= (size_t)received;
        available -= (size_t)received;
        if (reading_header) {
            ctx->header_used += (size_t)received;
            if (ctx->header_used == ESY_PROTO_JSON_TCP_HEADER_BYTES) {
                ctx->rx_expected = json_channel_read_length(ctx->header);
                if (ctx->rx_expected == 0u) {
                    return fail(ctx, ESY_ERR_FAIL);
                }
                if (ctx->rx_expected > ctx->resources.rx_buffer_size) {
                    return fail(ctx, ESY_ERR_NO_MEM);
                }
            }
        } else {
            ctx->rx_used += (size_t)received;
            if (ctx->rx_used == ctx->rx_expected) {
                return deliver(channel, ctx);
            }
        }
    }
    return ESY_OK;
}

esy_err_t esy_proto_json_tcp_channel_poll(esy_proto_json_tcp_channel_t *channel)
{
    json_tcp_ctx_t ctx;
    esy_network_connection_status_t status;
    esy_err_t result = load(channel, &ctx);
    bool pressure = false;
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.busy) {
        return ESY_ERR_BUSY;
    }
    if (ctx.error != ESY_OK) {
        return ctx.error;
    }
    ctx.busy = true;
    store(channel, &ctx);
    if (ctx.connection.stream.progress != NULL) {
        result = ctx.connection.stream.progress(ctx.connection.stream.context);
        if (result != ESY_OK) {
            if (result != ESY_ERR_BUSY) {
                result = fail(&ctx, result);
            }
            goto done;
        }
    }
    result = observe(&ctx, &status);
    if (result != ESY_OK) {
        goto done;
    }
    if (!ctx.peer_drained) {
        result = transmit(&ctx);
        if (result == ESY_ERR_BUSY) {
            pressure = true;
        } else if (result != ESY_OK) {
            goto done;
        }
        result = observe(&ctx, &status);
        if (result != ESY_OK) {
            goto done;
        }
        result = receive(channel, &ctx, &status);
        if (result == ESY_ERR_BUSY) {
            pressure = true;
        } else if (result != ESY_OK) {
            goto done;
        }
    }
    result = observe(&ctx, &status);
    if (result != ESY_OK) {
        goto done;
    }
    if (status.state == ESY_NETWORK_CONNECTION_EOF && status.rx_available == 0u) {
        bool ready = ctx.rx_expected != 0u && ctx.rx_used == ctx.rx_expected;
        if (!ready && (ctx.header_used != 0u || ctx.rx_expected != 0u || ctx.rx_used != 0u)) {
            result = fail(&ctx, ESY_ERR_FAIL);
            goto done;
        }
        if (!ready && ctx.tx_length == 0u && status.unacked_bytes == 0u) {
            ctx.peer_drained = true;
        }
    }
    result = pressure ? ESY_ERR_BUSY : ESY_OK;
done:
    ctx.busy = false;
    ctx.callback_active = false;
    store(channel, &ctx);
    return result;
}

esy_err_t esy_proto_json_tcp_channel_get_status(const esy_proto_json_tcp_channel_t *channel,
                                               esy_proto_json_tcp_channel_status_t *status)
{
    json_tcp_ctx_t ctx;
    esy_err_t result;
    if (status == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    result = load(channel, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    esy_proto_json_tcp_channel_status_t snapshot = {
        .session_id = ctx.session_id,
        .rx_received = ctx.rx_used,
        .rx_expected = ctx.rx_expected,
        .tx_remaining = ctx.tx_length - ctx.tx_offset,
        .error = ctx.error,
        .tx_pending = ctx.tx_length != 0u,
        .peer_drained = ctx.peer_drained
    };
    *status = snapshot;
    return ESY_OK;
}

esy_err_t esy_proto_json_tcp_channel_deinit(esy_proto_json_tcp_channel_t *channel)
{
    json_tcp_ctx_t ctx;
    if (channel == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(&ctx, channel->storage.bytes, sizeof(ctx));
    if (ctx.marker == 0u) {
        return ESY_OK;
    }
    if (ctx.marker != JSON_TCP_MARKER) {
        return ESY_ERR_STATE;
    }
    if (ctx.busy) {
        return ESY_ERR_BUSY;
    }
    memset(channel, 0, sizeof(*channel));
    return ESY_OK;
}
