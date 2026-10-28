/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/coredump/esy_coredump_logger.h"

#include <inttypes.h>

#include "esy_middleware/logger/esy_logger.h"

static const char k_hex_digits[] = "0123456789abcdef";

esy_err_t esy_coredump_logger_export_chunk(esy_coredump_t *store, size_t offset) {
  esy_coredump_info_t info;
  esy_err_t result = esy_coredump_get_info(store, &info);
  if (result != ESY_OK) return result;
  if (offset >= info.record_size) return ESY_ERR_INVALID_ARG;
  size_t length = info.record_size - offset;
  if (length > ESY_COREDUMP_LOGGER_CHUNK_BYTES) {
    length = ESY_COREDUMP_LOGGER_CHUNK_BYTES;
  }
  uint8_t bytes[ESY_COREDUMP_LOGGER_CHUNK_BYTES];
  result = esy_coredump_read(store, &info.record_id, offset, bytes, length);
  if (result != ESY_OK) return result;
  char hexadecimal[ESY_COREDUMP_LOGGER_CHUNK_BYTES * 2u + 1u];
  for (size_t index = 0u; index < length; ++index) {
    uint8_t high = bytes[index] >> 4u;
    uint8_t low = bytes[index] & 0x0fu;
    hexadecimal[index * 2u] = k_hex_digits[high];
    hexadecimal[index * 2u + 1u] = k_hex_digits[low];
  }
  hexadecimal[length * 2u] = '\0';
  char identifier[ESY_COREDUMP_RECORD_ID_BYTES * 2u + 1u];
  for (size_t index = 0u; index < ESY_COREDUMP_RECORD_ID_BYTES; ++index) {
    identifier[index * 2u] = k_hex_digits[info.record_id.bytes[index] >> 4u];
    identifier[index * 2u + 1u] = k_hex_digits[info.record_id.bytes[index] & 0x0fu];
  }
  identifier[ESY_COREDUMP_RECORD_ID_BYTES * 2u] = '\0';
  esy_log("COREDUMP", ESY_LOG_LEVEL_INFO,
          "CD1 %s %" PRIu32 " %zu %s",
          identifier, info.record_size, offset, hexadecimal);
  return ESY_OK;
}
