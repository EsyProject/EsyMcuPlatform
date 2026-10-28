/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/settings/esy_settings.h"

#include <string.h>

#include "esy_common/utils/base/esy_endian.h"

#define SNAPSHOT_HEADER_BYTES 16u
#define SNAPSHOT_VERSION_OFFSET 4u
#define SNAPSHOT_COUNT_OFFSET 8u
#define SNAPSHOT_LENGTH_OFFSET 12u
#define ENTRY_HEADER_BYTES 8u
#define ENTRY_VALUE_LENGTH_OFFSET 4u

enum { SNAPSHOT_VERSION = 1 };

static const uint32_t k_snapshot_magic = UINT32_C(0x53595345);
static const uint32_t k_handle_magic = UINT32_C(0x53455453);

typedef struct {
  uintptr_t owner;
  uint32_t magic;
  esy_file_t *file;
  uint8_t *work_buffer;
  uint8_t *scratch_buffer;
  size_t buffer_size;
  size_t capacity;
  size_t max_key_length;
  size_t length;
  uint32_t count;
  bool has_snapshot;
  esy_settings_state_t state;
} settings_ctx_t;

typedef struct {
  size_t offset;
  size_t span;
  uint32_t key_length;
  uint32_t value_length;
} entry_t;

_Static_assert(sizeof(settings_ctx_t) <= ESY_SETTINGS_STORAGE_BYTES,
               "Settings handle capacity is insufficient");

static bool bytes_are_zero(const uint8_t *bytes, size_t length) {
  for (size_t i = 0u; i < length; ++i) {
    if (bytes[i] != 0u) return false;
  }
  return true;
}

static bool ranges_overlap(const void *left, size_t left_size,
                           const void *right, size_t right_size) {
  uintptr_t a = (uintptr_t)left;
  uintptr_t b = (uintptr_t)right;
  if (left_size > UINTPTR_MAX - a || right_size > UINTPTR_MAX - b) return true;
  return left_size != 0u && right_size != 0u &&
         a < b + right_size && b < a + left_size;
}

static bool overlaps_resources(const settings_ctx_t *ctx,
                               const void *data, size_t length) {
  return ranges_overlap(data, length, (const void *)ctx->owner,
                        sizeof(esy_settings_t)) ||
         ranges_overlap(data, length, ctx->file, sizeof(*ctx->file)) ||
         ranges_overlap(data, length, ctx->work_buffer, ctx->buffer_size) ||
         ranges_overlap(data, length, ctx->scratch_buffer, ctx->buffer_size);
}

static esy_err_t load_context(const esy_settings_t *settings,
                              settings_ctx_t *ctx) {
  if (settings == NULL) return ESY_ERR_INVALID_ARG;
  memcpy(ctx, settings->storage.bytes, sizeof(*ctx));
  if (ctx->magic != k_handle_magic || ctx->owner != (uintptr_t)settings) {
    return ESY_ERR_STATE;
  }
  return ESY_OK;
}

static void save_context(esy_settings_t *settings, const settings_ctx_t *ctx) {
  memcpy(settings->storage.bytes, ctx, sizeof(*ctx));
}

static void build_header(uint8_t *buffer, uint32_t count, size_t length) {
  esy_write_u32_le(buffer, k_snapshot_magic);
  esy_write_u32_le(buffer + SNAPSHOT_VERSION_OFFSET, SNAPSHOT_VERSION);
  esy_write_u32_le(buffer + SNAPSHOT_COUNT_OFFSET, count);
  esy_write_u32_le(buffer + SNAPSHOT_LENGTH_OFFSET, (uint32_t)length);
}

static entry_t read_entry(const uint8_t *buffer, size_t offset) {
  entry_t entry = {
      .offset = offset,
      .key_length = esy_read_u32_le(buffer + offset),
      .value_length = esy_read_u32_le(buffer + offset + ENTRY_VALUE_LENGTH_OFFSET)};
  entry.span = ENTRY_HEADER_BYTES + (size_t)entry.key_length + entry.value_length;
  return entry;
}

