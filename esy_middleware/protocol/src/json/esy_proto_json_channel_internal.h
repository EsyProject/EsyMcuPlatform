/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_JSON_CHANNEL_INTERNAL_H
#define ESY_PROTO_JSON_CHANNEL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared wire and storage primitives for the two actual JSON channel users.
 * Transport progression, session lifetime and callbacks stay with each owner. */
#define JSON_CHANNEL_HEADER_BYTES 4u

typedef struct {
    const void *data;
    size_t size;
} json_channel_range_t;

static inline bool json_channel_range_valid(const void *data, size_t size)
{
    return data != NULL && size != 0u && size <= UINTPTR_MAX - (uintptr_t)data;
}

/* Both ranges must have passed json_channel_range_valid first. */
static inline bool json_channel_overlaps(const void *first, size_t first_size,
                                         const void *second, size_t second_size)
{
    uintptr_t a = (uintptr_t)first;
    uintptr_t b = (uintptr_t)second;
    return a < b + second_size && b < a + first_size;
}

static inline void json_channel_write_length(uint8_t *header, uint32_t length)
{
    header[0] = (uint8_t)(length >> 24u);
    header[1] = (uint8_t)(length >> 16u);
    header[2] = (uint8_t)(length >> 8u);
    header[3] = (uint8_t)length;
}

static inline uint32_t json_channel_read_length(const uint8_t *header)
{
    return ((uint32_t)header[0] << 24u) | ((uint32_t)header[1] << 16u) |
           ((uint32_t)header[2] << 8u) | (uint32_t)header[3];
}

#endif /* ESY_PROTO_JSON_CHANNEL_INTERNAL_H */
