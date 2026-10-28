/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/credentials/esy_credentials_store.h"

#include <string.h>
#include "esy_common/utils/integrity/esy_crc32.h"
#include "esy_common/utils/base/esy_endian.h"

#define STORE_MAGIC UINT32_C(0x53524345)
#define HANDLE_MAGIC UINT32_C(0x43535452)
#define HEADER_BYTES 96u
#define CRC_BYTES 4u
#define VERSION_OFFSET 4u
#define LENGTH_OFFSET 8u
#define RESERVED_OFFSET 12u
#define TRANSACTION_OFFSET 16u
#define LAST_ID_OFFSET 24u
#define CONFIRMED_ID_OFFSET 32u
#define CANDIDATE_ID_OFFSET 40u
#define CONFIRMED_DESCRIPTOR_OFFSET 48u
#define CANDIDATE_DESCRIPTOR_OFFSET 72u
#define BLOB_DESCRIPTOR_BYTES 8u
#define FORMAT_OFFSET 4u
#define BUNDLE_BLOB_COUNT 3u
#define COUNTER_HALF_BYTES 4u
#define COUNTER_HALF_BITS 32u

enum { STORE_VERSION = 1 };

typedef struct {
  const esy_credentials_store_t *owner;
  esy_file_t *file;
  uint8_t *working;
  uint8_t *scratch;
  size_t buffer_size;
  esy_credentials_store_info_t info;
  uint32_t magic;
  bool busy;
} credentials_store_ctx_t;

typedef struct {
  esy_credentials_config_t confirmed;
  esy_credentials_config_t candidate;
} snapshot_view_t;

_Static_assert(sizeof(credentials_store_ctx_t) <= ESY_CREDENTIALS_STORE_STORAGE_BYTES,
               "Credential store handle capacity is insufficient");
_Static_assert(HEADER_BYTES + CRC_BYTES == ESY_CREDENTIALS_STORE_MIN_SNAPSHOT_BYTES,
               "Credential snapshot minimum is inconsistent");

static void clear_bytes(void *data, size_t size) {
  volatile uint8_t *bytes = data;
  while (size != 0u) {
    *bytes++ = 0u;
    --size;
  }
}

static uint64_t read_counter(const uint8_t *data) {
  return (uint64_t)esy_read_u32_le(data) |
         ((uint64_t)esy_read_u32_le(data + COUNTER_HALF_BYTES) << COUNTER_HALF_BITS);
}

static void write_counter(uint8_t *data, uint64_t value) {
  esy_write_u32_le(data, (uint32_t)value);
  esy_write_u32_le(data + COUNTER_HALF_BYTES, (uint32_t)(value >> COUNTER_HALF_BITS));
}

static bool bytes_zero(const void *data, size_t size) {
  const uint8_t *bytes = data;
  for (size_t i = 0u; i < size; ++i) {
    if (bytes[i] != 0u) return false;
  }
  return true;
}

static bool overlaps(const void *left, size_t left_size,
                      const void *right, size_t right_size) {
  uintptr_t a = (uintptr_t)left;
  uintptr_t b = (uintptr_t)right;
  if (left_size == 0u || right_size == 0u) return false;
  if (left_size - 1u > UINTPTR_MAX - a || right_size - 1u > UINTPTR_MAX - b)
    return true;
  return a <= b + right_size - 1u && b <= a + left_size - 1u;
}

static bool overlaps_resources(const credentials_store_ctx_t *ctx,
                                const void *data, size_t size) {
  return overlaps(data, size, ctx->owner, sizeof(*ctx->owner)) ||
         overlaps(data, size, ctx->file, sizeof(*ctx->file)) ||
         overlaps(data, size, ctx->working, ctx->buffer_size) ||
         overlaps(data, size, ctx->scratch, ctx->buffer_size);
}

static esy_err_t get_context(const esy_credentials_store_t *store,
                              credentials_store_ctx_t *ctx) {
  if (store == NULL) return ESY_ERR_INVALID_ARG;
  memcpy(ctx, store->storage.bytes, sizeof(*ctx));
  if (ctx->magic != HANDLE_MAGIC || ctx->owner != store) return ESY_ERR_STATE;
  return ctx->busy ? ESY_ERR_BUSY : ESY_OK;
}