static esy_err_t validate_snapshot(const uint8_t *buffer, size_t length,
                                   size_t max_key_length, uint32_t *count) {
  if (length < SNAPSHOT_HEADER_BYTES) return ESY_ERR_CRC_FAIL;
  if (esy_read_u32_le(buffer) != k_snapshot_magic ||
      esy_read_u32_le(buffer + SNAPSHOT_VERSION_OFFSET) != SNAPSHOT_VERSION) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  if (esy_read_u32_le(buffer + SNAPSHOT_LENGTH_OFFSET) != length) {
    return ESY_ERR_CRC_FAIL;
  }
  uint32_t entries = esy_read_u32_le(buffer + SNAPSHOT_COUNT_OFFSET);
  if (entries > (length - SNAPSHOT_HEADER_BYTES) / (ENTRY_HEADER_BYTES + 1u)) {
    return ESY_ERR_CRC_FAIL;
  }
  size_t offset = SNAPSHOT_HEADER_BYTES;
  for (uint32_t i = 0u; i < entries; ++i) {
    if (length - offset < ENTRY_HEADER_BYTES) return ESY_ERR_CRC_FAIL;
    uint32_t key_length = esy_read_u32_le(buffer + offset);
    uint32_t value_length =
        esy_read_u32_le(buffer + offset + ENTRY_VALUE_LENGTH_OFFSET);
    size_t remaining = length - offset - ENTRY_HEADER_BYTES;
    if (key_length == 0u || key_length > remaining ||
        value_length > remaining - key_length) return ESY_ERR_CRC_FAIL;
    if (key_length > max_key_length) return ESY_ERR_NO_MEM;
    const uint8_t *key = buffer + offset + ENTRY_HEADER_BYTES;
    if (memchr(key, '\0', key_length) != NULL) return ESY_ERR_CRC_FAIL;
    size_t previous = SNAPSHOT_HEADER_BYTES;
    for (uint32_t j = 0u; j < i; ++j) {
      entry_t entry = read_entry(buffer, previous);
      if (entry.key_length == key_length &&
          memcmp(buffer + previous + ENTRY_HEADER_BYTES, key, key_length) == 0) {
        return ESY_ERR_CRC_FAIL;
      }
      previous += entry.span;
    }
    offset += ENTRY_HEADER_BYTES + (size_t)key_length + value_length;
  }
  if (offset != length) return ESY_ERR_CRC_FAIL;
  *count = entries;
  return ESY_OK;
}

static bool find_entry(const settings_ctx_t *ctx, const char *key,
                       size_t key_length, entry_t *entry) {
  size_t offset = SNAPSHOT_HEADER_BYTES;
  for (uint32_t i = 0u; i < ctx->count; ++i) {
    *entry = read_entry(ctx->work_buffer, offset);
    if (entry->key_length == key_length &&
        memcmp(ctx->work_buffer + offset + ENTRY_HEADER_BYTES, key, key_length) == 0) {
      return true;
    }
    offset += entry->span;
  }
  *entry = (entry_t){.offset = ctx->length};
  return false;
}

static esy_err_t key_length(const settings_ctx_t *ctx, const char *key,
                            size_t *length) {
  if (key == NULL || overlaps_resources(ctx, key, 1u)) return ESY_ERR_INVALID_ARG;
  size_t size = 0u;
  while (size <= ctx->max_key_length && key[size] != '\0') ++size;
  if (size == 0u || size > ctx->max_key_length ||
      overlaps_resources(ctx, key, size + 1u)) {
    return ESY_ERR_INVALID_ARG;
  }
  *length = size;
  return ESY_OK;
}

static esy_err_t reload_context(settings_ctx_t *ctx) {
  esy_file_info_t info;
  esy_err_t result = esy_file_get_info(ctx->file, &info);
  if (result != ESY_OK) return result;
  const uint32_t required = ESY_FILE_CAP_READ | ESY_FILE_CAP_ATOMIC_REPLACE;
  if ((info.capabilities & required) != required) return ESY_ERR_NOT_SUPPORTED;
  size_t capacity = ctx->buffer_size < info.capacity ? ctx->buffer_size : info.capacity;
  if (capacity < SNAPSHOT_HEADER_BYTES) return ESY_ERR_NO_MEM;
  size_t length = SNAPSHOT_HEADER_BYTES;
  uint32_t count = 0u;
  if (info.exists) {
    length = info.length;
    if (length > capacity) return ESY_ERR_NO_MEM;
    result = esy_file_read(ctx->file, 0u, ctx->scratch_buffer, length);
    if (result != ESY_OK) return result;
    result = validate_snapshot(ctx->scratch_buffer, length, ctx->max_key_length, &count);
    if (result != ESY_OK) return result;
  } else {
    build_header(ctx->scratch_buffer, count, length);
  }
  memcpy(ctx->work_buffer, ctx->scratch_buffer, length);
  ctx->length = length;
  ctx->capacity = capacity;
  ctx->count = count;
  ctx->has_snapshot = info.exists;
  ctx->state = ESY_SETTINGS_CLEAN;
  return ESY_OK;
}

