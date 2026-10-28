/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef COREDUMP_FIXTURE_H
#define COREDUMP_FIXTURE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/diagnostics/esy_coredump_defs.h"

#define FIXTURE_HEADER_BYTES 384u
#define FIXTURE_MAGIC UINT32_C(0x31444345)
#define FIXTURE_ARCH_ARM 1u
#define FIXTURE_RECORD_ID_OFFSET 268u

typedef struct {
  uint32_t sequence;
  unsigned calls;
  esy_err_t result;
  bool zero;
  bool repeat;
  esy_coredump_record_id_t last;
} fixture_id_source_t;

static inline void fixture_u32(uint8_t *bytes, uint32_t value) {
  for (size_t index = 0u; index < 4u; ++index) {
    bytes[index] = (uint8_t)(value >> (index * 8u));
  }
}

static inline uint32_t fixture_read_u32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
         ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static inline esy_err_t fixture_new_record_id(void *context,
                                              esy_coredump_record_id_t *id) {
  fixture_id_source_t *source = context;
  ++source->calls;
  if (source->result != ESY_OK) return source->result;
  if (source->zero) {
    memset(id->bytes, 0, sizeof(id->bytes));
  } else if (source->repeat) {
    *id = source->last;
  } else {
    memset(id->bytes, 0xa7, sizeof(id->bytes));
    fixture_u32(id->bytes, ++source->sequence);
    source->last = *id;
  }
  return ESY_OK;
}

static inline uint32_t fixture_crc_part(uint32_t crc, const uint8_t *bytes, size_t length) {
  for (size_t index = 0u; index < length; ++index) {
    crc ^= bytes[index];
    for (uint32_t bit = 0u; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
    }
  }
  return crc;
}

static inline uint32_t fixture_record_crc(const uint8_t *bytes, size_t length) {
  uint32_t crc = fixture_crc_part(UINT32_MAX, bytes + 4u, 8u);
  return ~fixture_crc_part(crc, bytes + 16u, length - 16u);
}

/* Independent fixtures encode the documented wire format, not private structs. */
static inline size_t fixture_record(uint8_t *bytes, uint32_t image_base,
                                    uint8_t identity, const uint8_t *stack, size_t length) {
  size_t total = FIXTURE_HEADER_BYTES + length;
  memset(bytes, 0, total);
  fixture_u32(bytes, FIXTURE_MAGIC);
  fixture_u32(bytes + 4u, 1u);
  fixture_u32(bytes + 8u, (uint32_t)total);
  fixture_u32(bytes + 16u, FIXTURE_ARCH_ARM);
  fixture_u32(bytes + 20u, 1u);
  fixture_u32(bytes + 28u, 3u);
  fixture_u32(bytes + 32u, image_base);
  fixture_u32(bytes + 36u, 10001u);
  fixture_u32(bytes + 40u, UINT32_C(0x20001000));
  fixture_u32(bytes + 44u, (uint32_t)length);
  memset(bytes + 48u, identity, 20u);
  fixture_u32(bytes + 148u, UINT32_C(0x20001000));
  fixture_u32(bytes + 152u, image_base + 17u);
  fixture_u32(bytes + 156u, image_base + 33u);
  memset(bytes + FIXTURE_RECORD_ID_OFFSET, identity, ESY_COREDUMP_RECORD_ID_BYTES);
  if (length != 0u) memcpy(bytes + FIXTURE_HEADER_BYTES, stack, length);
  fixture_u32(bytes + 12u, fixture_record_crc(bytes, total));
  return total;
}

#endif /* COREDUMP_FIXTURE_H */