static void put_context(esy_credentials_store_t *store,
                         const credentials_store_ctx_t *ctx) {
  memcpy(store->storage.bytes, ctx, sizeof(*ctx));
}

static esy_err_t validate_bundle(const esy_credentials_config_t *bundle) {
  esy_credentials_t temporary = {0};
  esy_err_t result = esy_credentials_init(&temporary, bundle);
  if (result == ESY_OK) (void)esy_credentials_deinit(&temporary);
  return result;
}

static void bundle_blobs(const esy_credentials_config_t *bundle,
                           const esy_credentials_blob_t *blobs[BUNDLE_BLOB_COUNT]) {
  blobs[0] = &bundle->trust_anchors;
  blobs[1] = &bundle->client_certificate;
  blobs[2] = &bundle->private_key;
}

static esy_err_t read_bundle(const uint8_t *bytes, size_t end, size_t descriptor,
                              uint64_t id, size_t *offset,
                              esy_credentials_config_t *bundle, size_t *size) {
  esy_credentials_blob_t *blobs[BUNDLE_BLOB_COUNT] = {
      &bundle->trust_anchors, &bundle->client_certificate, &bundle->private_key};
  size_t start = *offset;
  for (size_t i = 0u; i < BUNDLE_BLOB_COUNT; ++i) {
    const uint8_t *entry = bytes + descriptor + i * BLOB_DESCRIPTOR_BYTES;
    uint32_t length = esy_read_u32_le(entry);
    uint32_t format = esy_read_u32_le(entry + FORMAT_OFFSET);
    if ((id == 0u && (length != 0u || format != 0u)) ||
        format > ESY_CREDENTIALS_FORMAT_PEM || length > end - *offset)
      return ESY_ERR_CRC_FAIL;
    *blobs[i] = (esy_credentials_blob_t){
        length != 0u ? bytes + *offset : NULL, length,
        (esy_credentials_format_t)format};
    *offset += length;
  }
  *size = *offset - start;
  if (id != 0u && validate_bundle(bundle) != ESY_OK) return ESY_ERR_CRC_FAIL;
  return ESY_OK;
}

static esy_err_t parse_snapshot(const uint8_t *bytes, size_t size, size_t capacity,
                                 esy_credentials_store_info_t *info,
                                 snapshot_view_t *view) {
  if (size < HEADER_BYTES + CRC_BYTES) return ESY_ERR_CRC_FAIL;
  if (esy_read_u32_le(bytes) != STORE_MAGIC ||
      esy_read_u32_le(bytes + VERSION_OFFSET) != STORE_VERSION)
    return ESY_ERR_NOT_SUPPORTED;
  if (size > capacity || esy_read_u32_le(bytes + LENGTH_OFFSET) != size ||
      esy_read_u32_le(bytes + RESERVED_OFFSET) != 0u ||
      !esy_crc32_validate_tail_le(bytes, size)) return ESY_ERR_CRC_FAIL;

  *info = (esy_credentials_store_info_t){
      .state = ESY_CREDENTIALS_STORE_READY,
      .has_snapshot = true,
      .transaction_id = read_counter(bytes + TRANSACTION_OFFSET),
      .last_issued_id = read_counter(bytes + LAST_ID_OFFSET),
      .confirmed_id = read_counter(bytes + CONFIRMED_ID_OFFSET),
      .candidate_id = read_counter(bytes + CANDIDATE_ID_OFFSET),
      .used_bytes = size,
      .capacity = capacity};
  if (info->transaction_id == 0u || info->last_issued_id == 0u ||
      info->last_issued_id > info->transaction_id ||
      info->confirmed_id > info->last_issued_id ||
      (info->candidate_id != 0u &&
       (info->candidate_id != info->last_issued_id ||
        info->candidate_id <= info->confirmed_id))) return ESY_ERR_CRC_FAIL;

  size_t offset = HEADER_BYTES;
  size_t end = size - CRC_BYTES;
  esy_err_t result = read_bundle(bytes, end, CONFIRMED_DESCRIPTOR_OFFSET,
      info->confirmed_id, &offset, &view->confirmed, &info->confirmed_size);
  if (result == ESY_OK)
    result = read_bundle(bytes, end, CANDIDATE_DESCRIPTOR_OFFSET,
        info->candidate_id, &offset, &view->candidate, &info->candidate_size);
  if (result == ESY_OK && offset != end) result = ESY_ERR_CRC_FAIL;
  return result;
}

