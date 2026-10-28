/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESY_FILE_FLASH_H
#define ESY_FILE_FLASH_H

#include "esy_middleware/file/esy_file.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Private handle capacity, unrelated to persistent file format. */
#define ESY_FILE_FLASH_STORAGE_BYTES 128u
/** @brief Caller-owned opaque provider requiring {0}; do not copy while live. */
typedef struct {
    union {
        uint8_t bytes[ESY_FILE_FLASH_STORAGE_BYTES]; /**< Private in-place state. */
        max_align_t alignment; /**< Natural private-state alignment. */
    } storage; /**< Opaque storage. */
} esy_file_flash_t;
/** @brief Exclusive SRAM scratch; no whole-file RAM copy is required. */
typedef struct {
    uint8_t *program_buffer; /**< HAL-programmable RAM, disjoint from handles and caller data. */
    size_t program_capacity; /**< At least one HAL program unit. */
} esy_file_flash_static_resources_t;
/** @brief One exclusive whole-sector region, disjoint from all other storage users. */
typedef struct {
    uint32_t address; /**< Public HAL Flash coordinate, not a runtime file address. */
    uint32_t size; /**< Complete erase sectors including the final commit footer. */
    const esy_file_flash_static_resources_t *static_resources; /**< Required SRAM scratch. */
} esy_file_flash_config_t;

/**
 * @brief Bind a single-file Flash region after geometry/ownership validation.
 * Provides READ, CREATE and REMOVE; atomic replacement is unsupported. The
 * region remains untouched until an explicit mutation. Creation writes and
 * verifies complete content and metadata before a separate aligned commit
 * marker is programmed last; existing valid files are
 * never replaced. Removal erases and verifies the complete reserved region.
 * The consumer owns Flash initialization, serialization, safe execution and
 * watchdog/OTA arbitration. Calls are synchronous, normal startup/task only.
 * Scratch and region remain exclusively borrowed until all file users detach
 * and provider deinit succeeds. Data input must not alias this region or scratch.
 * @return OK; INVALID_ARG, NO_MEM, STATE, NOT_SUPPORTED or HAL query error.
 * Failure preserves the handle; no Flash erase/program is performed by init.
 */
esy_err_t esy_file_flash_init(esy_file_flash_t *flash, const esy_file_flash_config_t *config);
/** @brief Copy read/create/remove operations borrowing the initialized provider.
 * @return OK, INVALID_ARG for NULL or STATE before init. */
esy_err_t esy_file_flash_get_ops(esy_file_flash_t *flash, esy_file_ops_t *ops);
/** @brief Release bindings without touching Flash; repeated zero deinit succeeds.
 * @return OK, INVALID_ARG or STATE for an invalid/copied handle. */
esy_err_t esy_file_flash_deinit(esy_file_flash_t *flash);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FILE_FLASH_H */
