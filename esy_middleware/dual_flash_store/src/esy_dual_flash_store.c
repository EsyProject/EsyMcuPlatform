/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/dual_flash_store/esy_dual_flash_store.h"

#include <string.h>

#include "esy_common/utils/integrity/esy_crc32.h"
#include "esy_hal/esy_hal_flash.h"

#define RECORD_HEADER_BYTES 32u
#define RECORD_COMMIT_BYTES 32u
#define RECORD_VERSION_OFFSET 4u
#define RECORD_GENERATION_OFFSET 8u
#define RECORD_LENGTH_OFFSET 16u
#define RECORD_KIND_OFFSET 20u
#define RECORD_PAYLOAD_CRC_OFFSET 24u
#define RECORD_HEADER_CRC_OFFSET 28u
#define COMMIT_HEADER_CRC_OFFSET 16u
#define COMMIT_PAYLOAD_CRC_OFFSET 20u
#define COMMIT_LENGTH_OFFSET 24u
#define COMMIT_CRC_OFFSET 28u

enum {
  RECORD_VERSION = 1,
  RECORD_KIND_DATA = 1,
  RECORD_KIND_CLEAR = 2
};

static const uint32_t k_record_magic = UINT32_C(0x52595345);
static const uint32_t k_commit_magic = UINT32_C(0x54494D43);
static const uint32_t k_handle_magic = UINT32_C(0x52535452);

typedef struct {
  uintptr_t owner;
  uint32_t magic;
  esy_dual_flash_store_region_t regions[ESY_DUAL_FLASH_STORE_REGION_COUNT];
  uint8_t *work_buffer;
  size_t work_buffer_size;
  uint32_t program_size;
  uint32_t header_span;
  uint32_t commit_span;
  uint32_t capacity;
} dual_flash_store_ctx_t;

typedef struct {
  bool valid;
  uint32_t index;
  uint32_t length;
  uint32_t kind;
  uint32_t payload_crc;
  uint32_t header_crc;
  uint64_t generation;
} record_state_t;

_Static_assert(sizeof(dual_flash_store_ctx_t) <= ESY_DUAL_FLASH_STORE_STORAGE_BYTES,
               "Dual Flash store handle capacity is insufficient");

