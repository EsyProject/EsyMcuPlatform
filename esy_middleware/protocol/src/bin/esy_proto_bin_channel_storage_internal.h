/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_PROTO_BIN_CHANNEL_STORAGE_INTERNAL_H
#define ESY_PROTO_BIN_CHANNEL_STORAGE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Initialization-only ranges: handle, descriptors and writable buffers must be
 * distinct. Integer bounds are checked before any endpoint addition. */
typedef struct {
  const void *data;
  size_t size;
} esy_proto_bin_channel_range_t;

static inline bool esy_proto_bin_channel_range_is_valid(const void *data, size_t size) {
  return data != NULL && size != 0u && size <= UINTPTR_MAX - (uintptr_t)data;
}

static inline bool esy_proto_bin_channel_ranges_are_disjoint(
    const esy_proto_bin_channel_range_t *ranges, size_t count) {
  for (size_t i = 0u; i < count; ++i) {
    if (!esy_proto_bin_channel_range_is_valid(ranges[i].data, ranges[i].size))
      return false;
  }
  for (size_t i = 0u; i < count; ++i) {
    const uintptr_t begin = (uintptr_t)ranges[i].data;
    const uintptr_t end = begin + ranges[i].size;
    for (size_t j = i + 1u; j < count; ++j) {
      const uintptr_t other_begin = (uintptr_t)ranges[j].data;
      const uintptr_t other_end = other_begin + ranges[j].size;
      if (begin < other_end && other_begin < end) return false;
    }
  }
  return true;
}

#endif /* ESY_PROTO_BIN_CHANNEL_STORAGE_INTERNAL_H */
