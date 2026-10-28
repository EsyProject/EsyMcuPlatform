/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esy_middleware/file/esy_file_flash.h"
#include "esy_hal/esy_hal_flash.h"
#include <string.h>

#define FILE_FLASH_HANDLE_MAGIC UINT32_C(0x46494c48)
#define FILE_FLASH_COMMIT_MAGIC UINT32_C(0x314c4645)
#define FILE_FLASH_METADATA_MAGIC UINT32_C(0x31444d46)
#define FILE_FLASH_COMMIT_BYTES 4u
#define FILE_FLASH_FOOTER_BYTES 32u
#define FILE_FLASH_FOOTER_CRC_OFFSET 16u
#define FILE_FLASH_CRC_POLYNOMIAL UINT32_C(0xedb88320)
enum { FILE_FLASH_VERSION = 1 };
typedef struct {
    uintptr_t owner;
    uint8_t *program;
    size_t program_capacity;
    uint32_t address, size, unit, metadata_span, commit_span, capacity, magic;
} file_flash_ctx_t;
_Static_assert(sizeof(file_flash_ctx_t) <= ESY_FILE_FLASH_STORAGE_BYTES,
               "File Flash handle is too small");
static bool overlap(const void *a, size_t an, const void *b, size_t bn) {
    uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
    return an > UINTPTR_MAX - aa || bn > UINTPTR_MAX - bb ||
        (an != 0u && bn != 0u && aa < bb + bn && bb < aa + an);
}
static bool all_bytes(const void *data, size_t length, uint8_t value) {
    const uint8_t *bytes = data;
    for (size_t i = 0u; i < length; ++i) if (bytes[i] != value) return false;
    return true;
}
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
        ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}