static bool bytes_are(const void *data, size_t length, uint8_t value) {
  const uint8_t *bytes = data;
  for (size_t i = 0u; i < length; ++i) {
    if (bytes[i] != value) return false;
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

static bool overlaps_resources(const dual_flash_store_ctx_t *ctx,
                               const void *data, size_t length) {
  return ranges_overlap(data, length, (const void *)ctx->owner,
                        sizeof(esy_dual_flash_store_t)) ||
         ranges_overlap(data, length, ctx->work_buffer, ctx->work_buffer_size);
}

static uint64_t read_generation(const uint8_t *bytes) {
  return (uint64_t)esy_read_u32_le(bytes) |
         ((uint64_t)esy_read_u32_le(bytes + sizeof(uint32_t)) << 32u);
}

static void write_generation(uint8_t *bytes, uint64_t value) {
  esy_write_u32_le(bytes, (uint32_t)value);
  esy_write_u32_le(bytes + sizeof(uint32_t), (uint32_t)(value >> 32u));
}

static esy_err_t load_context(const esy_dual_flash_store_t *store,
                              dual_flash_store_ctx_t *ctx) {
  if (store == NULL) return ESY_ERR_INVALID_ARG;
  memcpy(ctx, store->storage.bytes, sizeof(*ctx));
  if (ctx->magic != k_handle_magic || ctx->owner != (uintptr_t)store) {
    return ESY_ERR_STATE;
  }
  return ESY_OK;
}

static esy_err_t validate_region(const esy_dual_flash_store_region_t *region,
                                 const esy_flash_geometry_t *geometry) {
  const uint64_t end = (uint64_t)region->address + region->size_bytes;
  const uint64_t physical_end = (uint64_t)geometry->base_address + geometry->size_bytes;
  if (region->size_bytes == 0u || region->address < geometry->base_address ||
      end > physical_end || end > (uint64_t)UINT32_MAX + 1u ||
      region->address % geometry->program_size != 0u ||
      region->size_bytes % geometry->program_size != 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  for (uint64_t cursor = region->address; cursor < end;) {
    esy_flash_sector_t sector;
    esy_err_t result = esy_hal_flash_get_sector((uint32_t)cursor, &sector);
    if (result != ESY_OK) return result;
    if (sector.start_address != cursor || sector.size_bytes == 0u ||
        sector.size_bytes > end - cursor) return ESY_ERR_INVALID_ARG;
    cursor += sector.size_bytes;
  }
  return ESY_OK;
}

static esy_err_t configure_context(esy_dual_flash_store_t *store,
                                   const esy_dual_flash_store_config_t *config,
                                   dual_flash_store_ctx_t *ctx) {
  if (config == NULL || config->static_resources == NULL ||
      config->static_resources->work_buffer == NULL) return ESY_ERR_INVALID_ARG;
  esy_flash_geometry_t geometry;
  esy_err_t result = esy_hal_flash_get_geometry(&geometry);
  if (result != ESY_OK) return result;
  if (geometry.program_size == 0u || geometry.size_bytes == 0u ||
      (uint64_t)geometry.base_address + geometry.size_bytes >
          (uint64_t)UINT32_MAX + 1u) return ESY_ERR_NOT_SUPPORTED;

  *ctx = (dual_flash_store_ctx_t){
      .owner = (uintptr_t)store,
      .magic = k_handle_magic,
      .work_buffer = config->static_resources->work_buffer,
      .work_buffer_size = config->static_resources->work_buffer_size,
      .program_size = geometry.program_size};
  memcpy(ctx->regions, config->regions, sizeof(ctx->regions));
  if (ctx->work_buffer_size < ctx->program_size) return ESY_ERR_NO_MEM;
  if (ranges_overlap(store, sizeof(*store), ctx->work_buffer, ctx->work_buffer_size)) {
    return ESY_ERR_INVALID_ARG;
  }

  const uint64_t header_span =
      ((uint64_t)RECORD_HEADER_BYTES + ctx->program_size - 1u) /
      ctx->program_size * ctx->program_size;
  const uint64_t commit_span =
      ((uint64_t)RECORD_COMMIT_BYTES + ctx->program_size - 1u) /
      ctx->program_size * ctx->program_size;
  if (header_span > UINT32_MAX || commit_span > UINT32_MAX) return ESY_ERR_NO_MEM;
  ctx->header_span = (uint32_t)header_span;
  ctx->commit_span = (uint32_t)commit_span;
  ctx->capacity = UINT32_MAX;
  for (size_t i = 0u; i < ESY_DUAL_FLASH_STORE_REGION_COUNT; ++i) {
    result = validate_region(&ctx->regions[i], &geometry);
    if (result != ESY_OK) return result;
    if ((uint64_t)ctx->regions[i].size_bytes <= header_span + commit_span) {
      return ESY_ERR_NO_MEM;
    }
    uint32_t capacity = ctx->regions[i].size_bytes - ctx->header_span - ctx->commit_span;
    if (capacity < ctx->capacity) ctx->capacity = capacity;
  }
  const uint64_t end0 = (uint64_t)ctx->regions[0].address + ctx->regions[0].size_bytes;
  const uint64_t end1 = (uint64_t)ctx->regions[1].address + ctx->regions[1].size_bytes;
  if ((uint64_t)ctx->regions[0].address < end1 &&
      (uint64_t)ctx->regions[1].address < end0) return ESY_ERR_INVALID_ARG;
  return ESY_OK;
}

static esy_err_t payload_crc(const dual_flash_store_ctx_t *ctx, uint32_t address,
                             uint32_t length, uint32_t *crc) {
  uint32_t state = UINT32_MAX;
  size_t remaining = length;
  while (remaining != 0u) {
    size_t chunk = remaining < ctx->work_buffer_size ? remaining : ctx->work_buffer_size;
    esy_err_t result = esy_hal_flash_read(address, ctx->work_buffer, chunk);
    if (result != ESY_OK) return result;
    state = esy_crc32(state, ctx->work_buffer, chunk);
    remaining -= chunk;
    if (remaining != 0u) address += (uint32_t)chunk;
  }
  *crc = ~state;
  return ESY_OK;
}

static esy_err_t scan_region(const dual_flash_store_ctx_t *ctx, uint32_t index,
                             record_state_t *record) {
  uint8_t commit[RECORD_COMMIT_BYTES];
  uint8_t header[RECORD_HEADER_BYTES];
  const esy_dual_flash_store_region_t *region = &ctx->regions[index];
  uint32_t commit_address = region->address + region->size_bytes - ctx->commit_span;
  *record = (record_state_t){.index = index};
  esy_err_t result = esy_hal_flash_read(commit_address, commit, sizeof(commit));
  if (result != ESY_OK) return result;
  if (bytes_are(commit, sizeof(commit), UINT8_MAX) ||
      esy_crc32_finalize(commit, COMMIT_CRC_OFFSET) !=
          esy_read_u32_le(commit + COMMIT_CRC_OFFSET)) return ESY_OK;
  if (esy_read_u32_le(commit) != k_commit_magic ||
      esy_read_u32_le(commit + RECORD_VERSION_OFFSET) != RECORD_VERSION) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  result = esy_hal_flash_read(region->address, header, sizeof(header));
  if (result != ESY_OK) return result;
  uint32_t header_crc = esy_crc32_finalize(header, RECORD_HEADER_CRC_OFFSET);
  if (header_crc != esy_read_u32_le(header + RECORD_HEADER_CRC_OFFSET)) return ESY_OK;
  if (esy_read_u32_le(header) != k_record_magic ||
      esy_read_u32_le(header + RECORD_VERSION_OFFSET) != RECORD_VERSION) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  record->generation = read_generation(header + RECORD_GENERATION_OFFSET);
  record->length = esy_read_u32_le(header + RECORD_LENGTH_OFFSET);
  record->kind = esy_read_u32_le(header + RECORD_KIND_OFFSET);
  record->payload_crc = esy_read_u32_le(header + RECORD_PAYLOAD_CRC_OFFSET);
  record->header_crc = header_crc;
  if (record->generation == 0u || record->length > ctx->capacity ||
      (record->kind != RECORD_KIND_DATA && record->kind != RECORD_KIND_CLEAR) ||
      (record->kind == RECORD_KIND_CLEAR && record->length != 0u) ||
      record->generation != read_generation(commit + RECORD_GENERATION_OFFSET) ||
      header_crc != esy_read_u32_le(commit + COMMIT_HEADER_CRC_OFFSET) ||
      record->payload_crc != esy_read_u32_le(commit + COMMIT_PAYLOAD_CRC_OFFSET) ||
      record->length != esy_read_u32_le(commit + COMMIT_LENGTH_OFFSET)) return ESY_OK;
  uint32_t actual_crc;
  result = payload_crc(ctx, region->address + ctx->header_span,
                       record->length, &actual_crc);
  if (result != ESY_OK) return result;
  record->valid = actual_crc == record->payload_crc;
  return ESY_OK;
}

static esy_err_t scan_latest(const dual_flash_store_ctx_t *ctx, record_state_t *latest) {
  record_state_t records[ESY_DUAL_FLASH_STORE_REGION_COUNT];
  for (uint32_t i = 0u; i < ESY_DUAL_FLASH_STORE_REGION_COUNT; ++i) {
    esy_err_t result = scan_region(ctx, i, &records[i]);
    if (result != ESY_OK) return result;
  }
  if (records[0].valid && records[1].valid) {
    uint32_t newer = records[1].generation > records[0].generation ? 1u : 0u;
    if (records[newer].generation - records[1u - newer].generation != 1u) {
      return ESY_ERR_CRC_FAIL;
    }
    *latest = records[newer];
  } else {
    *latest = records[0].valid ? records[0] : records[1];
  }
  return ESY_OK;
}

esy_err_t esy_dual_flash_store_init(esy_dual_flash_store_t *store,
                              const esy_dual_flash_store_config_t *config) {
  if (store == NULL) return ESY_ERR_INVALID_ARG;
  if (!bytes_are(store->storage.bytes, sizeof(store->storage.bytes), 0u)) {
    return ESY_ERR_STATE;
  }
  dual_flash_store_ctx_t ctx;
  esy_err_t result = configure_context(store, config, &ctx);
  if (result != ESY_OK) return result;
  record_state_t latest;
  result = scan_latest(&ctx, &latest);
  if (result == ESY_OK) memcpy(store->storage.bytes, &ctx, sizeof(ctx));
  return result;
}

esy_err_t esy_dual_flash_store_deinit(esy_dual_flash_store_t *store) {
  if (store == NULL) return ESY_ERR_INVALID_ARG;
  if (bytes_are(store->storage.bytes, sizeof(store->storage.bytes), 0u)) return ESY_OK;
  dual_flash_store_ctx_t ctx;
  esy_err_t result = load_context(store, &ctx);
  if (result == ESY_OK) memset(store, 0, sizeof(*store));
  return result;
}

esy_err_t esy_dual_flash_store_get_info(esy_dual_flash_store_t *store,
                                  esy_dual_flash_store_info_t *info) {
  dual_flash_store_ctx_t ctx;
  esy_err_t result = load_context(store, &ctx);
  if (result != ESY_OK) return result;
  if (info == NULL || overlaps_resources(&ctx, info, sizeof(*info))) {
    return ESY_ERR_INVALID_ARG;
  }
  record_state_t latest;
  result = scan_latest(&ctx, &latest);
  if (result != ESY_OK) return result;
  *info = (esy_dual_flash_store_info_t){
      .has_record = latest.valid && latest.kind == RECORD_KIND_DATA,
      .length = latest.valid ? latest.length : 0u,
      .capacity = ctx.capacity,
      .generation = latest.valid ? latest.generation : 0u};
  return ESY_OK;
}

esy_err_t esy_dual_flash_store_read(esy_dual_flash_store_t *store, void *buffer,
                              size_t capacity, size_t *length) {
  dual_flash_store_ctx_t ctx;
  esy_err_t result = load_context(store, &ctx);
  if (result != ESY_OK) return result;
  if (length == NULL || (buffer == NULL && capacity != 0u) ||
      overlaps_resources(&ctx, length, sizeof(*length)) ||
      overlaps_resources(&ctx, buffer, capacity) ||
      ranges_overlap(buffer, capacity, length, sizeof(*length))) {
    return ESY_ERR_INVALID_ARG;
  }
  record_state_t latest;
  result = scan_latest(&ctx, &latest);
  if (result != ESY_OK) return result;
  if (!latest.valid || latest.kind != RECORD_KIND_DATA) return ESY_ERR_STATE;
  if (capacity < latest.length) return ESY_ERR_NO_MEM;
  if (latest.length != 0u) {
    result = esy_hal_flash_read(ctx.regions[latest.index].address + ctx.header_span,
                                buffer, latest.length);
    if (result != ESY_OK) return result;
  }
  *length = latest.length;
  return ESY_OK;
}

esy_err_t esy_dual_flash_store_read_range(esy_dual_flash_store_t *store, uint32_t offset,
                                     void *buffer, size_t length) {
  dual_flash_store_ctx_t ctx;
  esy_err_t result = load_context(store, &ctx);
  if (result != ESY_OK) return result;
  if ((buffer == NULL && length != 0u) || length > UINT32_MAX - offset ||
      overlaps_resources(&ctx, buffer, length)) return ESY_ERR_INVALID_ARG;
  record_state_t latest;
  result = scan_latest(&ctx, &latest);
  if (result != ESY_OK) return result;
  if (!latest.valid || latest.kind != RECORD_KIND_DATA) return ESY_ERR_STATE;
  if (offset > latest.length || length > latest.length - offset) return ESY_ERR_INVALID_ARG;
  return length == 0u ? ESY_OK : esy_hal_flash_read(
      ctx.regions[latest.index].address + ctx.header_span + offset, buffer, length);
}

static void build_header(uint8_t *header, uint64_t generation, uint32_t kind,
                          uint32_t length, uint32_t crc) {
  memset(header, 0, RECORD_HEADER_BYTES);
  esy_write_u32_le(header, k_record_magic);
  esy_write_u32_le(header + RECORD_VERSION_OFFSET, RECORD_VERSION);
  write_generation(header + RECORD_GENERATION_OFFSET, generation);
  esy_write_u32_le(header + RECORD_LENGTH_OFFSET, length);
  esy_write_u32_le(header + RECORD_KIND_OFFSET, kind);
  esy_write_u32_le(header + RECORD_PAYLOAD_CRC_OFFSET, crc);
  esy_write_u32_le(header + RECORD_HEADER_CRC_OFFSET,
                   esy_crc32_finalize(header, RECORD_HEADER_CRC_OFFSET));
}

static void build_commit(uint8_t *commit, const uint8_t *header) {
  memset(commit, 0, RECORD_COMMIT_BYTES);
  esy_write_u32_le(commit, k_commit_magic);
  esy_write_u32_le(commit + RECORD_VERSION_OFFSET, RECORD_VERSION);
  write_generation(commit + RECORD_GENERATION_OFFSET,
                    read_generation(header + RECORD_GENERATION_OFFSET));
  esy_write_u32_le(commit + COMMIT_HEADER_CRC_OFFSET,
                    esy_read_u32_le(header + RECORD_HEADER_CRC_OFFSET));
  esy_write_u32_le(commit + COMMIT_PAYLOAD_CRC_OFFSET,
                    esy_read_u32_le(header + RECORD_PAYLOAD_CRC_OFFSET));
  esy_write_u32_le(commit + COMMIT_LENGTH_OFFSET,
                    esy_read_u32_le(header + RECORD_LENGTH_OFFSET));
  esy_write_u32_le(commit + COMMIT_CRC_OFFSET,
                   esy_crc32_finalize(commit, COMMIT_CRC_OFFSET));
}

static esy_err_t program_padded(const dual_flash_store_ctx_t *ctx, uint32_t address,
                                const uint8_t *data, uint32_t length) {
  uint32_t offset = 0u;
  while (offset < length) {
    uint32_t chunk = length - offset;
    if (chunk > ctx->program_size) chunk = ctx->program_size;
    memset(ctx->work_buffer, UINT8_MAX, ctx->program_size);
    memcpy(ctx->work_buffer, data + offset, chunk);
    esy_err_t result = esy_hal_flash_write(address, ctx->work_buffer, ctx->program_size);
    if (result != ESY_OK) return result;
    offset += chunk;
    if (offset < length) address += ctx->program_size;
  }
  return ESY_OK;
}

static esy_err_t verify_body(const dual_flash_store_ctx_t *ctx, uint32_t index,
                             const uint8_t *header) {
  uint8_t actual_header[RECORD_HEADER_BYTES];
  uint32_t address = ctx->regions[index].address;
  esy_err_t result = esy_hal_flash_read(address, actual_header, sizeof(actual_header));
  if (result != ESY_OK) return result;
  if (memcmp(header, actual_header, sizeof(actual_header)) != 0) return ESY_ERR_CRC_FAIL;
  uint32_t actual_crc;
  result = payload_crc(ctx, address + ctx->header_span,
                       esy_read_u32_le(header + RECORD_LENGTH_OFFSET), &actual_crc);
  if (result != ESY_OK) return result;
  return actual_crc == esy_read_u32_le(header + RECORD_PAYLOAD_CRC_OFFSET) ?
         ESY_OK : ESY_ERR_CRC_FAIL;
}

static esy_err_t replace_record(const dual_flash_store_ctx_t *ctx, const void *data,
                                uint32_t length, uint32_t kind) {
  record_state_t latest;
  esy_err_t result = scan_latest(ctx, &latest);
  if (result != ESY_OK) return result;
  if (kind == RECORD_KIND_CLEAR &&
      (!latest.valid || latest.kind == RECORD_KIND_CLEAR)) return ESY_OK;
  if (latest.valid && latest.generation == UINT64_MAX) return ESY_ERR_STATE;

  uint32_t target = latest.valid ? 1u - latest.index : 0u;
  uint64_t generation = latest.valid ? latest.generation + 1u : 1u;
  uint8_t header[RECORD_HEADER_BYTES];
  uint8_t commit[RECORD_COMMIT_BYTES];
  uint32_t crc = ~esy_crc32(UINT32_MAX, data, length);
  build_header(header, generation, kind, length, crc);
  build_commit(commit, header);
  const esy_dual_flash_store_region_t *region = &ctx->regions[target];
  /* Each operation rescans the medium. Even an error after physical completion
   * therefore cannot leave an in-memory generation or selection cache stale. */
  result = esy_hal_flash_erase(region->address, region->size_bytes);
  if (result != ESY_OK) return result;
  result = program_padded(ctx, region->address, header, sizeof(header));
  if (result != ESY_OK) return result;
  result = program_padded(ctx, region->address + ctx->header_span, data, length);
  if (result != ESY_OK) return result;
  result = verify_body(ctx, target, header);
  if (result != ESY_OK) return result;
  result = program_padded(ctx, region->address + region->size_bytes - ctx->commit_span,
                          commit, sizeof(commit));
  if (result != ESY_OK) return result;
  result = scan_latest(ctx, &latest);
  if (result != ESY_OK) return result;
  return latest.valid && latest.index == target && latest.generation == generation ?
         ESY_OK : ESY_ERR_CRC_FAIL;
}

esy_err_t esy_dual_flash_store_write(esy_dual_flash_store_t *store, const void *data,
                               size_t length) {
  dual_flash_store_ctx_t ctx;
  esy_err_t result = load_context(store, &ctx);
  if (result != ESY_OK) return result;
  if ((data == NULL && length != 0u) || overlaps_resources(&ctx, data, length)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (length > ctx.capacity) return ESY_ERR_NO_MEM;
  return replace_record(&ctx, data, (uint32_t)length, RECORD_KIND_DATA);
}

esy_err_t esy_dual_flash_store_clear(esy_dual_flash_store_t *store) {
  dual_flash_store_ctx_t ctx;
  esy_err_t result = load_context(store, &ctx);
  if (result != ESY_OK) return result;
  return replace_record(&ctx, NULL, 0u, RECORD_KIND_CLEAR);
}
