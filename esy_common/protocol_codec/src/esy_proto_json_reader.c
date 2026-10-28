/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/protocol/esy_proto_json.h"

#include <limits.h>
#include <string.h>

#include "core_json.h"

#define JSON_UTF8_MAX_BYTES 4u
#define JSON_HEX_DIGITS 4u
#define JSON_SURROGATE_HIGH_MIN 0xd800u
#define JSON_SURROGATE_HIGH_MAX 0xdbffu
#define JSON_SURROGATE_LOW_MIN 0xdc00u
#define JSON_SURROGATE_LOW_MAX 0xdfffu
#define JSON_SUPPLEMENTARY_MIN 0x10000u
#define JSON_UNICODE_MAX 0x10ffffu

enum { JSON_ITERATOR_MARKER = 0x4a534954 };

typedef struct {
    uint32_t marker;
    esy_json_value_t collection;
    size_t start;
    size_t next;
    bool exhausted;
} json_iterator_ctx_t;

_Static_assert(sizeof(json_iterator_ctx_t) <= ESY_JSON_ITERATOR_STORAGE_BYTES,
               "JSON iterator storage is too small");

static bool json_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static esy_json_type_t classify(char c)
{
    switch (c) {
    case 'n': return ESY_JSON_NULL;
    case 't':
    case 'f': return ESY_JSON_BOOL;
    case '"': return ESY_JSON_STRING;
    case '[': return ESY_JSON_ARRAY;
    case '{': return ESY_JSON_OBJECT;
    default: return ESY_JSON_NUMBER;
    }
}

static esy_err_t provider_status(JSONStatus_t status)
{
    switch (status) {
    case JSONSuccess: return ESY_OK;
    case JSONMaxDepthExceeded: return ESY_ERR_NO_MEM;
    case JSONNotFound: return ESY_ERR_NOT_SUPPORTED;
    default: return ESY_ERR_FAIL;
    }
}

static bool valid_view(const esy_json_value_t *value, esy_json_type_t type)
{
    return value != NULL && value->data != NULL && value->length != 0u &&
           value->type == type;
}

static bool utf8_valid(const char *text, size_t length)
{
    size_t offset = 0u;
    while (offset < length) {
        unsigned char first = (unsigned char)text[offset++];
        uint32_t codepoint;
        uint32_t minimum;
        size_t remaining;
        if (first < 0x80u) {
            continue;
        }
        if (first >= 0xc2u && first <= 0xdfu) {
            codepoint = first & 0x1fu;
            minimum = 0x80u;
            remaining = 1u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            codepoint = first & 0x0fu;
            minimum = 0x800u;
            remaining = 2u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            codepoint = first & 0x07u;
            minimum = JSON_SUPPLEMENTARY_MIN;
            remaining = 3u;
        } else {
            return false;
        }
        if (remaining > length - offset) {
            return false;
        }
        while (remaining-- != 0u) {
            unsigned char next = (unsigned char)text[offset++];
            if (next < 0x80u || next > 0xbfu) {
                return false;
            }
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > JSON_UNICODE_MAX ||
            (codepoint >= JSON_SURROGATE_HIGH_MIN &&
             codepoint <= JSON_SURROGATE_LOW_MAX)) {
            return false;
        }
    }
    return true;
}

static bool hex_four(const esy_json_value_t *value, size_t *offset,
                     uint32_t *codepoint)
{
    uint32_t result = 0u;
    if (value->length - *offset < JSON_HEX_DIGITS + 1u) {
        return false;
    }
    for (size_t i = 0u; i < JSON_HEX_DIGITS; ++i) {
        unsigned char c = (unsigned char)value->data[*offset + i];
        unsigned int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10u;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10u;
        } else {
            return false;
        }
        result = (result << 4u) | digit;
    }
    *offset += JSON_HEX_DIGITS;
    *codepoint = result;
    return true;
}