static void write_bundle(uint8_t *bytes, size_t descriptor, size_t *offset,
                           const esy_credentials_config_t *bundle) {
  if (bundle == NULL) return;
  const esy_credentials_blob_t *blobs[BUNDLE_BLOB_COUNT];
  bundle_blobs(bundle, blobs);
  for (size_t i = 0u; i < BUNDLE_BLOB_COUNT; ++i) {
    uint8_t *entry = bytes + descriptor + i * BLOB_DESCRIPTOR_BYTES;
    esy_write_u32_le(entry, (uint32_t)blobs[i]->size);
    esy_write_u32_le(entry + FORMAT_OFFSET, (uint32_t)blobs[i]->format);
    if (blobs[i]->size != 0u) memcpy(bytes + *offset, blobs[i]->data, blobs[i]->size);
    *offset += blobs[i]->size;
  }
}

static size_t build_snapshot(credentials_store_ctx_t *ctx, uint64_t last_id,
                               uint64_t confirmed_id, uint64_t candidate_id,
                               const esy_credentials_config_t *confirmed,
                               const esy_credentials_config_t *candidate) {
  clear_bytes(ctx->scratch, ctx->buffer_size);
  esy_write_u32_le(ctx->scratch, STORE_MAGIC);
  esy_write_u32_le(ctx->scratch + VERSION_OFFSET, STORE_VERSION);
  write_counter(ctx->scratch + TRANSACTION_OFFSET, ctx->info.transaction_id + 1u);
  write_counter(ctx->scratch + LAST_ID_OFFSET, last_id);
  write_counter(ctx->scratch + CONFIRMED_ID_OFFSET, confirmed_id);
  write_counter(ctx->scratch + CANDIDATE_ID_OFFSET, candidate_id);
  size_t offset = HEADER_BYTES;
  write_bundle(ctx->scratch, CONFIRMED_DESCRIPTOR_OFFSET, &offset, confirmed);
  write_bundle(ctx->scratch, CANDIDATE_DESCRIPTOR_OFFSET, &offset, candidate);
  esy_write_u32_le(ctx->scratch + LENGTH_OFFSET, (uint32_t)(offset + CRC_BYTES));
  esy_crc32_fill_tail_le(ctx->scratch, offset);
  return offset + CRC_BYTES;
}

static esy_err_t read_file(credentials_store_ctx_t *ctx) {
  esy_file_info_t file_info;
  esy_err_t result = esy_file_get_info(ctx->file, &file_info);
  if (result != ESY_OK) return result;
  uint32_t required = ESY_FILE_CAP_READ | ESY_FILE_CAP_ATOMIC_REPLACE;
  if ((file_info.capabilities & required) != required) return ESY_ERR_NOT_SUPPORTED;
  size_t capacity = file_info.capacity < ctx->buffer_size
                        ? file_info.capacity : ctx->buffer_size;
  if (capacity < HEADER_BYTES + CRC_BYTES) return ESY_ERR_NO_MEM;
  esy_credentials_store_info_t next = {.capacity = capacity};
  if (!file_info.exists) {
    if (ctx->info.has_snapshot) return ESY_ERR_STATE;
  } else {
    if (file_info.length > capacity) return ESY_ERR_NO_MEM;
    result = esy_file_read(ctx->file, 0u, ctx->scratch, file_info.length);
    snapshot_view_t view;
    if (result == ESY_OK)
      result = parse_snapshot(ctx->scratch, file_info.length, capacity, &next, &view);
    if (result != ESY_OK) return result;
    if (next.transaction_id < ctx->info.transaction_id ||
        next.last_issued_id < ctx->info.last_issued_id) return ESY_ERR_STATE;
    if (ctx->info.has_snapshot && next.transaction_id == ctx->info.transaction_id &&
        (next.used_bytes != ctx->info.used_bytes ||
         memcmp(ctx->scratch, ctx->working, next.used_bytes) != 0)) return ESY_ERR_STATE;
  }
  clear_bytes(ctx->working, ctx->buffer_size);
  if (next.used_bytes != 0u) memcpy(ctx->working, ctx->scratch, next.used_bytes);
  ctx->info = next;
  return ESY_OK;
}