static esy_err_t configure_context(esy_settings_t *settings,
                                   const esy_settings_config_t *config,
                                   settings_ctx_t *ctx) {
  if (config == NULL || config->file == NULL ||
      config->static_resources == NULL || config->max_key_length == 0u ||
      config->max_key_length > UINT32_MAX || config->max_key_length == SIZE_MAX) {
    return ESY_ERR_INVALID_ARG;
  }
  const esy_settings_static_resources_t *resources = config->static_resources;
  if (resources->working_buffer == NULL || resources->scratch_buffer == NULL ||
      resources->buffer_size > UINT32_MAX) return ESY_ERR_INVALID_ARG;
  if (resources->buffer_size < SNAPSHOT_HEADER_BYTES) return ESY_ERR_NO_MEM;
  *ctx = (settings_ctx_t){
      .owner = (uintptr_t)settings,
      .magic = k_handle_magic,
      .file = config->file,
      .work_buffer = resources->working_buffer,
      .scratch_buffer = resources->scratch_buffer,
      .buffer_size = resources->buffer_size,
      .max_key_length = config->max_key_length};
  if (ranges_overlap(settings, sizeof(*settings), ctx->file, sizeof(*ctx->file)) ||
      ranges_overlap(settings, sizeof(*settings), ctx->work_buffer, ctx->buffer_size) ||
      ranges_overlap(settings, sizeof(*settings), ctx->scratch_buffer, ctx->buffer_size) ||
      ranges_overlap(ctx->file, sizeof(*ctx->file), ctx->work_buffer, ctx->buffer_size) ||
      ranges_overlap(ctx->file, sizeof(*ctx->file), ctx->scratch_buffer, ctx->buffer_size) ||
      ranges_overlap(ctx->work_buffer, ctx->buffer_size,
                     ctx->scratch_buffer, ctx->buffer_size)) return ESY_ERR_INVALID_ARG;
  return ESY_OK;
}

esy_err_t esy_settings_init(esy_settings_t *settings,
                            const esy_settings_config_t *config) {
  if (settings == NULL) return ESY_ERR_INVALID_ARG;
  if (!bytes_are_zero(settings->storage.bytes, sizeof(settings->storage.bytes))) {
    return ESY_ERR_STATE;
  }
  settings_ctx_t ctx;
  esy_err_t result = configure_context(settings, config, &ctx);
  if (result == ESY_OK) result = reload_context(&ctx);
  if (result == ESY_OK) save_context(settings, &ctx);
  return result;
}

esy_err_t esy_settings_deinit(esy_settings_t *settings) {
  if (settings == NULL) return ESY_ERR_INVALID_ARG;
  if (bytes_are_zero(settings->storage.bytes, sizeof(settings->storage.bytes))) {
    return ESY_OK;
  }
  settings_ctx_t ctx;
  esy_err_t result = load_context(settings, &ctx);
  if (result == ESY_OK) memset(settings, 0, sizeof(*settings));
  return result;
}

esy_err_t esy_settings_get(const esy_settings_t *settings, const char *key,
                           void *buffer, size_t capacity, size_t *length,
                           bool *found) {
  settings_ctx_t ctx;
  esy_err_t result = load_context(settings, &ctx);
  if (result != ESY_OK) return result;
  if (length == NULL || found == NULL || (buffer == NULL && capacity != 0u) ||
      overlaps_resources(&ctx, buffer, capacity) ||
      overlaps_resources(&ctx, length, sizeof(*length)) ||
      overlaps_resources(&ctx, found, sizeof(*found)) ||
      ranges_overlap(buffer, capacity, length, sizeof(*length)) ||
      ranges_overlap(buffer, capacity, found, sizeof(*found)) ||
      ranges_overlap(length, sizeof(*length), found, sizeof(*found))) {
    return ESY_ERR_INVALID_ARG;
  }
  size_t size;
  result = key_length(&ctx, key, &size);
  if (result != ESY_OK) return result;
  entry_t entry;
  bool present = find_entry(&ctx, key, size, &entry);
  if (!present) {
    *found = false;
    *length = 0u;
    return ESY_OK;
  }
  if (capacity < entry.value_length) {
    *found = true;
    *length = entry.value_length;
    return ESY_ERR_NO_MEM;
  }
  if (entry.value_length != 0u) {
    memcpy(buffer, ctx.work_buffer + entry.offset + ENTRY_HEADER_BYTES + entry.key_length,
           entry.value_length);
  }
  *found = true;
  *length = entry.value_length;
  return ESY_OK;
}

