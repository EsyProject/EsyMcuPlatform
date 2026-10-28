/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/fs/esy_fs_flash.h"
#include "esy_hal/esy_hal_flash.h"
#include <limits.h>
#include <string.h>

#define FS_FLASH_MIN_BLOCK_SIZE 128u

static bool overlap(const void *a, size_t an, const void *b, size_t bn) {
    if (an == 0u || bn == 0u) return false;
    const uintptr_t ap = (uintptr_t)a;
    const uintptr_t bp = (uintptr_t)b;
    if (ap > UINTPTR_MAX - (an - 1u) || bp > UINTPTR_MAX - (bn - 1u)) return true;
    return ap <= bp + bn - 1u && bp <= ap + an - 1u;
}

static bool valid(const esy_fs_flash_t *flash) {
    return flash != NULL && flash->self == flash;
}

static bool address_of(const struct lfs_config *config, lfs_block_t block,
                       lfs_off_t offset, lfs_size_t size, uint32_t *address) {
    if (config == NULL || !valid(config->context)) return false;
    const esy_fs_flash_t *flash = config->context;
    if (config->block_size != flash->block_size ||
        config->block_count != flash->size / flash->block_size ||
        block >= config->block_count || offset > flash->block_size ||
        size > flash->block_size - offset) return false;
    const uint64_t start = (uint64_t)flash->address + (uint64_t)block * flash->block_size + offset;
    if (start > UINT32_MAX) return false;
    *address = (uint32_t)start;
    return true;
}

static int flash_read(const struct lfs_config *config, lfs_block_t block,
                      lfs_off_t offset, void *buffer, lfs_size_t size) {
    uint32_t address;
    if (buffer == NULL || !address_of(config, block, offset, size, &address)) return LFS_ERR_INVAL;
    if (size == 0u) return 0;
    return esy_hal_flash_read(address, buffer, size) == ESY_OK ? 0 : LFS_ERR_IO;
}

static int flash_program(const struct lfs_config *config, lfs_block_t block,
                         lfs_off_t offset, const void *buffer, lfs_size_t size) {
    uint32_t address;
    if (buffer == NULL || !address_of(config, block, offset, size, &address)) return LFS_ERR_INVAL;
    const esy_fs_flash_t *flash = config->context;
    if (address % flash->program_size != 0u || size % flash->program_size != 0u) return LFS_ERR_INVAL;
    if (size == 0u) return 0;
    return esy_hal_flash_write(address, buffer, size) == ESY_OK ? 0 : LFS_ERR_IO;
}

static int flash_erase(const struct lfs_config *config, lfs_block_t block) {
    uint32_t address;
    if (!address_of(config, block, 0u, config != NULL ? config->block_size : 0u, &address))
        return LFS_ERR_INVAL;
    return esy_hal_flash_erase(address, config->block_size) == ESY_OK ? 0 : LFS_ERR_IO;
}

static int flash_sync(const struct lfs_config *config) {
    return config != NULL && valid(config->context) ? 0 : LFS_ERR_BADF;
}

esy_err_t esy_fs_flash_init(esy_fs_flash_t *flash, const esy_fs_flash_config_t *config) {
    if (flash == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
    if (flash->self != NULL) return ESY_ERR_STATE;
    const esy_fs_flash_config_t input = *config;
    if (input.block_size < FS_FLASH_MIN_BLOCK_SIZE || input.size == 0u ||
        input.size % input.block_size != 0u || input.size / input.block_size < 2u)
        return ESY_ERR_INVALID_ARG;
    esy_flash_geometry_t geometry;
    esy_err_t result = esy_hal_flash_get_geometry(&geometry);
    if (result != ESY_OK) return result;
    const uint64_t end = (uint64_t)input.address + input.size;
    const uint64_t device_end = (uint64_t)geometry.base_address + geometry.size_bytes;
    if (geometry.size_bytes == 0u || geometry.program_size == 0u ||
        device_end > (uint64_t)UINT32_MAX + 1u || end > device_end ||
        input.address < geometry.base_address || input.address % geometry.program_size != 0u ||
        input.block_size % geometry.program_size != 0u) return ESY_ERR_INVALID_ARG;
    for (uint64_t block = input.address; block < end; block += input.block_size) {
        const uint64_t block_end = block + input.block_size;
        for (uint64_t cursor = block; cursor < block_end;) {
            esy_flash_sector_t sector;
            result = esy_hal_flash_get_sector((uint32_t)cursor, &sector);
            if (result != ESY_OK) return result;
            if (sector.start_address != cursor || sector.size_bytes == 0u ||
                sector.size_bytes > block_end - cursor) return ESY_ERR_INVALID_ARG;
            cursor += sector.size_bytes;
        }
    }
    *flash = (esy_fs_flash_t){.self = flash, .address = input.address, .size = input.size,
        .block_size = input.block_size, .program_size = geometry.program_size};
    return ESY_OK;
}

esy_err_t esy_fs_flash_get_config(esy_fs_flash_t *flash, struct lfs_config *config) {
    if (!valid(flash)) return flash == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_STATE;
    if (config == NULL || overlap(config, sizeof(*config), flash, sizeof(*flash)))
        return ESY_ERR_INVALID_ARG;
    *config = (struct lfs_config){.context = flash, .read = flash_read, .prog = flash_program,
        .erase = flash_erase, .sync = flash_sync, .read_size = 1u,
        .prog_size = flash->program_size, .block_size = flash->block_size,
        .block_count = flash->size / flash->block_size};
    return ESY_OK;
}

esy_err_t esy_fs_flash_deinit(esy_fs_flash_t *flash) {
    if (flash == NULL) return ESY_ERR_INVALID_ARG;
    if (flash->self == NULL) return ESY_OK;
    if (!valid(flash)) return ESY_ERR_STATE;
    memset(flash, 0, sizeof(*flash));
    return ESY_OK;
}