static esy_err_t commit_snapshot(esy_credentials_store_t *store,
                                  credentials_store_ctx_t *ctx, size_t length) {
  esy_credentials_store_info_t next;
  snapshot_view_t view;
  esy_err_t result = parse_snapshot(ctx->scratch, length, ctx->info.capacity, &next, &view);
  if (result == ESY_OK) {
    ctx->busy = true;
    put_context(store, ctx);
    result = esy_file_replace(ctx->file, ctx->scratch, length);
    ctx->busy = false;
    if (result == ESY_OK) {
      clear_bytes(ctx->working, ctx->buffer_size);
      memcpy(ctx->working, ctx->scratch, length);
      ctx->info = next;
    } else {
      ctx->info.state = ESY_CREDENTIALS_STORE_UNCERTAIN;
      if (result > ESY_OK) result = ESY_ERR_FAIL;
    }
  }
  clear_bytes(ctx->scratch, ctx->buffer_size);
  put_context(store, ctx);
  return result;
}

esy_err_t esy_credentials_store_init(esy_credentials_store_t *store,
                                    const esy_credentials_store_config_t *config) {
  if (store == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
  if (!bytes_zero(store->storage.bytes, sizeof(store->storage.bytes))) {
    credentials_store_ctx_t live;
    return get_context(store, &live) == ESY_ERR_BUSY ? ESY_ERR_BUSY : ESY_ERR_STATE;
  }
  if (overlaps(config, sizeof(*config), store, sizeof(*store)) || config->file == NULL ||
      config->static_resources == NULL) return ESY_ERR_INVALID_ARG;
  const esy_credentials_store_static_resources_t *resources = config->static_resources;
  if (resources->working_buffer == NULL || resources->scratch_buffer == NULL ||
      resources->buffer_size < HEADER_BYTES + CRC_BYTES || resources->buffer_size > UINT32_MAX)
    return ESY_ERR_INVALID_ARG;
  credentials_store_ctx_t ctx = {
      .owner = store, .file = config->file,
      .working = resources->working_buffer, .scratch = resources->scratch_buffer,
      .buffer_size = resources->buffer_size, .magic = HANDLE_MAGIC};
  if (overlaps(ctx.working, ctx.buffer_size, ctx.scratch, ctx.buffer_size) ||
      overlaps(ctx.owner, sizeof(*store), ctx.file, sizeof(*ctx.file)) ||
      overlaps_resources(&ctx, config, sizeof(*config)) ||
      overlaps_resources(&ctx, resources, sizeof(*resources)) ||
      overlaps(ctx.working, ctx.buffer_size, store, sizeof(*store)) ||
      overlaps(ctx.scratch, ctx.buffer_size, store, sizeof(*store)) ||
      overlaps(ctx.working, ctx.buffer_size, ctx.file, sizeof(*ctx.file)) ||
      overlaps(ctx.scratch, ctx.buffer_size, ctx.file, sizeof(*ctx.file)))
    return ESY_ERR_INVALID_ARG;
  ctx.busy = true;
  put_context(store, &ctx);
  esy_err_t result = read_file(&ctx);
  ctx.busy = false;
  clear_bytes(ctx.scratch, ctx.buffer_size);
  if (result == ESY_OK) put_context(store, &ctx);
  else {
    clear_bytes(ctx.working, ctx.buffer_size);
    clear_bytes(store, sizeof(*store));
  }
  return result;
}

esy_err_t esy_credentials_store_deinit(esy_credentials_store_t *store) {
  if (store == NULL) return ESY_ERR_INVALID_ARG;
  if (bytes_zero(store->storage.bytes, sizeof(store->storage.bytes))) return ESY_OK;
  credentials_store_ctx_t ctx;
  esy_err_t result = get_context(store, &ctx);
  if (result != ESY_OK) return result;
  clear_bytes(ctx.working, ctx.buffer_size);
  clear_bytes(ctx.scratch, ctx.buffer_size);
  clear_bytes(store, sizeof(*store));
  return ESY_OK;
}

esy_err_t esy_credentials_store_get_info(const esy_credentials_store_t *store,
                                        esy_credentials_store_info_t *info) {
  if (info == NULL) return ESY_ERR_INVALID_ARG;
  credentials_store_ctx_t ctx;
  esy_err_t result = get_context(store, &ctx);
  if (result != ESY_OK) return result;
  if (overlaps_resources(&ctx, info, sizeof(*info))) return ESY_ERR_INVALID_ARG;
  *info = ctx.info;
  return ESY_OK;
}

esy_err_t esy_credentials_store_load(const esy_credentials_store_t *store,
                                    esy_credentials_store_slot_t slot,
                                    const esy_credentials_store_load_resources_t *resources,
                                    esy_credentials_config_t *config, uint64_t *bundle_id) {
  if (resources == NULL || config == NULL || bundle_id == NULL ||
      (slot != ESY_CREDENTIALS_STORE_CONFIRMED && slot != ESY_CREDENTIALS_STORE_CANDIDATE))
    return ESY_ERR_INVALID_ARG;
  credentials_store_ctx_t ctx;
  esy_err_t result = get_context(store, &ctx);
  if (result != ESY_OK) return result;
  if (overlaps_resources(&ctx, resources, sizeof(*resources)) ||
      overlaps_resources(&ctx, config, sizeof(*config)) ||
      overlaps_resources(&ctx, bundle_id, sizeof(*bundle_id)) ||
      overlaps(config, sizeof(*config), bundle_id, sizeof(*bundle_id)) ||
      overlaps(resources, sizeof(*resources), config, sizeof(*config)) ||
      overlaps(resources, sizeof(*resources), bundle_id, sizeof(*bundle_id)))
    return ESY_ERR_INVALID_ARG;
  if (ctx.info.state != ESY_CREDENTIALS_STORE_READY || !ctx.info.has_snapshot)
    return ESY_ERR_STATE;
  uint64_t id = slot == ESY_CREDENTIALS_STORE_CONFIRMED
                    ? ctx.info.confirmed_id : ctx.info.candidate_id;
  if (id == 0u) return ESY_ERR_STATE;
  if ((resources->buffer == NULL && resources->buffer_size != 0u) ||
      overlaps_resources(&ctx, resources->buffer, resources->buffer_size) ||
      overlaps(resources->buffer, resources->buffer_size, resources, sizeof(*resources)) ||
      overlaps(resources->buffer, resources->buffer_size, config, sizeof(*config)) ||
      overlaps(resources->buffer, resources->buffer_size, bundle_id, sizeof(*bundle_id)))
    return ESY_ERR_INVALID_ARG;
  snapshot_view_t view;
  esy_credentials_store_info_t ignored;
  result = parse_snapshot(ctx.working, ctx.info.used_bytes, ctx.info.capacity, &ignored, &view);
  if (result != ESY_OK) return result;
  esy_credentials_config_t next = slot == ESY_CREDENTIALS_STORE_CONFIRMED
                                    ? view.confirmed : view.candidate;
  size_t size = slot == ESY_CREDENTIALS_STORE_CONFIRMED
                    ? ctx.info.confirmed_size : ctx.info.candidate_size;
  if (size > resources->buffer_size) return ESY_ERR_NO_MEM;
  esy_credentials_blob_t *blobs[BUNDLE_BLOB_COUNT] = {
      &next.trust_anchors, &next.client_certificate, &next.private_key};
  size_t offset = 0u;
  for (size_t i = 0u; i < BUNDLE_BLOB_COUNT; ++i) {
    if (blobs[i]->size != 0u) {
      memcpy(resources->buffer + offset, blobs[i]->data, blobs[i]->size);
      blobs[i]->data = resources->buffer + offset;
      offset += blobs[i]->size;
    }
  }
  *config = next;
  *bundle_id = id;
  return ESY_OK;
}

esy_err_t esy_credentials_store_stage(esy_credentials_store_t *store,
                                     const esy_credentials_config_t *candidate,
                                     uint64_t *candidate_id) {
  if (candidate == NULL || candidate_id == NULL) return ESY_ERR_INVALID_ARG;
  credentials_store_ctx_t ctx;
  esy_err_t result = get_context(store, &ctx);
  if (result != ESY_OK) return result;
  if (ctx.info.state != ESY_CREDENTIALS_STORE_READY) return ESY_ERR_STATE;
  if (ctx.info.candidate_id != 0u) return ESY_ERR_BUSY;
  /* Reserve a subsequent commit so successful staging can still be finalized. */
  if (ctx.info.transaction_id >= UINT64_MAX - 1u || ctx.info.last_issued_id == UINT64_MAX)
    return ESY_ERR_STATE;
  if (overlaps_resources(&ctx, candidate, sizeof(*candidate)) ||
      overlaps_resources(&ctx, candidate_id, sizeof(*candidate_id)) ||
      overlaps(candidate, sizeof(*candidate), candidate_id, sizeof(*candidate_id)))
    return ESY_ERR_INVALID_ARG;
  size_t size = HEADER_BYTES + CRC_BYTES + ctx.info.confirmed_size;
  const esy_credentials_blob_t *blobs[BUNDLE_BLOB_COUNT];
  bundle_blobs(candidate, blobs);
  for (size_t i = 0u; i < BUNDLE_BLOB_COUNT; ++i) {
    if ((blobs[i]->data == NULL) != (blobs[i]->size == 0u) ||
        overlaps_resources(&ctx, blobs[i]->data, blobs[i]->size) ||
        overlaps(blobs[i]->data, blobs[i]->size, candidate_id, sizeof(*candidate_id)))
      return ESY_ERR_INVALID_ARG;
    if (blobs[i]->size > ctx.info.capacity - size) return ESY_ERR_NO_MEM;
    size += blobs[i]->size;
  }
  result = validate_bundle(candidate);
  if (result != ESY_OK) return result;
  snapshot_view_t view = {0};
  esy_credentials_store_info_t ignored;
  if (ctx.info.has_snapshot) {
    result = parse_snapshot(ctx.working, ctx.info.used_bytes, ctx.info.capacity, &ignored, &view);
    if (result != ESY_OK) return result;
  }
  uint64_t next_id = ctx.info.last_issued_id + 1u;
  size = build_snapshot(&ctx, next_id, ctx.info.confirmed_id, next_id,
      ctx.info.confirmed_id != 0u ? &view.confirmed : NULL, candidate);
  result = commit_snapshot(store, &ctx, size);
  if (result == ESY_OK) *candidate_id = next_id;
  return result;
}

static esy_err_t finish_candidate(esy_credentials_store_t *store,
                                   uint64_t candidate_id, bool confirm) {
  if (candidate_id == 0u) return ESY_ERR_INVALID_ARG;
  credentials_store_ctx_t ctx;
  esy_err_t result = get_context(store, &ctx);
  if (result != ESY_OK) return result;
  if (ctx.info.state != ESY_CREDENTIALS_STORE_READY ||
      ctx.info.candidate_id != candidate_id || ctx.info.transaction_id == UINT64_MAX)
    return ESY_ERR_STATE;
  snapshot_view_t view;
  esy_credentials_store_info_t ignored;
  result = parse_snapshot(ctx.working, ctx.info.used_bytes, ctx.info.capacity, &ignored, &view);
  if (result != ESY_OK) return result;
  const esy_credentials_config_t *confirmed = confirm ? &view.candidate :
      (ctx.info.confirmed_id != 0u ? &view.confirmed : NULL);
  uint64_t confirmed_id = confirm ? candidate_id : ctx.info.confirmed_id;
  size_t size = build_snapshot(&ctx, ctx.info.last_issued_id, confirmed_id, 0u, confirmed, NULL);
  return commit_snapshot(store, &ctx, size);
}

esy_err_t esy_credentials_store_confirm(esy_credentials_store_t *store,
                                       uint64_t candidate_id) {
  return finish_candidate(store, candidate_id, true);
}

esy_err_t esy_credentials_store_discard(esy_credentials_store_t *store,
                                       uint64_t candidate_id) {
  return finish_candidate(store, candidate_id, false);
}

esy_err_t esy_credentials_store_reload(esy_credentials_store_t *store) {
  credentials_store_ctx_t ctx;
  esy_err_t result = get_context(store, &ctx);
  if (result != ESY_OK) return result;
  ctx.busy = true;
  put_context(store, &ctx);
  result = read_file(&ctx);
  ctx.busy = false;
  if (result != ESY_OK) ctx.info.state = ESY_CREDENTIALS_STORE_UNCERTAIN;
  clear_bytes(ctx.scratch, ctx.buffer_size);
  put_context(store, &ctx);
  return result;
}
