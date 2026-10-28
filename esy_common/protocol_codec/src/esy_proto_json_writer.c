/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/protocol/esy_proto_json.h"

#include <string.h>

#define JSON_WRITER_MAGIC UINT32_C(0x4a575254)
#define JSON_INTEGER_TEXT_CAPACITY 21u
#define JSON_DECIMAL_RADIX UINT64_C(10)
#define JSON_UNICODE_ESCAPE_BYTES 6u

typedef enum {
    JSON_ARRAY_EMPTY = 1,
    JSON_ARRAY_NEXT_VALUE,
    JSON_OBJECT_EMPTY,
    JSON_OBJECT_NEXT_KEY,
    JSON_OBJECT_VALUE
} json_frame_state_t;

typedef struct {
    uint32_t magic;
    esy_json_writer_resources_t resources;
    size_t length;
    size_t depth;
    esy_err_t error;
    bool root_started;
    bool finished;
} esy_json_writer_ctx_t;

_Static_assert(sizeof(esy_json_writer_ctx_t) <= ESY_JSON_WRITER_STORAGE_BYTES,
               "JSON writer context exceeds its public storage");

static void store_context(esy_json_writer_t *writer,
                          const esy_json_writer_ctx_t *ctx)
{
    memcpy(writer->storage.bytes, ctx, sizeof(*ctx));
}

