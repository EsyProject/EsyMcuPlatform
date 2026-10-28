/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/http/esy_http_client.h"

#include <limits.h>
#include <string.h>
#include "llhttp.h"

#define HTTP_DECIMAL_BUFFER_BYTES 32u

typedef struct {
    bool initialized;
    bool busy;
    bool deadline_active;
    bool response_complete;
    bool transfer_seen;
    bool unsupported_encoding;
    const esy_http_client_t *self;
    esy_http_client_config_t config;
    esy_http_client_static_resources_t resources;
    esy_http_client_status_t status;
    llhttp_t parser;
    llhttp_settings_t settings;
    esy_http_method_t method;
    const uint8_t *request_body;
    size_t request_body_size;
    size_t request_body_sent;
    size_t request_size;
    size_t request_sent;
    size_t field_size;
    size_t value_size;
    size_t protocol_size;
    size_t pending_body_size;
    uint64_t parsed_body_bytes;
    uint64_t started_us;
    uint64_t progressed_us;
    uint32_t header_bytes;
    esy_err_t callback_error;
} esy_http_client_ctx_t;

_Static_assert(sizeof(esy_http_client_ctx_t) <= ESY_HTTP_CLIENT_STORAGE_BYTES,
               "HTTP client storage is too small");
_Static_assert(_Alignof(esy_http_client_ctx_t) <= _Alignof(esy_http_client_t),
               "HTTP client storage is misaligned");

static esy_http_client_ctx_t *client_context(esy_http_client_t *client)
{
    return (esy_http_client_ctx_t *)(void *)client->priv.bytes;
}

static const esy_http_client_ctx_t *const_client_context(const esy_http_client_t *client)
{
    return (const esy_http_client_ctx_t *)(const void *)client->priv.bytes;
}

static bool overlaps(const void *first, size_t first_size,
                     const void *second, size_t second_size)
{
    uintptr_t a = (uintptr_t)first;
    uintptr_t b = (uintptr_t)second;
    if (first_size == 0u || second_size == 0u) {
        return false;
    }
    return a <= b ? b - a < first_size : a - b < second_size;
}

static bool active(const esy_http_client_ctx_t *ctx)
{
    return ctx->status.state == ESY_HTTP_SENDING ||
           ctx->status.state == ESY_HTTP_RECEIVING;
}