esy_err_t esy_settings_set(esy_settings_t *settings, const char *key,
                           const void *value, size_t length) {
  settings_ctx_t ctx;
  esy_err_t result = load_context(settings, &ctx);
  if (result != ESY_OK) return result;
  if (ctx.state == ESY_SETTINGS_UNCERTAIN) return ESY_ERR_STATE;
  if ((value == NULL && length != 0u) || overlaps_resources(&ctx, value, length)) {
    return ESY_ERR_INVALID_ARG;
  }
  size_t size;
  result = key_length(&ctx, key, &size);
  if (result != ESY_OK) return result;
  entry_t entry;
  bool present = find_entry(&ctx, key, size, &entry);
  if (present && entry.value_length == length &&
      (length == 0u ||
       memcmp(ctx.work_buffer + entry.offset + ENTRY_HEADER_BYTES + entry.key_length,
              value, length) == 0)) return ESY_OK;
  size_t retained = ctx.length - entry.span;
  if (size > ctx.capacity - retained ||
      ENTRY_HEADER_BYTES > ctx.capacity - retained - size ||
      length > ctx.capacity - retained - size - ENTRY_HEADER_BYTES) {
    return ESY_ERR_NO_MEM;
  }
  size_t span = ENTRY_HEADER_BYTES + size + length;
  size_t total = retained + span;
  memcpy(ctx.scratch_buffer, ctx.work_buffer, entry.offset);
  uint8_t *replacement = ctx.scratch_buffer + entry.offset;
  esy_write_u32_le(replacement, (uint32_t)size);
  esy_write_u32_le(replacement + ENTRY_VALUE_LENGTH_OFFSET, (uint32_t)length);
  memcpy(replacement + ENTRY_HEADER_BYTES, key, size);
  if (length != 0u) memcpy(replacement + ENTRY_HEADER_BYTES + size, value, length);
  size_t suffix = ctx.length - entry.offset - entry.span;
  memcpy(replacement + span, ctx.work_buffer + entry.offset + entry.span, suffix);
  uint32_t count = ctx.count + (present ? 0u : 1u);
  build_header(ctx.scratch_buffer, count, total);
  memcpy(ctx.work_buffer, ctx.scratch_buffer, total);
  ctx.count = count;
  ctx.length = total;
  ctx.state = ESY_SETTINGS_DIRTY;
  save_context(settings, &ctx);
  return ESY_OK;
}

esy_err_t esy_settings_remove(esy_settings_t *settings, const char *key) {
  settings_ctx_t ctx;
  esy_err_t result = load_context(settings, &ctx);
  if (result != ESY_OK) return result;
  if (ctx.state == ESY_SETTINGS_UNCERTAIN) return ESY_ERR_STATE;
  size_t size;
  result = key_length(&ctx, key, &size);
  if (result != ESY_OK) return result;
  entry_t entry;
  if (!find_entry(&ctx, key, size, &entry)) return ESY_OK;
  memcpy(ctx.scratch_buffer, ctx.work_buffer, entry.offset);
  memcpy(ctx.scratch_buffer + entry.offset,
         ctx.work_buffer + entry.offset + entry.span,
         ctx.length - entry.offset - entry.span);
  ctx.length -= entry.span;
  --ctx.count;
  build_header(ctx.scratch_buffer, ctx.count, ctx.length);
  memcpy(ctx.work_buffer, ctx.scratch_buffer, ctx.length);
  ctx.state = ESY_SETTINGS_DIRTY;
  save_context(settings, &ctx);
  return ESY_OK;
}

esy_err_t esy_settings_commit(esy_settings_t *settings) {
  settings_ctx_t ctx;
  esy_err_t result = load_context(settings, &ctx);
  if (result != ESY_OK) return result;
  if (ctx.state == ESY_SETTINGS_UNCERTAIN) return ESY_ERR_STATE;
  if (ctx.state == ESY_SETTINGS_CLEAN) return ESY_OK;
  ctx.state = ESY_SETTINGS_UNCERTAIN;
  save_context(settings, &ctx);
  result = esy_file_replace(ctx.file, ctx.work_buffer, ctx.length);
  if (result == ESY_OK) {
    ctx.state = ESY_SETTINGS_CLEAN;
    ctx.has_snapshot = true;
    save_context(settings, &ctx);
  }
  return result;
}

esy_err_t esy_settings_reload(esy_settings_t *settings) {
  settings_ctx_t ctx;
  esy_err_t result = load_context(settings, &ctx);
  if (result == ESY_OK) result = reload_context(&ctx);
  if (result == ESY_OK) save_context(settings, &ctx);
  return result;
}

esy_err_t esy_settings_get_info(const esy_settings_t *settings,
                                esy_settings_info_t *info) {
  settings_ctx_t ctx;
  esy_err_t result = load_context(settings, &ctx);
  if (result != ESY_OK) return result;
  if (info == NULL || overlaps_resources(&ctx, info, sizeof(*info))) {
    return ESY_ERR_INVALID_ARG;
  }
  *info = (esy_settings_info_t){
      .state = ctx.state,
      .has_snapshot = ctx.has_snapshot,
      .entry_count = ctx.count,
      .used_bytes = ctx.length,
      .capacity = ctx.capacity};
  return ESY_OK;
}