static void write_u32(uint8_t *p, uint32_t value) {
    for (size_t i = 0u; i < sizeof(value); ++i) p[i] = (uint8_t)(value >> (i * 8u));
}
static uint32_t crc_update(uint32_t crc, const void *data, size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0u; i < length; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (FILE_FLASH_CRC_POLYNOMIAL & (0u - (crc & 1u)));
    }
    return crc;
}
static esy_err_t context_of(esy_file_flash_t *flash, file_flash_ctx_t *ctx) {
    if (flash == NULL) return ESY_ERR_INVALID_ARG;
    memcpy(ctx, flash->storage.bytes, sizeof(*ctx));
    return ctx->magic == FILE_FLASH_HANDLE_MAGIC && ctx->owner == (uintptr_t)flash
        ? ESY_OK : ESY_ERR_STATE;
}
static bool aliases(const file_flash_ctx_t *ctx, const void *data, size_t length) {
    return overlap(data, length, (const void *)ctx->owner, sizeof(esy_file_flash_t)) ||
        overlap(data, length, ctx->program, ctx->program_capacity) ||
        overlap(data, length, (const void *)(uintptr_t)ctx->address, ctx->size);
}
static esy_err_t media_crc(const file_flash_ctx_t *ctx, uint32_t length, uint32_t *crc) {
    uint32_t offset = 0u, state = UINT32_MAX;
    while (offset < length) {
        const uint32_t remaining = length - offset;
        size_t count = remaining < ctx->unit ? remaining : ctx->unit;
        esy_err_t result = esy_hal_flash_read(ctx->address + offset, ctx->program, count);
        if (result != ESY_OK) return result;
        state = crc_update(state, ctx->program, count);
        offset += (uint32_t)count;
    }
    *crc = ~state;
    return ESY_OK;
}
static esy_err_t scan(const file_flash_ctx_t *ctx, esy_file_info_t *info) {
    uint8_t commit[FILE_FLASH_COMMIT_BYTES];
    esy_err_t result = esy_hal_flash_read(
        ctx->address + ctx->size - ctx->commit_span, commit, sizeof(commit));
    if (result != ESY_OK) return result;
    *info = (esy_file_info_t){.capacity = ctx->capacity};
    if (read_u32(commit) != FILE_FLASH_COMMIT_MAGIC) return ESY_OK;
    uint8_t footer[FILE_FLASH_FOOTER_BYTES];
    result = esy_hal_flash_read(ctx->address + ctx->capacity, footer, sizeof(footer));
    if (result != ESY_OK) return result;
    if (read_u32(footer) != FILE_FLASH_METADATA_MAGIC ||
        read_u32(footer + FILE_FLASH_FOOTER_CRC_OFFSET) !=
        ~crc_update(UINT32_MAX, footer, FILE_FLASH_FOOTER_CRC_OFFSET)) return ESY_ERR_CRC_FAIL;
    if (read_u32(footer + 4u) != FILE_FLASH_VERSION) return ESY_ERR_NOT_SUPPORTED;
    const uint32_t length = read_u32(footer + 8u);
    if (length > ctx->capacity) return ESY_ERR_CRC_FAIL;
    uint32_t crc;
    result = media_crc(ctx, length, &crc);
    if (result != ESY_OK) return result;
    if (crc != read_u32(footer + 12u)) return ESY_ERR_CRC_FAIL;
    info->exists = true;
    info->length = length;
    return ESY_OK;
}
static esy_err_t flash_info(void *context, esy_file_info_t *info) {
    file_flash_ctx_t ctx;
    esy_err_t result = context_of(context, &ctx);
    if (result != ESY_OK) return result;
    if (info == NULL || aliases(&ctx, info, sizeof(*info))) return ESY_ERR_INVALID_ARG;
    esy_file_info_t snapshot;
    result = scan(&ctx, &snapshot);
    if (result == ESY_OK) *info = snapshot;
    return result;
}
static esy_err_t flash_read(void *context, uint32_t offset, void *buffer, size_t length) {
    file_flash_ctx_t ctx;
    esy_err_t result = context_of(context, &ctx);
    if (result != ESY_OK) return result;
    if ((buffer == NULL && length != 0u) || aliases(&ctx, buffer, length) ||
        length > UINT32_MAX - offset) return ESY_ERR_INVALID_ARG;
    esy_file_info_t info;
    result = scan(&ctx, &info);
    if (result != ESY_OK) return result;
    if (!info.exists) return ESY_ERR_STATE;
    if (offset > info.length || length > info.length - offset) return ESY_ERR_INVALID_ARG;
    return length == 0u ? ESY_OK : esy_hal_flash_read(ctx.address + offset, buffer, length);
}
static esy_err_t program_bytes(const file_flash_ctx_t *ctx, uint32_t address,
                               const uint8_t *data, size_t length) {
    size_t offset = 0u;
    while (offset < length) {
        size_t count = length - offset;
        if (count > ctx->unit) count = ctx->unit;
        memset(ctx->program, UINT8_MAX, ctx->unit);
        memcpy(ctx->program, data + offset, count);
        esy_err_t result = esy_hal_flash_write(address, ctx->program, ctx->unit);
        if (result != ESY_OK) return result;
        offset += count;
        if (offset < length) address += ctx->unit;
    }
    return ESY_OK;
}
static esy_err_t flash_create(void *context, const void *data, size_t length) {
    file_flash_ctx_t ctx;
    esy_err_t result = context_of(context, &ctx);
    if (result != ESY_OK) return result;
    if ((data == NULL && length != 0u) || aliases(&ctx, data, length)) return ESY_ERR_INVALID_ARG;
    if (length > ctx.capacity) return ESY_ERR_NO_MEM;
    esy_file_info_t info;
    result = scan(&ctx, &info);
    if (result != ESY_OK) return result;
    if (info.exists) return ESY_ERR_BUSY;
    uint32_t crc = ~crc_update(UINT32_MAX, data, length);
    uint8_t footer[FILE_FLASH_FOOTER_BYTES] = {0};
    write_u32(footer, FILE_FLASH_METADATA_MAGIC);
    write_u32(footer + 4u, FILE_FLASH_VERSION);
    write_u32(footer + 8u, (uint32_t)length);
    write_u32(footer + 12u, crc);
    write_u32(footer + FILE_FLASH_FOOTER_CRC_OFFSET,
        ~crc_update(UINT32_MAX, footer, FILE_FLASH_FOOTER_CRC_OFFSET));
    result = esy_hal_flash_erase(ctx.address, ctx.size);
    if (result != ESY_OK) return result;
    result = program_bytes(&ctx, ctx.address, data, length);
    if (result != ESY_OK) return result;
    uint32_t actual_crc;
    result = media_crc(&ctx, (uint32_t)length, &actual_crc);
    if (result != ESY_OK) return result;
    if (actual_crc != crc) return ESY_ERR_CRC_FAIL;
    result = program_bytes(&ctx, ctx.address + ctx.capacity, footer, sizeof(footer));
    if (result != ESY_OK) return result;
    uint8_t actual_footer[FILE_FLASH_FOOTER_BYTES];
    result = esy_hal_flash_read(ctx.address + ctx.capacity, actual_footer, sizeof(actual_footer));
    if (result != ESY_OK) return result;
    if (memcmp(actual_footer, footer, sizeof(footer)) != 0) return ESY_ERR_CRC_FAIL;
    uint8_t commit[FILE_FLASH_COMMIT_BYTES];
    write_u32(commit, FILE_FLASH_COMMIT_MAGIC);
    result = program_bytes(&ctx, ctx.address + ctx.size - ctx.commit_span, commit, sizeof(commit));
    if (result != ESY_OK) return result;
    result = scan(&ctx, &info);
    return result == ESY_OK && (!info.exists || info.length != length) ? ESY_ERR_CRC_FAIL : result;
}
static esy_err_t flash_remove(void *context) {
    file_flash_ctx_t ctx;
    esy_err_t result = context_of(context, &ctx);
    if (result != ESY_OK) return result;
    esy_file_info_t info;
    result = scan(&ctx, &info);
    if (result != ESY_OK) return result;
    result = esy_hal_flash_erase(ctx.address, ctx.size);
    if (result != ESY_OK) return result;
    uint32_t offset = 0u;
    while (offset < ctx.size) {
        uint32_t count = ctx.size - offset;
        if (count > ctx.unit) count = ctx.unit;
        result = esy_hal_flash_read(ctx.address + offset, ctx.program, count);
        if (result != ESY_OK) return result;
        if (!all_bytes(ctx.program, count, UINT8_MAX)) return ESY_ERR_CRC_FAIL;
        offset += count;
    }
    return ESY_OK;
}
esy_err_t esy_file_flash_init(esy_file_flash_t *flash, const esy_file_flash_config_t *config) {
    if (flash == NULL) return ESY_ERR_INVALID_ARG;
    if (!all_bytes(flash->storage.bytes, sizeof(flash->storage.bytes), 0u)) return ESY_ERR_STATE;
    if (config == NULL || config->static_resources == NULL ||
        config->static_resources->program_buffer == NULL || config->size == 0u)
        return ESY_ERR_INVALID_ARG;
    esy_flash_geometry_t geometry;
    esy_err_t result = esy_hal_flash_get_geometry(&geometry);
    if (result != ESY_OK) return result;
    if (geometry.program_size == 0u || geometry.size_bytes == 0u ||
        (uint64_t)geometry.base_address + geometry.size_bytes > (uint64_t)UINT32_MAX + 1u)
        return ESY_ERR_NOT_SUPPORTED;
    const uint64_t end = (uint64_t)config->address + config->size;
    if (config->address < geometry.base_address ||
        end > (uint64_t)geometry.base_address + geometry.size_bytes ||
        config->address % geometry.program_size != 0u || config->size % geometry.program_size != 0u)
        return ESY_ERR_INVALID_ARG;
    const uint64_t span = ((uint64_t)FILE_FLASH_FOOTER_BYTES + geometry.program_size - 1u) /
        geometry.program_size * geometry.program_size;
    const uint64_t commit_span = ((uint64_t)FILE_FLASH_COMMIT_BYTES + geometry.program_size - 1u) /
        geometry.program_size * geometry.program_size;
    if (span + commit_span > config->size ||
        config->static_resources->program_capacity < geometry.program_size)
        return ESY_ERR_NO_MEM;
    file_flash_ctx_t ctx = {.owner = (uintptr_t)flash,
        .program = config->static_resources->program_buffer,
        .program_capacity = config->static_resources->program_capacity,
        .address = config->address, .size = config->size,
        .unit = geometry.program_size, .metadata_span = (uint32_t)span,
        .commit_span = (uint32_t)commit_span,
        .capacity = config->size - (uint32_t)(span + commit_span), .magic = FILE_FLASH_HANDLE_MAGIC};
    if (overlap(ctx.program, ctx.program_capacity, flash, sizeof(*flash)) ||
        overlap(ctx.program, ctx.program_capacity, (const void *)(uintptr_t)ctx.address, ctx.size))
        return ESY_ERR_INVALID_ARG;
    for (uint64_t cursor = config->address; cursor < end;) {
        esy_flash_sector_t sector;
        result = esy_hal_flash_get_sector((uint32_t)cursor, &sector);
        if (result != ESY_OK) return result;
        if (sector.start_address != cursor || sector.size_bytes == 0u ||
            sector.size_bytes > end - cursor) return ESY_ERR_INVALID_ARG;
        cursor += sector.size_bytes;
    }
    memcpy(flash->storage.bytes, &ctx, sizeof(ctx));
    return ESY_OK;
}
esy_err_t esy_file_flash_get_ops(esy_file_flash_t *flash, esy_file_ops_t *ops) {
    file_flash_ctx_t ctx;
    esy_err_t result = context_of(flash, &ctx);
    if (result != ESY_OK) return result;
    if (ops == NULL || aliases(&ctx, ops, sizeof(*ops))) return ESY_ERR_INVALID_ARG;
    *ops = (esy_file_ops_t){.info = flash_info, .read = flash_read,
        .create = flash_create, .remove = flash_remove, .context = flash};
    return ESY_OK;
}
esy_err_t esy_file_flash_deinit(esy_file_flash_t *flash) {
    if (flash == NULL) return ESY_ERR_INVALID_ARG;
    if (all_bytes(flash->storage.bytes, sizeof(flash->storage.bytes), 0u)) return ESY_OK;
    file_flash_ctx_t ctx;
    esy_err_t result = context_of(flash, &ctx);
    if (result == ESY_OK) memset(flash->storage.bytes, 0, sizeof(flash->storage.bytes));
    return result;
}
