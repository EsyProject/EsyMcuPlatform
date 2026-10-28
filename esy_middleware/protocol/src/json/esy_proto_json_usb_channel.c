/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_json_usb_channel.h"
#include "esy_proto_json_channel_internal.h"

#include <string.h>

enum { JSON_USB_MARKER = 0x4a554250 };

typedef struct {
    uint32_t marker;
    esy_usb_cdc_t *cdc;
    esy_proto_json_usb_channel_static_resources_t resources;
    esy_proto_json_usb_message_cb on_message;
    void *user_ctx;
    size_t rx_byte_budget;
    size_t tx_byte_budget;
    size_t header_used;
    size_t rx_used;
    size_t tx_length;
    size_t tx_offset;
    uint32_t session_id;
    uint32_t rx_expected;
    esy_err_t error;
    uint8_t header[ESY_PROTO_JSON_USB_HEADER_BYTES];
    bool connected;
    bool busy;
    bool callback_active;
} json_usb_ctx_t;

_Static_assert(sizeof(json_usb_ctx_t) <= ESY_PROTO_JSON_USB_CHANNEL_STORAGE_BYTES,
               "JSON USB channel storage is too small");
_Static_assert(ESY_PROTO_JSON_USB_HEADER_BYTES == JSON_CHANNEL_HEADER_BYTES,
               "JSON USB framing must match the shared wire contract");

static void store(esy_proto_json_usb_channel_t *channel, const json_usb_ctx_t *ctx)
{
    memcpy(channel->storage.bytes, ctx, sizeof(*ctx));
}

