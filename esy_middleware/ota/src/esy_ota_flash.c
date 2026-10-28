/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/ota/esy_ota_flash.h"

#include <string.h>

#include "esy_middleware/bootloader/esy_bootloader.h"
#include "esy_hal/esy_hal_flash.h"
#include "esy_common/utils/integrity/esy_sha256.h"

/* -------------------------------------------------------------------------- */
/* Private context (Scheme C)                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
  bool initialized;
  bool session_active;
  bool verified;
  bool activation_pending_reconcile;

  esy_image_manifest_t metadata;
  uint32_t total_size;

  uint32_t write_offset; /* bytes committed to flash */
  uint32_t erase_offset; /* bytes erased ahead */

  uint32_t base_addr;
  uint32_t slot_size;
  int slot;
  uint32_t write_size;

  uint32_t cache_len;  /* bytes currently in cache */
  uint32_t cache_size; /* caller-owned cache capacity */
  uint8_t *cache;      /* caller-owned program-aligned cache */
} esy_ota_flash_ctx_t;

_Static_assert(ESY_IMAGE_SHA256_BYTES == ESY_FIRMWARE_SHA256_BYTES, "Image hash size mismatch");
_Static_assert(ESY_IMAGE_SIGNATURE_BYTES == ESY_FIRMWARE_SIGNATURE_BYTES, "Image signature size mismatch");
_Static_assert(ESY_OTA_FLASH_STORAGE_BYTES >= sizeof(esy_ota_flash_ctx_t),
               "ESY_OTA_FLASH_STORAGE_BYTES too small");

static inline esy_ota_flash_ctx_t *ota_ctx(esy_ota_flash_t *ota) {
  return (esy_ota_flash_ctx_t *)(void *)ota->storage.bytes;
}

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

static void clear_cached_bytes(esy_ota_flash_ctx_t *ctx) {
  if (!ctx) return;
  ctx->cache_len = 0;
}

static void ota_reset_all(esy_ota_flash_t *ota) {
  if (!ota) return;
  memset(ota, 0, sizeof(*ota));
}

/* Sector queries are validated before erase, including after session start. */
static esy_err_t get_owned_sector(uint32_t base, uint32_t size,
                                  uint32_t offset, esy_flash_sector_t *sector) {
  if (offset >= size) return ESY_ERR_INVALID_ARG;
  esy_err_t ret = esy_hal_flash_get_sector(base + offset, sector);
  if (ret != ESY_OK) return ret;
  if (sector->start_address != base + offset || sector->size_bytes == 0u ||
      sector->size_bytes > size - offset) return ESY_ERR_INVALID_ARG;
  return ESY_OK;
}

static esy_err_t ensure_erased_until(esy_ota_flash_ctx_t *ctx,
                                     uint32_t need_end) {
  if (need_end > ctx->slot_size) return ESY_ERR_INVALID_ARG;
  while (ctx->erase_offset < need_end) {
    esy_flash_sector_t sector;
    esy_err_t ret = get_owned_sector(ctx->base_addr, ctx->slot_size,
                                     ctx->erase_offset, &sector);
    if (ret != ESY_OK) return ret;
    ret = esy_hal_flash_erase(sector.start_address, sector.size_bytes);
    if (ret != ESY_OK) return ret;
    ctx->erase_offset += sector.size_bytes;
  }
  return ESY_OK;
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
  if (alignment == 0u) return 0u;
  const uint32_t remainder = value % alignment;
  if (remainder == 0u) return value;
  const uint32_t padding = alignment - remainder;
  return value > UINT32_MAX - padding ? 0u : value + padding;
}

static esy_err_t write_block(esy_ota_flash_ctx_t *ctx, const uint8_t *data,
                              uint32_t size) {
  if (ctx->write_offset > ctx->slot_size ||
      size > ctx->slot_size - ctx->write_offset) return ESY_ERR_INVALID_ARG;
  esy_err_t ret = ensure_erased_until(ctx, ctx->write_offset + size);
  if (ret != ESY_OK) return ret;
  return esy_hal_flash_write(ctx->base_addr + ctx->write_offset, data, size);
}

static esy_err_t abort_session(esy_ota_flash_ctx_t *ctx, esy_err_t error) {
  clear_cached_bytes(ctx);
  ctx->session_active = false;
  ctx->verified = false;
  return error;
}