static esy_err_t enter(esy_http_client_t *client, esy_http_client_ctx_t **context)
{
    if (client == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_http_client_ctx_t *ctx = client_context(client);
    if (!ctx->initialized || ctx->self != client) {
        return ESY_ERR_STATE;
    }
    if (ctx->busy) {
        return ESY_ERR_BUSY;
    }
    ctx->busy = true;
    *context = ctx;
    return ESY_OK;
}

static esy_err_t terminate(esy_http_client_ctx_t *ctx,
                           esy_http_client_state_t state, esy_err_t error)
{
    ctx->status.state = state;
    ctx->status.error = error;
    ctx->deadline_active = false;
    ctx->request_body = NULL;
    ctx->request_body_size = 0u;
    ctx->pending_body_size = 0u;
    return error;
}

static esy_err_t check_time(esy_http_client_ctx_t *ctx, bool progress)
{
    uint64_t now = 0u;
    esy_err_t result = ctx->config.monotonic_us(ctx->config.clock_context, &now);
    if (result != ESY_OK) {
        return terminate(ctx, ESY_HTTP_FAILED, result);
    }
    if (ctx->deadline_active &&
        ((uint64_t)(now - ctx->started_us) >= ctx->config.total_timeout_us ||
         (uint64_t)(now - ctx->progressed_us) >= ctx->config.idle_timeout_us)) {
        return terminate(ctx, ESY_HTTP_FAILED, ESY_ERR_TIMEOUT);
    }
    if (progress) {
        ctx->progressed_us = now;
    }
    return ESY_OK;
}

static unsigned char ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static bool equals(esy_http_string_t value, const char *literal)
{
    size_t length = strlen(literal);
    if (value.length != length) {
        return false;
    }
    for (size_t i = 0u; i < length; ++i) {
        if (ascii_lower((unsigned char)value.data[i]) != (unsigned char)literal[i]) {
            return false;
        }
    }
    return true;
}

static esy_http_string_t trim(esy_http_string_t value)
{
    while (value.length != 0u && (*value.data == ' ' || *value.data == '\t')) {
        ++value.data;
        --value.length;
    }
    while (value.length != 0u &&
           (value.data[value.length - 1u] == ' ' || value.data[value.length - 1u] == '\t')) {
        --value.length;
    }
    return value;
}

static bool reserved_header(esy_http_string_t name)
{
    return equals(name, "host") || equals(name, "connection") ||
           equals(name, "content-length") || equals(name, "transfer-encoding") ||
           equals(name, "expect") || equals(name, "upgrade") ||
           equals(name, "trailer") || equals(name, "te");
}

static bool valid_token(esy_http_string_t value)
{
    if (value.data == NULL || value.length == 0u) {
        return false;
    }
    for (size_t i = 0u; i < value.length; ++i) {
        unsigned char c = (unsigned char)value.data[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool digit = c >= '0' && c <= '9';
        if (!alpha && !digit && strchr("!#$%&'*+-.^_`|~", c) == NULL) {
            return false;
        }
        if (c == 0u) {
            return false;
        }
    }
    return true;
}

static bool valid_value(esy_http_string_t value)
{
    if (value.data == NULL && value.length != 0u) {
        return false;
    }
    for (size_t i = 0u; i < value.length; ++i) {
        unsigned char c = (unsigned char)value.data[i];
        if ((c < 0x20u && c != '\t') || c == 0x7fu) {
            return false;
        }
    }
    return true;
}

static bool valid_target(esy_http_string_t value, bool host)
{
    if (value.data == NULL || value.length == 0u || (!host && value.data[0] != '/')) {
        return false;
    }
    for (size_t i = 0u; i < value.length; ++i) {
        unsigned char c = (unsigned char)value.data[i];
        if (c <= 0x20u || c >= 0x7fu || c == '#' || c == '\\' ||
            (host && (c == '/' || c == '?' || c == '@'))) {
            return false;
        }
    }
    return true;
}

static const char *method_name(esy_http_method_t method)
{
    switch (method) {
    case ESY_HTTP_GET: return "GET";
    case ESY_HTTP_HEAD: return "HEAD";
    case ESY_HTTP_POST: return "POST";
    case ESY_HTTP_PUT: return "PUT";
    case ESY_HTTP_DELETE: return "DELETE";
    case ESY_HTTP_PATCH: return "PATCH";
    default: return NULL;
    }
}

static bool aliases_workspace(const esy_http_client_ctx_t *ctx, const void *data, size_t size)
{
    const esy_http_client_static_resources_t *r = &ctx->resources;
    return overlaps(data, size, ctx->self, sizeof(*ctx->self)) ||
           overlaps(data, size, r->request_buffer, r->request_buffer_size) ||
           overlaps(data, size, r->receive_buffer, r->receive_buffer_size) ||
           overlaps(data, size, r->body_buffer, r->body_buffer_size) ||
           overlaps(data, size, r->header_buffer, r->header_buffer_size);
}

static esy_err_t validate_request(const esy_http_client_ctx_t *ctx,
                                  const esy_http_request_t *request)
{
    if (request == NULL || method_name(request->method) == NULL ||
        !valid_target(request->host, true) || !valid_target(request->target, false) ||
        (request->body == NULL && request->body_size != 0u) ||
        (request->headers == NULL && request->header_count != 0u) ||
        request->header_count > SIZE_MAX / sizeof(*request->headers) ||
        aliases_workspace(ctx, request, sizeof(*request)) ||
        aliases_workspace(ctx, request->host.data, request->host.length) ||
        aliases_workspace(ctx, request->target.data, request->target.length) ||
        aliases_workspace(ctx, request->headers, request->header_count * sizeof(*request->headers)) ||
        aliases_workspace(ctx, request->body, request->body_size)) {
        return ESY_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < request->header_count; ++i) {
        const esy_http_request_header_t *header = &request->headers[i];
        if (!valid_token(header->name) || !valid_value(header->value) ||
            reserved_header(header->name) ||
            aliases_workspace(ctx, header->name.data, header->name.length) ||
            aliases_workspace(ctx, header->value.data, header->value.length)) {
            return ESY_ERR_INVALID_ARG;
        }
    }
    return ESY_OK;
}

static bool append(esy_http_client_ctx_t *ctx, const void *data, size_t size)
{
    if (size > ctx->resources.request_buffer_size - ctx->request_size) {
        return false;
    }
    if (size != 0u) {
        memcpy(ctx->resources.request_buffer + ctx->request_size, data, size);
        ctx->request_size += size;
    }
    return true;
}

static bool append_literal(esy_http_client_ctx_t *ctx, const char *literal)
{
    return append(ctx, literal, strlen(literal));
}

static bool append_decimal(esy_http_client_ctx_t *ctx, size_t value)
{
    char buffer[HTTP_DECIMAL_BUFFER_BYTES];
    size_t offset = sizeof(buffer);
    do {
        buffer[--offset] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    return append(ctx, buffer + offset, sizeof(buffer) - offset);
}

static esy_err_t encode_request(esy_http_client_ctx_t *ctx, const esy_http_request_t *request)
{
    ctx->request_size = 0u;
    if (!append_literal(ctx, method_name(request->method)) ||
        !append_literal(ctx, " ") || !append(ctx, request->target.data, request->target.length) ||
        !append_literal(ctx, " HTTP/1.1\r\nHost: ") ||
        !append(ctx, request->host.data, request->host.length) ||
        !append_literal(ctx, "\r\nConnection: close\r\nContent-Length: ") ||
        !append_decimal(ctx, request->body_size) || !append_literal(ctx, "\r\n")) {
        return ESY_ERR_NO_MEM;
    }
    for (size_t i = 0u; i < request->header_count; ++i) {
        const esy_http_request_header_t *header = &request->headers[i];
        if (!append(ctx, header->name.data, header->name.length) ||
            !append_literal(ctx, ": ") || !append(ctx, header->value.data, header->value.length) ||
            !append_literal(ctx, "\r\n")) {
            return ESY_ERR_NO_MEM;
        }
    }
    return append_literal(ctx, "\r\n") ? ESY_OK : ESY_ERR_NO_MEM;
}

static int parser_failure(esy_http_client_ctx_t *ctx, esy_err_t error)
{
    ctx->callback_error = error;
    return HPE_USER;
}

static bool account_header(esy_http_client_ctx_t *ctx, size_t size)
{
    if (size > ctx->config.max_header_bytes - ctx->header_bytes) {
        ctx->callback_error = ESY_ERR_NO_MEM;
        return false;
    }
    ctx->header_bytes += (uint32_t)size;
    return true;
}

static int on_message_begin(llhttp_t *parser)
{
    esy_http_client_ctx_t *ctx = parser->data;
    ctx->field_size = 0u;
    ctx->value_size = 0u;
    ctx->protocol_size = 0u;
    ctx->transfer_seen = false;
    ctx->unsupported_encoding = false;
    return account_header(ctx, 1u) ? HPE_OK : HPE_USER;
}

static int on_protocol(llhttp_t *parser, const char *data, size_t size)
{
    esy_http_client_ctx_t *ctx = parser->data;
    static const char k_protocol[] = "HTTP";
    if (size > sizeof(k_protocol) - 1u - ctx->protocol_size ||
        memcmp(data, k_protocol + ctx->protocol_size, size) != 0) {
        return parser_failure(ctx, ESY_ERR_NOT_SUPPORTED);
    }
    ctx->protocol_size += size;
    return HPE_OK;
}

static int on_protocol_complete(llhttp_t *parser)
{
    esy_http_client_ctx_t *ctx = parser->data;
    return ctx->protocol_size == sizeof("HTTP") - 1u ? HPE_OK :
           parser_failure(ctx, ESY_ERR_NOT_SUPPORTED);
}

static int on_status(llhttp_t *parser, const char *data, size_t size)
{
    esy_http_client_ctx_t *ctx = parser->data;
    (void)data;
    return account_header(ctx, size) ? HPE_OK : HPE_USER;
}

static int on_header_field(llhttp_t *parser, const char *data, size_t size)
{
    esy_http_client_ctx_t *ctx = parser->data;
    if (!account_header(ctx, size) ||
        size > ctx->resources.header_buffer_size - ctx->field_size - ctx->value_size) {
        return parser_failure(ctx, ESY_ERR_NO_MEM);
    }
    memcpy(ctx->resources.header_buffer + ctx->field_size, data, size);
    ctx->field_size += size;
    return HPE_OK;
}

static int on_header_value(llhttp_t *parser, const char *data, size_t size)
{
    esy_http_client_ctx_t *ctx = parser->data;
    if (!account_header(ctx, size) ||
        size > ctx->resources.header_buffer_size - ctx->field_size - ctx->value_size) {
        return parser_failure(ctx, ESY_ERR_NO_MEM);
    }
    memcpy(ctx->resources.header_buffer + ctx->field_size + ctx->value_size, data, size);
    ctx->value_size += size;
    return HPE_OK;
}

static int on_header_complete(llhttp_t *parser)
{
    esy_http_client_ctx_t *ctx = parser->data;
    esy_http_response_header_t header = {
        {(const char *)ctx->resources.header_buffer, ctx->field_size},
        {(const char *)ctx->resources.header_buffer + ctx->field_size, ctx->value_size},
        (uint16_t)llhttp_get_status_code(parser),
        (parser->flags & F_TRAILING) != 0u
    };
    if (!account_header(ctx, 1u)) {
        return HPE_USER;
    }
    if (header.trailer && (reserved_header(header.name) || equals(header.name, "content-encoding"))) {
        return parser_failure(ctx, ESY_ERR_INVALID_ARG);
    }
    if (!header.trailer && equals(header.name, "transfer-encoding")) {
        if (ctx->transfer_seen || !equals(trim(header.value), "chunked")) {
            return parser_failure(ctx, ESY_ERR_NOT_SUPPORTED);
        }
        ctx->transfer_seen = true;
    }
    if (!header.trailer && equals(header.name, "content-encoding") &&
        !equals(trim(header.value), "identity")) {
        ctx->unsupported_encoding = true;
    }
    if (ctx->config.header_cb != NULL) {
        esy_err_t result = ctx->config.header_cb(ctx->config.callback_context, &header);
        if (result != ESY_OK) {
            return parser_failure(ctx, result);
        }
    }
    ctx->field_size = 0u;
    ctx->value_size = 0u;
    return HPE_OK;
}

static int on_headers_complete(llhttp_t *parser)
{
    esy_http_client_ctx_t *ctx = parser->data;
    int code = llhttp_get_status_code(parser);
    bool bodyless = ctx->method == ESY_HTTP_HEAD || code < 200 || code == 204 || code == 304;
    if (llhttp_get_http_major(parser) != 1u || llhttp_get_http_minor(parser) > 1u ||
        code < 100 || code > 599 || code == 101 || llhttp_get_upgrade(parser) != 0u) {
        return parser_failure(ctx, ESY_ERR_NOT_SUPPORTED);
    }
    if (llhttp_get_http_minor(parser) == 0u && ctx->transfer_seen) {
        return parser_failure(ctx, ESY_ERR_INVALID_ARG);
    }
    if (code >= 200) {
        ctx->status.status_code = (uint16_t)code;
    }
    if (!bodyless && (ctx->unsupported_encoding || llhttp_message_needs_eof(parser))) {
        return parser_failure(ctx, ESY_ERR_NOT_SUPPORTED);
    }
    if (!bodyless && (parser->flags & F_CONTENT_LENGTH) != 0u &&
        parser->content_length > ctx->config.max_body_bytes) {
        return parser_failure(ctx, ESY_ERR_NO_MEM);
    }
    return bodyless ? 1 : HPE_OK;
}

static int on_body(llhttp_t *parser, const char *data, size_t size)
{
    esy_http_client_ctx_t *ctx = parser->data;
    if (size > ctx->resources.body_buffer_size - ctx->pending_body_size ||
        size > ctx->config.max_body_bytes - ctx->parsed_body_bytes) {
        return parser_failure(ctx, ESY_ERR_NO_MEM);
    }
    memcpy(ctx->resources.body_buffer + ctx->pending_body_size, data, size);
    ctx->pending_body_size += size;
    ctx->parsed_body_bytes += size;
    return HPE_OK;
}

static int on_message_complete(llhttp_t *parser)
{
    esy_http_client_ctx_t *ctx = parser->data;
    if (llhttp_get_status_code(parser) < 200) {
        return HPE_OK;
    }
    ctx->response_complete = true;
    return HPE_PAUSED;
}

static void setup_parser(esy_http_client_ctx_t *ctx)
{
    llhttp_settings_init(&ctx->settings);
    ctx->settings.on_message_begin = on_message_begin;
    ctx->settings.on_protocol = on_protocol;
    ctx->settings.on_protocol_complete = on_protocol_complete;
    ctx->settings.on_status = on_status;
    ctx->settings.on_header_field = on_header_field;
    ctx->settings.on_header_value = on_header_value;
    ctx->settings.on_header_value_complete = on_header_complete;
    ctx->settings.on_headers_complete = on_headers_complete;
    ctx->settings.on_body = on_body;
    ctx->settings.on_message_complete = on_message_complete;
    llhttp_init(&ctx->parser, HTTP_RESPONSE, &ctx->settings);
    ctx->parser.data = ctx;
}

static bool valid_resources(const esy_http_client_t *client,
                             const esy_http_client_static_resources_t *resources)
{
    const void *buffers[] = {resources->request_buffer, resources->receive_buffer,
                             resources->body_buffer, resources->header_buffer};
    const size_t sizes[] = {resources->request_buffer_size, resources->receive_buffer_size,
                            resources->body_buffer_size, resources->header_buffer_size};
    if (resources->header_buffer_size < 2u ||
        resources->body_buffer_size < resources->receive_buffer_size) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(buffers) / sizeof(buffers[0]); ++i) {
        if (buffers[i] == NULL || sizes[i] == 0u || sizes[i] > INT32_MAX ||
            overlaps(buffers[i], sizes[i], client, sizeof(*client))) {
            return false;
        }
        for (size_t j = 0u; j < i; ++j) {
            if (overlaps(buffers[i], sizes[i], buffers[j], sizes[j])) {
                return false;
            }
        }
    }
    return true;
}

esy_err_t esy_http_client_init(esy_http_client_t *client,
                               const esy_http_client_config_t *config)
{
    if (client == NULL || config == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_http_client_ctx_t *ctx = client_context(client);
    if (ctx->initialized) {
        return ctx->busy ? ESY_ERR_BUSY : ESY_ERR_STATE;
    }
    if (config->transport.send == NULL || config->transport.recv == NULL ||
        config->monotonic_us == NULL || config->body_cb == NULL ||
        config->total_timeout_us == 0u || config->idle_timeout_us == 0u ||
        config->io_budget_bytes == 0u || config->max_header_bytes == 0u ||
        config->max_body_bytes == 0u || config->static_resources == NULL ||
        overlaps(client, sizeof(*client), config, sizeof(*config)) ||
        !valid_resources(client, config->static_resources)) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_http_client_config_t copied_config = *config;
    esy_http_client_static_resources_t copied_resources = *config->static_resources;
    memset(client, 0, sizeof(*client));
    ctx->config = copied_config;
    ctx->config.static_resources = NULL;
    ctx->resources = copied_resources;
    ctx->self = client;
    ctx->initialized = true;
    setup_parser(ctx);
    return ESY_OK;
}

esy_err_t esy_http_client_submit(esy_http_client_t *client,
                                 const esy_http_request_t *request)
{
    esy_http_client_ctx_t *ctx = NULL;
    esy_err_t result = enter(client, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx->status.state != ESY_HTTP_IDLE) {
        result = ESY_ERR_STATE;
    } else {
        result = validate_request(ctx, request);
        if (result == ESY_OK) {
            result = encode_request(ctx, request);
        }
        uint64_t now = 0u;
        if (result == ESY_OK) {
            result = ctx->config.monotonic_us(ctx->config.clock_context, &now);
        }
        if (result == ESY_OK) {
            ctx->request_body = request->body;
            ctx->request_body_size = request->body_size;
            ctx->method = request->method;
            ctx->started_us = now;
            ctx->progressed_us = now;
            ctx->deadline_active = true;
            ctx->status.state = ESY_HTTP_SENDING;
        }
    }
    ctx->busy = false;
    return result;
}

static esy_err_t deliver_body(esy_http_client_ctx_t *ctx)
{
    size_t size = ctx->pending_body_size;
    esy_err_t result = ctx->config.body_cb(ctx->config.callback_context,
                                          ctx->resources.body_buffer, size);
    if (result == ESY_OK) {
        /* Consumption can have effects even if its synchronous work expires a wait. */
        ctx->status.body_bytes += size;
        ctx->pending_body_size = 0u;
    }
    esy_err_t timing = check_time(ctx, result == ESY_OK);
    if (timing != ESY_OK) {
        return timing;
    }
    if (result == ESY_ERR_BUSY) {
        return ESY_OK;
    }
    if (result != ESY_OK) {
        return terminate(ctx, ESY_HTTP_FAILED, result);
    }
    if (ctx->response_complete) {
        return terminate(ctx, ESY_HTTP_COMPLETE, ESY_OK);
    }
    return ESY_OK;
}

static esy_err_t send_step(esy_http_client_ctx_t *ctx)
{
    bool header = ctx->request_sent < ctx->request_size;
    const uint8_t *data = header ? ctx->resources.request_buffer + ctx->request_sent :
                                  ctx->request_body + ctx->request_body_sent;
    size_t size = header ? ctx->request_size - ctx->request_sent :
                          ctx->request_body_size - ctx->request_body_sent;
    if (size > ctx->config.io_budget_bytes) {
        size = ctx->config.io_budget_bytes;
    }
    if (size > INT32_MAX) {
        size = INT32_MAX;
    }
    int32_t sent = ctx->config.transport.send(ctx->config.transport.context, data, size);
    esy_err_t timing = check_time(ctx, sent > 0);
    if (timing != ESY_OK) {
        return timing;
    }
    if (sent < 0 || (size_t)sent > size) {
        return terminate(ctx, ESY_HTTP_FAILED, sent < 0 ? sent : ESY_ERR_IO);
    }
    if (header) {
        ctx->request_sent += (size_t)sent;
    } else {
        ctx->request_body_sent += (size_t)sent;
    }
    if (ctx->request_sent == ctx->request_size && ctx->request_body_sent == ctx->request_body_size) {
        ctx->status.state = ESY_HTTP_RECEIVING;
    }
    return ESY_OK;
}

static esy_err_t receive_step(esy_http_client_ctx_t *ctx)
{
    size_t size = ctx->resources.receive_buffer_size;
    if (size > ctx->config.io_budget_bytes) {
        size = ctx->config.io_budget_bytes;
    }
    int32_t received = ctx->config.transport.recv(ctx->config.transport.context,
                                                 ctx->resources.receive_buffer, size);
    esy_err_t timing = check_time(ctx, false);
    if (timing != ESY_OK) {
        return timing;
    }
    if (received < 0 || (size_t)received > size) {
        return terminate(ctx, ESY_HTTP_FAILED, received < 0 ? received : ESY_ERR_IO);
    }
    if (received == 0) {
        return ESY_OK;
    }
    const char *input = (const char *)ctx->resources.receive_buffer;
    llhttp_errno_t parsed = llhttp_execute(&ctx->parser, input, (size_t)received);
    timing = check_time(ctx, true);
    if (timing != ESY_OK) {
        return timing;
    }
    if (parsed != HPE_OK && !(parsed == HPE_PAUSED && ctx->response_complete)) {
        ctx->status.parser_error = (int)parsed;
        return terminate(ctx, ESY_HTTP_FAILED,
                         ctx->callback_error != ESY_OK ? ctx->callback_error : ESY_ERR_INVALID_ARG);
    }
    if (ctx->response_complete && llhttp_get_error_pos(&ctx->parser) != input + received) {
        return terminate(ctx, ESY_HTTP_FAILED, ESY_ERR_INVALID_ARG);
    }
    if (ctx->pending_body_size != 0u) {
        return deliver_body(ctx);
    }
    if (ctx->response_complete) {
        return terminate(ctx, ESY_HTTP_COMPLETE, ESY_OK);
    }
    return ESY_OK;
}

esy_err_t esy_http_client_process(esy_http_client_t *client)
{
    esy_http_client_ctx_t *ctx = NULL;
    esy_err_t result = enter(client, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (!active(ctx)) {
        result = ctx->status.state == ESY_HTTP_COMPLETE ? ESY_OK :
                 ctx->status.state == ESY_HTTP_FAILED ? ctx->status.error : ESY_ERR_STATE;
    } else {
        result = check_time(ctx, false);
        if (result == ESY_OK && ctx->pending_body_size != 0u) {
            result = deliver_body(ctx);
        } else if (result == ESY_OK) {
            if (ctx->config.transport.progress != NULL) {
                result = ctx->config.transport.progress(ctx->config.transport.context);
                if (result != ESY_OK) {
                    result = terminate(ctx, ESY_HTTP_FAILED, result);
                }
            }
            if (result == ESY_OK) {
                result = check_time(ctx, false);
            }
            if (result == ESY_OK) {
                result = ctx->status.state == ESY_HTTP_SENDING ? send_step(ctx) : receive_step(ctx);
            }
        }
    }
    ctx->busy = false;
    return result;
}

esy_err_t esy_http_client_get_status(const esy_http_client_t *client,
                                     esy_http_client_status_t *status)
{
    if (client == NULL || status == NULL || overlaps(client, sizeof(*client), status, sizeof(*status))) {
        return ESY_ERR_INVALID_ARG;
    }
    const esy_http_client_ctx_t *ctx = const_client_context(client);
    if (!ctx->initialized || ctx->self != client) {
        return ESY_ERR_STATE;
    }
    if (aliases_workspace(ctx, status, sizeof(*status))) {
        return ESY_ERR_INVALID_ARG;
    }
    *status = ctx->status;
    return ESY_OK;
}

esy_err_t esy_http_client_cancel(esy_http_client_t *client)
{
    esy_http_client_ctx_t *ctx = NULL;
    esy_err_t result = enter(client, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (active(ctx) || ctx->status.state == ESY_HTTP_IDLE) {
        (void)terminate(ctx, ESY_HTTP_CANCELLED, ESY_ERR_STATE);
    }
    ctx->busy = false;
    return ESY_OK;
}

esy_err_t esy_http_client_deinit(esy_http_client_t *client)
{
    if (client == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_http_client_ctx_t *ctx = client_context(client);
    if (!ctx->initialized) {
        return ESY_OK;
    }
    if (ctx->self != client) {
        return ESY_ERR_STATE;
    }
    if (ctx->busy) {
        return ESY_ERR_BUSY;
    }
    if (active(ctx)) {
        return ESY_ERR_STATE;
    }
    memset(client, 0, sizeof(*client));
    return ESY_OK;
}