static esy_err_t load_context(const esy_json_writer_t *writer,
                              esy_json_writer_ctx_t *ctx)
{
    if (writer == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(ctx, writer->storage.bytes, sizeof(*ctx));
    if (ctx->magic != JSON_WRITER_MAGIC) {
        return ESY_ERR_STATE;
    }
    return ESY_OK;
}

static esy_err_t load_writable(const esy_json_writer_t *writer,
                               esy_json_writer_ctx_t *ctx)
{
    esy_err_t result = load_context(writer, ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx->error != ESY_OK) {
        return ctx->error;
    }
    if (ctx->finished) {
        return ESY_ERR_STATE;
    }
    return ESY_OK;
}

static esy_err_t complete_write(esy_json_writer_t *writer,
                                esy_json_writer_ctx_t *ctx, esy_err_t result)
{
    if (result != ESY_OK) {
        ctx->error = result;
    }
    store_context(writer, ctx);
    return result;
}

static esy_err_t append_bytes(esy_json_writer_ctx_t *ctx,
                              const char *bytes, size_t length)
{
    size_t remaining = ctx->resources.buffer_capacity - ctx->length - 1u;
    if (length > remaining) {
        return ESY_ERR_NO_MEM;
    }
    if (length != 0u) {
        memcpy(ctx->resources.buffer + ctx->length, bytes, length);
        ctx->length += length;
    }
    ctx->resources.buffer[ctx->length] = '\0';
    return ESY_OK;
}

static esy_err_t append_character(esy_json_writer_ctx_t *ctx, char value)
{
    return append_bytes(ctx, &value, 1u);
}

static bool is_continuation(uint8_t value)
{
    return value >= 0x80u && value <= 0xbfu;
}

static bool valid_utf8(const char *text, size_t length)
{
    size_t offset = 0u;
    if (text == NULL && length != 0u) {
        return false;
    }
    while (offset < length) {
        uint8_t first = (uint8_t)text[offset++];
        if (first <= 0x7fu) {
            continue;
        }
        size_t trailing;
        uint8_t second_min = 0x80u;
        uint8_t second_max = 0xbfu;
        if (first >= 0xc2u && first <= 0xdfu) {
            trailing = 1u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            trailing = 2u;
            if (first == 0xe0u) {
                second_min = 0xa0u;
            } else if (first == 0xedu) {
                second_max = 0x9fu;
            }
        } else if (first >= 0xf0u && first <= 0xf4u) {
            trailing = 3u;
            if (first == 0xf0u) {
                second_min = 0x90u;
            } else if (first == 0xf4u) {
                second_max = 0x8fu;
            }
        } else {
            return false;
        }
        if (trailing > length - offset) {
            return false;
        }
        uint8_t second = (uint8_t)text[offset];
        if (second < second_min || second > second_max) {
            return false;
        }
        for (size_t index = 1u; index < trailing; ++index) {
            if (!is_continuation((uint8_t)text[offset + index])) {
                return false;
            }
        }
        offset += trailing;
    }
    return true;
}

static esy_err_t append_quoted(esy_json_writer_ctx_t *ctx,
                               const char *text, size_t length)
{
    static const char k_hex_digits[] = "0123456789abcdef";
    esy_err_t result = append_character(ctx, '"');
    for (size_t offset = 0u; result == ESY_OK && offset < length; ++offset) {
        uint8_t value = (uint8_t)text[offset];
        if (value == (uint8_t)'"' || value == (uint8_t)'\\') {
            char escaped[2] = {'\\', (char)value};
            result = append_bytes(ctx, escaped, sizeof(escaped));
        } else if (value < 0x20u) {
            char escaped[JSON_UNICODE_ESCAPE_BYTES] = {
                '\\', 'u', '0', '0',
                k_hex_digits[value >> 4u], k_hex_digits[value & 0x0fu]
            };
            result = append_bytes(ctx, escaped, sizeof(escaped));
        } else {
            result = append_character(ctx, (char)value);
        }
    }
    if (result == ESY_OK) {
        result = append_character(ctx, '"');
    }
    return result;
}

static esy_err_t prepare_value(esy_json_writer_ctx_t *ctx)
{
    if (ctx->depth == 0u) {
        if (ctx->root_started) {
            return ESY_ERR_STATE;
        }
        ctx->root_started = true;
        return ESY_OK;
    }
    uint8_t *frame = &ctx->resources.frame_storage[ctx->depth - 1u];
    switch (*frame) {
    case JSON_ARRAY_EMPTY:
        *frame = JSON_ARRAY_NEXT_VALUE;
        return ESY_OK;
    case JSON_ARRAY_NEXT_VALUE:
        return append_character(ctx, ',');
    case JSON_OBJECT_VALUE:
        *frame = JSON_OBJECT_NEXT_KEY;
        return ESY_OK;
    default:
        return ESY_ERR_STATE;
    }
}

static esy_err_t append_value(esy_json_writer_ctx_t *ctx,
                              const char *bytes, size_t length)
{
    esy_err_t result = prepare_value(ctx);
    if (result == ESY_OK) {
        result = append_bytes(ctx, bytes, length);
    }
    return result;
}

static esy_err_t begin_container(esy_json_writer_t *writer,
                                 json_frame_state_t frame, char delimiter)
{
    esy_json_writer_ctx_t ctx;
    esy_err_t result = load_writable(writer, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.depth >= ctx.resources.frame_capacity) {
        return complete_write(writer, &ctx, ESY_ERR_NO_MEM);
    }
    result = prepare_value(&ctx);
    if (result == ESY_OK) {
        result = append_character(&ctx, delimiter);
    }
    if (result == ESY_OK) {
        ctx.resources.frame_storage[ctx.depth++] = (uint8_t)frame;
    }
    return complete_write(writer, &ctx, result);
}

static esy_err_t write_literal(esy_json_writer_t *writer,
                               const char *text, size_t length)
{
    esy_json_writer_ctx_t ctx;
    esy_err_t result = load_writable(writer, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    result = append_value(&ctx, text, length);
    return complete_write(writer, &ctx, result);
}

static esy_err_t write_integer(esy_json_writer_t *writer,
                               uint64_t magnitude, bool negative)
{
    char number[JSON_INTEGER_TEXT_CAPACITY];
    size_t offset = sizeof(number);
    do {
        number[--offset] = (char)('0' + magnitude % JSON_DECIMAL_RADIX);
        magnitude /= JSON_DECIMAL_RADIX;
    } while (magnitude != 0u);
    if (negative) {
        number[--offset] = '-';
    }
    return write_literal(writer, number + offset, sizeof(number) - offset);
}

esy_err_t esy_json_writer_init(esy_json_writer_t *writer,
                               const esy_json_writer_resources_t *resources)
{
    if (writer == NULL || resources == NULL || resources->buffer == NULL ||
        resources->buffer_capacity == 0u || resources->frame_storage == NULL ||
        resources->frame_capacity == 0u) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_json_writer_ctx_t ctx = {0};
    ctx.magic = JSON_WRITER_MAGIC;
    ctx.resources = *resources;
    memset(writer, 0, sizeof(*writer));
    ctx.resources.buffer[0] = '\0';
    store_context(writer, &ctx);
    return ESY_OK;
}

esy_err_t esy_json_writer_begin_object(esy_json_writer_t *writer)
{
    return begin_container(writer, JSON_OBJECT_EMPTY, '{');
}

esy_err_t esy_json_writer_begin_array(esy_json_writer_t *writer)
{
    return begin_container(writer, JSON_ARRAY_EMPTY, '[');
}

esy_err_t esy_json_writer_end(esy_json_writer_t *writer)
{
    esy_json_writer_ctx_t ctx;
    esy_err_t result = load_writable(writer, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.depth == 0u) {
        return complete_write(writer, &ctx, ESY_ERR_STATE);
    }
    uint8_t frame = ctx.resources.frame_storage[ctx.depth - 1u];
    char delimiter;
    if (frame == JSON_ARRAY_EMPTY || frame == JSON_ARRAY_NEXT_VALUE) {
        delimiter = ']';
    } else if (frame == JSON_OBJECT_EMPTY || frame == JSON_OBJECT_NEXT_KEY) {
        delimiter = '}';
    } else {
        return complete_write(writer, &ctx, ESY_ERR_STATE);
    }
    result = append_character(&ctx, delimiter);
    if (result == ESY_OK) {
        --ctx.depth;
    }
    return complete_write(writer, &ctx, result);
}

esy_err_t esy_json_writer_key(esy_json_writer_t *writer,
                              const char *key, size_t length)
{
    esy_json_writer_ctx_t ctx;
    esy_err_t result = load_writable(writer, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (!valid_utf8(key, length)) {
        return complete_write(writer, &ctx, ESY_ERR_INVALID_ARG);
    }
    if (ctx.depth == 0u) {
        return complete_write(writer, &ctx, ESY_ERR_STATE);
    }
    uint8_t *frame = &ctx.resources.frame_storage[ctx.depth - 1u];
    if (*frame != JSON_OBJECT_EMPTY && *frame != JSON_OBJECT_NEXT_KEY) {
        return complete_write(writer, &ctx, ESY_ERR_STATE);
    }
    if (*frame == JSON_OBJECT_NEXT_KEY) {
        result = append_character(&ctx, ',');
    }
    if (result == ESY_OK) {
        result = append_quoted(&ctx, key, length);
    }
    if (result == ESY_OK) {
        result = append_character(&ctx, ':');
    }
    if (result == ESY_OK) {
        *frame = JSON_OBJECT_VALUE;
    }
    return complete_write(writer, &ctx, result);
}

esy_err_t esy_json_writer_string(esy_json_writer_t *writer,
                                 const char *text, size_t length)
{
    esy_json_writer_ctx_t ctx;
    esy_err_t result = load_writable(writer, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (!valid_utf8(text, length)) {
        return complete_write(writer, &ctx, ESY_ERR_INVALID_ARG);
    }
    result = prepare_value(&ctx);
    if (result == ESY_OK) {
        result = append_quoted(&ctx, text, length);
    }
    return complete_write(writer, &ctx, result);
}

esy_err_t esy_json_writer_u64(esy_json_writer_t *writer, uint64_t value)
{
    return write_integer(writer, value, false);
}

esy_err_t esy_json_writer_i64(esy_json_writer_t *writer, int64_t value)
{
    bool negative = value < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    return write_integer(writer, magnitude, negative);
}

esy_err_t esy_json_writer_bool(esy_json_writer_t *writer, bool value)
{
    return value ? write_literal(writer, "true", sizeof("true") - 1u)
                 : write_literal(writer, "false", sizeof("false") - 1u);
}

esy_err_t esy_json_writer_null(esy_json_writer_t *writer)
{
    return write_literal(writer, "null", sizeof("null") - 1u);
}

esy_err_t esy_json_writer_number(esy_json_writer_t *writer,
                                 const char *number, size_t length)
{
    esy_json_writer_ctx_t ctx;
    esy_err_t result = load_writable(writer, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (number == NULL || length == 0u) {
        return complete_write(writer, &ctx, ESY_ERR_INVALID_ARG);
    }
    esy_json_value_t value;
    result = esy_json_parse(number, length, &value);
    if (result == ESY_OK && (value.type != ESY_JSON_NUMBER ||
                            value.data != number || value.length != length)) {
        result = ESY_ERR_FAIL;
    }
    if (result == ESY_OK) {
        result = append_value(&ctx, number, length);
    }
    return complete_write(writer, &ctx, result);
}

esy_err_t esy_json_writer_finish(esy_json_writer_t *writer,
                                 const char **buffer, size_t *length)
{
    if (buffer == NULL || length == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_json_writer_ctx_t ctx;
    esy_err_t result = load_context(writer, &ctx);
    if (result != ESY_OK) {
        return result;
    }
    if (ctx.error != ESY_OK) {
        return ctx.error;
    }
    if (!ctx.root_started || ctx.depth != 0u) {
        return ESY_ERR_STATE;
    }
    ctx.finished = true;
    store_context(writer, &ctx);
    *buffer = ctx.resources.buffer;
    *length = ctx.length;
    return ESY_OK;
}