static esy_err_t calculate_written_sha256(
    esy_ota_flash_ctx_t *ctx, uint32_t base_addr,
    uint8_t digest[ESY_FIRMWARE_SHA256_BYTES]) {
  esy_sha256_ctx_t sha256;

  if (!ctx || !ctx->cache || ctx->cache_size == 0u) return ESY_ERR_STATE;
  if (digest == NULL) return ESY_ERR_INVALID_ARG;

  esy_sha256_init(&sha256);
  uint32_t offset = 0u;

  while (offset < ctx->total_size) {
    uint32_t remaining = ctx->total_size - offset;
    size_t chunk = (remaining < ctx->cache_size) ? (size_t)remaining
                                                 : (size_t)ctx->cache_size;
    esy_err_t ret =
        esy_hal_flash_read(base_addr + offset, ctx->cache, chunk);
    if (ret != ESY_OK) return ret;

    esy_sha256_update(&sha256, ctx->cache, chunk);
    offset += (uint32_t)chunk;
  }

  esy_sha256_final(&sha256, digest);
  return ESY_OK;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

static esy_err_t provider_begin(void *context,
                                 const esy_image_manifest_t *metadata) {
  if (context == NULL || metadata == NULL || metadata->version == 0u ||
      metadata->image_size == 0u) return ESY_ERR_INVALID_ARG;
  esy_ota_flash_ctx_t *ctx = ota_ctx(context);
  if (!ctx->initialized) return ESY_ERR_STATE;
  if (ctx->activation_pending_reconcile) return ESY_ERR_STATE;
  if (ctx->session_active) return ESY_ERR_BUSY;
  const int slot = esy_bootloader_get_flash_slot();
  const uint32_t base = esy_bootloader_get_flash_slot_addr();
  const uint32_t size = esy_bootloader_get_flash_slot_size();
  if (slot < 0 || size == 0u) return ESY_ERR_FAIL;
  if (metadata->image_size > size) return ESY_ERR_INVALID_ARG;

  esy_flash_geometry_t geometry;
  esy_err_t ret = esy_hal_flash_get_geometry(&geometry);
  if (ret != ESY_OK) return ret;
  const uint64_t flash_end = (uint64_t)geometry.base_address + geometry.size_bytes;
  if (geometry.size_bytes == 0u || geometry.program_size == 0u ||
      flash_end > (uint64_t)UINT32_MAX + 1u || base < geometry.base_address ||
      (uint64_t)base + size > flash_end || base % geometry.program_size != 0u ||
      size % geometry.program_size != 0u || ctx->cache_size % geometry.program_size != 0u)
    return ESY_ERR_INVALID_ARG;
  const uint32_t padded_size = align_up_u32(metadata->image_size, geometry.program_size);
  if (padded_size == 0u || padded_size > size) return ESY_ERR_INVALID_ARG;
  uint32_t offset = 0u;
  while (offset < size) {
    esy_flash_sector_t sector;
    ret = get_owned_sector(base, size, offset, &sector);
    if (ret != ESY_OK) return ret;
    offset += sector.size_bytes;
  }
  ctx->metadata = *metadata;
  ctx->total_size = metadata->image_size;
  ctx->write_offset = 0u;
  ctx->erase_offset = 0u;
  ctx->cache_len = 0u;
  ctx->base_addr = base;
  ctx->slot_size = size;
  ctx->slot = slot;
  ctx->write_size = geometry.program_size;
  ctx->verified = false;
  ctx->session_active = true;
  return ESY_OK;
}

static esy_err_t provider_write(void *context, uint32_t offset,
                                const uint8_t *data, size_t length) {
  if (context == NULL) return ESY_ERR_INVALID_ARG;
  esy_ota_flash_ctx_t *ctx = ota_ctx(context);
  if (!ctx->initialized || !ctx->session_active || ctx->verified) return ESY_ERR_STATE;
  if (offset != ctx->write_offset + ctx->cache_len) return ESY_ERR_STATE;
  if (data == NULL || length == 0u ||
      length > ctx->total_size - ctx->write_offset - ctx->cache_len)
    return abort_session(ctx, ESY_ERR_INVALID_ARG);
  while (length > 0u) {
    const uint32_t room = ctx->cache_size - ctx->cache_len;
    const uint32_t take = length < room ? (uint32_t)length : room;
    memcpy(ctx->cache + ctx->cache_len, data, take);
    ctx->cache_len += take;
    data += take;
    length -= take;
    if (ctx->cache_len == ctx->cache_size) {
      const esy_err_t ret = write_block(ctx, ctx->cache, ctx->cache_len);
      if (ret != ESY_OK) return abort_session(ctx, ret);
      ctx->write_offset += ctx->cache_len;
      ctx->cache_len = 0u;
    }
  }
  return ESY_OK;
}

static esy_err_t provider_finish(void *context) {
  if (context == NULL) return ESY_ERR_INVALID_ARG;
  esy_ota_flash_ctx_t *ctx = ota_ctx(context);
  if (!ctx->initialized || !ctx->session_active || ctx->verified) return ESY_ERR_STATE;
  if (ctx->write_offset + ctx->cache_len != ctx->total_size)
    return abort_session(ctx, ESY_ERR_INVALID_ARG);
  if (ctx->cache_len > 0u) {
    const uint32_t flush_size = align_up_u32(ctx->cache_len, ctx->write_size);
    if (flush_size > ctx->cache_len)
      memset(ctx->cache + ctx->cache_len, 0xff, flush_size - ctx->cache_len);
    const esy_err_t ret = write_block(ctx, ctx->cache, flush_size);
    if (ret != ESY_OK) return abort_session(ctx, ret);
    ctx->write_offset += ctx->cache_len;
    ctx->cache_len = 0u;
  }
  uint8_t digest[ESY_IMAGE_SHA256_BYTES];
  const esy_err_t ret = calculate_written_sha256(ctx, ctx->base_addr, digest);
  if (ret != ESY_OK) return abort_session(ctx, ret);
  if (memcmp(digest, ctx->metadata.image_sha256, sizeof(digest)) != 0)
    return abort_session(ctx, ESY_ERR_HASH_FAIL);
  ctx->verified = true;
  return ESY_OK;
}

static esy_err_t provider_activate(void *context) {
  if (context == NULL) return ESY_ERR_INVALID_ARG;
  esy_ota_flash_ctx_t *ctx = ota_ctx(context);
  if (!ctx->initialized || !ctx->session_active || !ctx->verified) return ESY_ERR_STATE;
  esy_firmware_header_t header = {
      .version = ctx->metadata.version, .firmware_size = ctx->total_size,
      .state = ESY_FIRMWARE_STATE_TRY_BOOT};
  memcpy(header.image_sha256, ctx->metadata.image_sha256, sizeof(header.image_sha256));
  memcpy(header.signature, ctx->metadata.signature, sizeof(header.signature));
  ctx->activation_pending_reconcile = true;
  esy_err_t ret = esy_bootloader_write_meta_data(ctx->slot, &header);
  if (ret == ESY_OK) ret = esy_bootloader_activate_image(ctx->slot);
  ctx->session_active = false;
  ctx->verified = false;
  clear_cached_bytes(ctx);
  return ret;
}

static esy_err_t provider_abort(void *context) {
  if (context == NULL) return ESY_ERR_INVALID_ARG;
  esy_ota_flash_ctx_t *ctx = ota_ctx(context);
  if (!ctx->initialized) return ESY_ERR_STATE;
  ctx->session_active = false;
  ctx->verified = false;
  clear_cached_bytes(ctx);
  return ESY_OK;
}

esy_err_t esy_ota_flash_init(esy_ota_flash_t *flash,
                             const esy_ota_flash_config_t *config) {
  if (flash == NULL) return ESY_ERR_INVALID_ARG;
  esy_ota_flash_ctx_t *ctx = ota_ctx(flash);
  if (ctx->initialized) return ESY_ERR_STATE;
  if (config == NULL || config->static_resources == NULL ||
      config->static_resources->cache_buffer == NULL ||
      config->static_resources->cache_size == 0u) return ESY_ERR_INVALID_ARG;
  const esy_ota_flash_static_resources_t *resources = config->static_resources;
#if SIZE_MAX > UINT32_MAX
  if (resources->cache_size > UINT32_MAX) return ESY_ERR_INVALID_ARG;
#endif
  const uintptr_t cache = (uintptr_t)resources->cache_buffer;
  const uintptr_t handle = (uintptr_t)flash;
  if (resources->cache_size > UINTPTR_MAX - cache || sizeof(*flash) > UINTPTR_MAX - handle ||
      (cache < handle + sizeof(*flash) && handle < cache + resources->cache_size))
    return ESY_ERR_INVALID_ARG;
  ctx->cache = resources->cache_buffer;
  ctx->cache_size = (uint32_t)resources->cache_size;
  ctx->initialized = true;
  return ESY_OK;
}

esy_err_t esy_ota_flash_deinit(esy_ota_flash_t *flash) {
  if (flash == NULL) return ESY_ERR_INVALID_ARG;
  ota_reset_all(flash);
  return ESY_OK;
}

esy_err_t esy_ota_flash_get_provider(esy_ota_flash_t *flash,
                                     esy_ota_provider_t *provider) {
  if (provider == NULL) return ESY_ERR_INVALID_ARG;
  *provider = (esy_ota_provider_t){0};
  if (flash == NULL) return ESY_ERR_INVALID_ARG;
  if (!ota_ctx(flash)->initialized) return ESY_ERR_STATE;
  *provider = (esy_ota_provider_t){
      .begin = provider_begin, .write = provider_write, .finish = provider_finish,
      .activate = provider_activate, .abort = provider_abort, .context = flash};
  return ESY_OK;
}