static size_t encode_codepoint(uint32_t codepoint,
                               unsigned char bytes[JSON_UTF8_MAX_BYTES])
{
    if (codepoint < 0x80u) {
        bytes[0] = (unsigned char)codepoint;
        return 1u;
    }
    if (codepoint < 0x800u) {
        bytes[0] = (unsigned char)(0xc0u | (codepoint >> 6u));
        bytes[1] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        return 2u;
    }
    if (codepoint < JSON_SUPPLEMENTARY_MIN) {
        bytes[0] = (unsigned char)(0xe0u | (codepoint >> 12u));
        bytes[1] = (unsigned char)(0x80u | ((codepoint >> 6u) & 0x3fu));
        bytes[2] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        return 3u;
    }
    bytes[0] = (unsigned char)(0xf0u | (codepoint >> 18u));
    bytes[1] = (unsigned char)(0x80u | ((codepoint >> 12u) & 0x3fu));
    bytes[2] = (unsigned char)(0x80u | ((codepoint >> 6u) & 0x3fu));
    bytes[3] = (unsigned char)(0x80u | (codepoint & 0x3fu));
    return 4u;
}

/* Decode one source byte or one complete escape from an already validated
 * quoted lexeme. Raw UTF-8 bytes may be emitted separately and are unchanged. */
static bool decoded_next(const esy_json_value_t *value, size_t *offset,
                          unsigned char bytes[JSON_UTF8_MAX_BYTES], size_t *count)
{
    unsigned char c = (unsigned char)value->data[(*offset)++];
    *count = 1u;
    if (c != '\\') {
        bytes[0] = c;
        return true;
    }
    if (*offset >= value->length - 1u) {
        return false;
    }
    c = (unsigned char)value->data[(*offset)++];
    switch (c) {
    case '"': bytes[0] = '"'; break;
    case '\\': bytes[0] = '\\'; break;
    case '/': bytes[0] = '/'; break;
    case 'b': bytes[0] = '\b'; break;
    case 'f': bytes[0] = '\f'; break;
    case 'n': bytes[0] = '\n'; break;
    case 'r': bytes[0] = '\r'; break;
    case 't': bytes[0] = '\t'; break;
    case 'u': {
        uint32_t codepoint;
        if (!hex_four(value, offset, &codepoint)) {
            return false;
        }
        if (codepoint >= JSON_SURROGATE_HIGH_MIN &&
            codepoint <= JSON_SURROGATE_HIGH_MAX) {
            uint32_t low;
            if (value->length - *offset < 3u ||
                value->data[*offset] != '\\' || value->data[*offset + 1u] != 'u') {
                return false;
            }
            *offset += 2u;
            if (!hex_four(value, offset, &low) || low < JSON_SURROGATE_LOW_MIN ||
                low > JSON_SURROGATE_LOW_MAX) {
                return false;
            }
            codepoint = JSON_SUPPLEMENTARY_MIN +
                        ((codepoint - JSON_SURROGATE_HIGH_MIN) << 10u) +
                        (low - JSON_SURROGATE_LOW_MIN);
        } else if (codepoint >= JSON_SURROGATE_LOW_MIN &&
                   codepoint <= JSON_SURROGATE_LOW_MAX) {
            return false;
        }
        *count = encode_codepoint(codepoint, bytes);
        break;
    }
    default: return false;
    }
    return true;
}