static esy_err_t load(const esy_proto_json_usb_channel_t *channel, json_usb_ctx_t *ctx)
{
    if (channel == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(ctx, channel->storage.bytes, sizeof(*ctx));
    return ctx->marker == JSON_USB_MARKER ? ESY_OK : ESY_ERR_STATE;
}

static esy_err_t fail(json_usb_ctx_t *ctx, esy_err_t error)
{
    if (ctx->error == ESY_OK) {
        ctx->error = error;
    }
    return ctx->error;
}

static esy_err_t observe(json_usb_ctx_t *ctx)
{
    uint32_t session_id = esy_usb_cdc_get_session_id(ctx->cdc);
    bool connected = esy_usb_cdc_connected(ctx->cdc);
    if (session_id == 0u) {
        ctx->connected = false;
        return fail(ctx, ESY_ERR_STATE);
    }
    if (session_id != ctx->session_id) {
        ctx->session_id = session_id;
        ctx->connected = connected;
        ctx->header_used = 0u;
        ctx->rx_used = 0u;
        ctx->rx_expected = 0u;
        ctx->tx_length = 0u;
        ctx->tx_offset = 0u;
        ctx->error = ESY_OK;
        return ESY_ERR_IO;
    }
    if (ctx->connected && !connected) {
        ctx->connected = false;
        return fail(ctx, ESY_ERR_STATE);
    }
    ctx->connected = connected;
    return ctx->error;
}

static bool valid_resources(const esy_proto_json_usb_channel_t *channel,
                             const esy_proto_json_usb_channel_config_t *config)
{
    const esy_proto_json_usb_channel_static_resources_t *resources = config->static_resources;
    if (!json_channel_range_valid(resources, sizeof(*resources)) ||
        resources->rx_buffer_size == 0u || resources->rx_buffer_size > UINT32_MAX ||
        resources->tx_buffer_size <= ESY_PROTO_JSON_USB_HEADER_BYTES) {
        return false;
    }
    const json_channel_range_t ranges[] = {
        {channel, sizeof(*channel)}, {config, sizeof(*config)},
        {config->cdc, sizeof(*config->cdc)}, {resources, sizeof(*resources)},
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

esy_err_t esy_proto_json_usb_channel_init(esy_proto_json_usb_channel_t *channel,
                                         const esy_proto_json_usb_channel_config_t *config)
{
    json_usb_ctx_t ctx;
    uint32_t session_id;
    if (!json_channel_range_valid(channel, sizeof(*channel)) ||
        !json_channel_range_valid(config, sizeof(*config)) ||
        config->on_message == NULL || !valid_resources(channel, config)) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(&ctx, channel->storage.bytes, sizeof(ctx));
    if (ctx.marker == JSON_USB_MARKER) {
        return ctx.busy ? ESY_ERR_BUSY : ESY_ERR_STATE;
    }
    if (ctx.marker != 0u) {
        return ESY_ERR_STATE;
    }
    session_id = esy_usb_cdc_get_session_id(config->cdc);
    if (session_id == 0u) {
        return ESY_ERR_INVALID_ARG;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.marker = JSON_USB_MARKER;
    ctx.cdc = config->cdc;
    ctx.resources = *config->static_resources;
    ctx.on_message = config->on_message;
    ctx.user_ctx = config->user_ctx;
    ctx.session_id = session_id;
    ctx.connected = esy_usb_cdc_connected(config->cdc);
    ctx.rx_byte_budget = config->rx_byte_budget != 0u ? config->rx_byte_budget :
                         ESY_PROTO_JSON_USB_DEFAULT_BYTE_BUDGET;
    ctx.tx_byte_budget = config->tx_byte_budget != 0u ? config->tx_byte_budget :
                         ESY_PROTO_JSON_USB_DEFAULT_BYTE_BUDGET;
    memset(channel, 0, sizeof(*channel));
    store(channel, &ctx);
    return ESY_OK;
}

esy_err_t esy_proto_json_usb_channel_send(esy_proto_json_usb_channel_t *channel,
                                         const char *json, size_t length)
{
    json_usb_ctx_t ctx;
    esy_json_value_t root;
    esy_err_t result = load(channel, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.busy && !ctx.callback_active) {
        return ESY_ERR_BUSY;
    }
    bool was_busy = ctx.busy;
    bool was_callback = ctx.callback_active;
    ctx.busy = true;
    ctx.callback_active = false;
    store(channel, &ctx);
    result = observe(&ctx);
    if (result != ESY_OK) {
        goto done;
    }
    if (!ctx.connected) {
        result = ESY_ERR_STATE;
        goto done;
    }
    if (ctx.tx_length != 0u) {
        result = ESY_ERR_BUSY;
        goto done;
    }
    if (!json_channel_range_valid(json, length) || length > UINT32_MAX ||
        json_channel_overlaps(json, length, channel, sizeof(*channel)) ||
        json_channel_overlaps(json, length, ctx.cdc, sizeof(*ctx.cdc)) ||
        json_channel_overlaps(json, length, ctx.resources.tx_buffer, ctx.resources.tx_buffer_size)) {
        result = ESY_ERR_INVALID_ARG;
        goto done;
    }
    if (length > ctx.resources.tx_buffer_size - ESY_PROTO_JSON_USB_HEADER_BYTES) {
        result = ESY_ERR_NO_MEM;
        goto done;
    }
    result = esy_json_parse(json, length, &root);
    if (result == ESY_OK) {
        json_channel_write_length(ctx.resources.tx_buffer, (uint32_t)length);
        memcpy(ctx.resources.tx_buffer + ESY_PROTO_JSON_USB_HEADER_BYTES, json, length);
        ctx.tx_length = length + ESY_PROTO_JSON_USB_HEADER_BYTES;
        ctx.tx_offset = 0u;
    }
done:
    ctx.busy = was_busy;
    ctx.callback_active = was_callback;
    store(channel, &ctx);
    return result;
}

static esy_err_t transmit(json_usb_ctx_t *ctx)
{
    size_t count;
    uint32_t available;
    uint32_t written;
    esy_err_t result;
    if (ctx->tx_length == 0u) {
        return ESY_OK;
    }
    count = ctx->tx_length - ctx->tx_offset;
    if (count > ctx->tx_byte_budget) {
        count = ctx->tx_byte_budget;
    }
    available = esy_usb_cdc_write_available(ctx->cdc);
    result = observe(ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (count > available) {
        count = available;
    }
    if (count == 0u) {
        return ESY_ERR_BUSY;
    }
    written = esy_usb_cdc_write(ctx->cdc, ctx->resources.tx_buffer + ctx->tx_offset,
                                (uint32_t)count);
    result = observe(ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (written > count) {
        return fail(ctx, ESY_ERR_IO);
    }
    if (written == 0u) {
        return ESY_ERR_BUSY;
    }
    ctx->tx_offset += written;
    if (ctx->tx_offset == ctx->tx_length) {
        ctx->tx_offset = 0u;
        ctx->tx_length = 0u;
    }
    esy_usb_cdc_flush(ctx->cdc);
    return observe(ctx);
}

static esy_err_t deliver(esy_proto_json_usb_channel_t *channel, json_usb_ctx_t *ctx)
{
    esy_json_value_t root;
    esy_err_t result = observe(ctx);
    uint32_t session_id = ctx->session_id;
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
    return ctx->session_id == session_id ? ctx->error : ESY_ERR_IO;
}

static esy_err_t receive(esy_proto_json_usb_channel_t *channel, json_usb_ctx_t *ctx)
{
    size_t budget = ctx->rx_byte_budget;
    size_t available = esy_usb_cdc_available(ctx->cdc);
    esy_err_t result = observe(ctx);
    if (result != ESY_OK) {
        return result;
    }
    while (budget != 0u && available != 0u) {
        bool reading_header = ctx->header_used < ESY_PROTO_JSON_USB_HEADER_BYTES;
        uint8_t *destination = reading_header ? ctx->header + ctx->header_used :
                               ctx->resources.rx_buffer + ctx->rx_used;
        size_t count = reading_header ? ESY_PROTO_JSON_USB_HEADER_BYTES - ctx->header_used :
                       (size_t)ctx->rx_expected - ctx->rx_used;
        if (count > budget) {
            count = budget;
        }
        if (count > available) {
            count = available;
        }
        uint32_t received = esy_usb_cdc_read(ctx->cdc, destination, (uint32_t)count);
        result = observe(ctx);
        if (result != ESY_OK) {
            return result;
        }
        if (received > count) {
            return fail(ctx, ESY_ERR_IO);
        }
        if (received == 0u) {
            return ESY_OK;
        }
        budget -= received;
        available -= received;
        if (reading_header) {
            ctx->header_used += received;
            if (ctx->header_used == ESY_PROTO_JSON_USB_HEADER_BYTES) {
                ctx->rx_expected = json_channel_read_length(ctx->header);
                if (ctx->rx_expected == 0u) {
                    return fail(ctx, ESY_ERR_FAIL);
                }
                if (ctx->rx_expected > ctx->resources.rx_buffer_size) {
                    return fail(ctx, ESY_ERR_NO_MEM);
                }
            }
        } else {
            ctx->rx_used += received;
            if (ctx->rx_used == ctx->rx_expected) {
                return deliver(channel, ctx);
            }
        }
    }
    return ESY_OK;
}

esy_err_t esy_proto_json_usb_channel_poll(esy_proto_json_usb_channel_t *channel)
{
    json_usb_ctx_t ctx;
    esy_err_t result = load(channel, &ctx);
    bool pressure = false;
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.busy) {
        return ESY_ERR_BUSY;
    }
    ctx.busy = true;
    store(channel, &ctx);
    result = observe(&ctx);
    if (result != ESY_OK || !ctx.connected) {
        goto done;
    }
    result = transmit(&ctx);
    if (result == ESY_ERR_BUSY) {
        pressure = true;
    } else if (result != ESY_OK) {
        goto done;
    }
    result = receive(channel, &ctx);
    if (result != ESY_OK) {
        goto done;
    }
    result = observe(&ctx);
    if (result == ESY_OK && pressure) {
        result = ESY_ERR_BUSY;
    }
done:
    ctx.busy = false;
    ctx.callback_active = false;
    store(channel, &ctx);
    return result;
}

esy_err_t esy_proto_json_usb_channel_get_status(const esy_proto_json_usb_channel_t *channel,
                                               esy_proto_json_usb_channel_status_t *status)
{
    json_usb_ctx_t ctx;
    esy_err_t result;
    if (status == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    result = load(channel, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    esy_proto_json_usb_channel_status_t snapshot = {
        .session_id = ctx.session_id,
        .rx_received = ctx.rx_used,
        .rx_expected = ctx.rx_expected,
        .tx_remaining = ctx.tx_length - ctx.tx_offset,
        .error = ctx.error,
        .connected = ctx.connected,
        .tx_pending = ctx.tx_length != 0u
    };
    *status = snapshot;
    return ESY_OK;
}

esy_err_t esy_proto_json_usb_channel_deinit(esy_proto_json_usb_channel_t *channel)
{
    json_usb_ctx_t ctx;
    if (channel == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(&ctx, channel->storage.bytes, sizeof(ctx));
    if (ctx.marker == 0u) {
        return ESY_OK;
    }
    if (ctx.marker != JSON_USB_MARKER) {
        return ESY_ERR_STATE;
    }
    if (ctx.busy) {
        return ESY_ERR_BUSY;
    }
    memset(channel, 0, sizeof(*channel));
    return ESY_OK;
}