esy_err_t esy_json_parse(const char *data, size_t length, esy_json_value_t *value)
{
    size_t first = 0u;
    size_t last = length;
    esy_json_value_t result;
    esy_err_t status;
    if (data == NULL || length == 0u || value == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    status = provider_status(JSON_Validate(data, length));
    if (status != ESY_OK) {
        return status;
    }
    while (first < last && json_space(data[first])) {
        ++first;
    }
    while (last > first && json_space(data[last - 1u])) {
        --last;
    }
    if (first == last) {
        return ESY_ERR_FAIL;
    }
    result.data = data + first;
    result.length = last - first;
    result.type = classify(data[first]);
    *value = result;
    return ESY_OK;
}

esy_err_t esy_json_iterator_init(esy_json_iterator_t *iterator,
                                 const esy_json_value_t *collection)
{
    json_iterator_ctx_t ctx = {0};
    esy_err_t status;
    if (iterator == NULL || collection == NULL ||
        (collection->type != ESY_JSON_ARRAY && collection->type != ESY_JSON_OBJECT)) {
        return ESY_ERR_INVALID_ARG;
    }
    status = esy_json_parse(collection->data, collection->length, &ctx.collection);
    if (status != ESY_OK) {
        return status;
    }
    if (ctx.collection.type != collection->type) {
        return ESY_ERR_INVALID_ARG;
    }
    ctx.marker = JSON_ITERATOR_MARKER;
    memset(iterator, 0, sizeof(*iterator));
    memcpy(iterator->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}

esy_err_t esy_json_iterator_next(esy_json_iterator_t *iterator,
                                 esy_json_value_t *key, esy_json_value_t *value)
{
    json_iterator_ctx_t ctx;
    JSONPair_t pair = {0};
    JSONStatus_t status;
    esy_json_value_t next_key = {NULL, 0u, ESY_JSON_NULL};
    esy_json_value_t next_value;
    if (iterator == NULL || key == NULL || value == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    memcpy(&ctx, iterator->storage.bytes, sizeof(ctx));
    if (ctx.marker != JSON_ITERATOR_MARKER) {
        return ESY_ERR_STATE;
    }
    if (ctx.exhausted) {
        return ESY_ERR_NOT_SUPPORTED;
    }
    status = JSON_Iterate(ctx.collection.data, ctx.collection.length,
                          &ctx.start, &ctx.next, &pair);
    if (status == JSONNotFound) {
        ctx.exhausted = true;
        memcpy(iterator->storage.bytes, &ctx, sizeof(ctx));
        return ESY_ERR_NOT_SUPPORTED;
    }
    if (status != JSONSuccess) {
        return provider_status(status);
    }
    if (pair.key != NULL) {
        next_key.data = pair.key - 1;
        next_key.length = pair.keyLength + 2u;
        next_key.type = ESY_JSON_STRING;
    }
    next_value.data = pair.value;
    next_value.length = pair.valueLength;
    if (pair.jsonType == JSONString) {
        --next_value.data;
        next_value.length += 2u;
    }
    next_value.type = classify(next_value.data[0]);
    memcpy(iterator->storage.bytes, &ctx, sizeof(ctx));
    *key = next_key;
    *value = next_value;
    return ESY_OK;
}

static esy_err_t key_equal(const esy_json_value_t *key, const char *expected,
                           size_t expected_length, bool *equal)
{
    size_t offset = 1u;
    size_t compared = 0u;
    bool matches = true;
    while (offset < key->length - 1u) {
        unsigned char bytes[JSON_UTF8_MAX_BYTES];
        size_t count;
        if (!decoded_next(key, &offset, bytes, &count)) {
            return ESY_ERR_FAIL;
        }
        if (matches) {
            if (count > expected_length - compared ||
                memcmp(bytes, expected + compared, count) != 0) {
                matches = false;
            } else {
                compared += count;
            }
        }
    }
    *equal = matches && compared == expected_length;
    return ESY_OK;
}

esy_err_t esy_json_object_get(const esy_json_value_t *object, const char *key,
                              size_t key_length, esy_json_value_t *value)
{
    esy_json_iterator_t iterator;
    esy_json_value_t current_key;
    esy_json_value_t current_value;
    esy_json_value_t found = {NULL, 0u, ESY_JSON_NULL};
    bool have_match = false;
    esy_err_t status;
    if (!valid_view(object, ESY_JSON_OBJECT) || value == NULL ||
        (key == NULL && key_length != 0u) || !utf8_valid(key, key_length)) {
        return ESY_ERR_INVALID_ARG;
    }
    status = esy_json_iterator_init(&iterator, object);
    if (status != ESY_OK) {
        return status;
    }
    while ((status = esy_json_iterator_next(&iterator, &current_key,
                                            &current_value)) == ESY_OK) {
        bool equal;
        status = key_equal(&current_key, key, key_length, &equal);
        if (status != ESY_OK) {
            return status;
        }
        if (equal) {
            if (have_match) {
                return ESY_ERR_FAIL;
            }
            found = current_value;
            have_match = true;
        }
    }
    if (status != ESY_ERR_NOT_SUPPORTED) {
        return status;
    }
    if (!have_match) {
        return ESY_ERR_NOT_SUPPORTED;
    }
    *value = found;
    return ESY_OK;
}

esy_err_t esy_json_array_get(const esy_json_value_t *array, size_t index,
                             esy_json_value_t *value)
{
    esy_json_iterator_t iterator;
    esy_json_value_t key;
    esy_json_value_t current;
    esy_err_t status;
    if (!valid_view(array, ESY_JSON_ARRAY) || value == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    status = esy_json_iterator_init(&iterator, array);
    if (status != ESY_OK) {
        return status;
    }
    for (;;) {
        status = esy_json_iterator_next(&iterator, &key, &current);
        if (status != ESY_OK) {
            return status;
        }
        if (index == 0u) {
            *value = current;
            return ESY_OK;
        }
        --index;
    }
}

esy_err_t esy_json_as_bool(const esy_json_value_t *value, bool *result)
{
    bool decoded;
    if (!valid_view(value, ESY_JSON_BOOL) || result == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    if (value->length == 4u && memcmp(value->data, "true", 4u) == 0) {
        decoded = true;
    } else if (value->length == 5u && memcmp(value->data, "false", 5u) == 0) {
        decoded = false;
    } else {
        return ESY_ERR_FAIL;
    }
    *result = decoded;
    return ESY_OK;
}

static esy_err_t magnitude(const char *data, size_t length, uint64_t limit,
                           uint64_t *result)
{
    uint64_t number = 0u;
    if (length == 0u || (length > 1u && data[0] == '0')) {
        return ESY_ERR_FAIL;
    }
    for (size_t i = 0u; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        unsigned int digit;
        if (c < '0' || c > '9') {
            return ESY_ERR_FAIL;
        }
        digit = c - '0';
        if (number > (limit - digit) / 10u) {
            return ESY_ERR_FAIL;
        }
        number = number * 10u + digit;
    }
    *result = number;
    return ESY_OK;
}

esy_err_t esy_json_as_u64(const esy_json_value_t *value, uint64_t *result)
{
    if (!valid_view(value, ESY_JSON_NUMBER) || result == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    return magnitude(value->data, value->length, UINT64_MAX, result);
}

esy_err_t esy_json_as_i64(const esy_json_value_t *value, int64_t *result)
{
    bool negative;
    uint64_t number;
    uint64_t limit;
    esy_err_t status;
    if (!valid_view(value, ESY_JSON_NUMBER) || result == NULL) {
        return ESY_ERR_INVALID_ARG;
    }
    negative = value->data[0] == '-';
    limit = (uint64_t)INT64_MAX + (negative ? 1u : 0u);
    status = magnitude(value->data + (negative ? 1u : 0u),
                        value->length - (negative ? 1u : 0u), limit, &number);
    if (status != ESY_OK) {
        return status;
    }
    if (negative && number == (uint64_t)INT64_MAX + 1u) {
        *result = INT64_MIN;
    } else {
        *result = negative ? -(int64_t)number : (int64_t)number;
    }
    return ESY_OK;
}

esy_err_t esy_json_string_copy(const esy_json_value_t *value, char *buffer,
                               size_t capacity, size_t *length)
{
    esy_json_value_t checked;
    size_t needed = 0u;
    size_t offset = 1u;
    esy_err_t status;
    if (!valid_view(value, ESY_JSON_STRING) || length == NULL ||
        (buffer == NULL && capacity != 0u)) {
        return ESY_ERR_INVALID_ARG;
    }
    status = esy_json_parse(value->data, value->length, &checked);
    if (status != ESY_OK) {
        return status;
    }
    if (checked.type != ESY_JSON_STRING || checked.length != value->length) {
        return ESY_ERR_INVALID_ARG;
    }
    while (offset < checked.length - 1u) {
        unsigned char bytes[JSON_UTF8_MAX_BYTES];
        size_t count;
        if (!decoded_next(&checked, &offset, bytes, &count)) {
            return ESY_ERR_FAIL;
        }
        needed += count;
    }
    if (buffer == NULL) {
        *length = needed;
        return ESY_OK;
    }
    if (needed >= capacity) {
        *length = needed;
        return ESY_ERR_NO_MEM;
    }
    offset = 1u;
    needed = 0u;
    while (offset < checked.length - 1u) {
        unsigned char bytes[JSON_UTF8_MAX_BYTES];
        size_t count;
        if (!decoded_next(&checked, &offset, bytes, &count)) {
            return ESY_ERR_FAIL;
        }
        memcpy(buffer + needed, bytes, count);
        needed += count;
    }
    buffer[needed] = '\0';
    *length = needed;
    return ESY_OK;
}
