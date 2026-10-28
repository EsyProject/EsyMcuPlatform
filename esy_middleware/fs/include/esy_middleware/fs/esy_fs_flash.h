/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FS_FLASH_H
#define ESY_FS_FLASH_H

#include "esy_common/defs/base/esy_common_defs.h"
#include "lfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Explicit region using public Flash HAL address coordinates. */
typedef struct {
    uint32_t address; /**< First byte, aligned to a complete physical sector. */
    uint32_t size; /**< Nonzero volume bytes, exactly divisible by block_size. */
    uint32_t block_size; /**< Logical erase block containing complete physical sectors. */
} esy_fs_flash_config_t;

/**
 * @brief Borrowed Flash medium, initially {0}; never copy/move while initialized.
 * Fields are private. The owner initializes the public Flash HAL and coordinates
 * all filesystem, OTA and direct controller accesses. This adapter neither
 * initializes/deinitializes HAL nor reserves a partition. Buffers passed through
 * littlefs must meet the active HAL port's source-memory requirements.
 */
typedef struct esy_fs_flash {
    const struct esy_fs_flash *self; /**< Private identity. */
    uint32_t address; /**< Private region base. */
    uint32_t size; /**< Private region size. */
    uint32_t block_size; /**< Private logical erase size. */
    uint32_t program_size; /**< Private queried physical write granularity. */
} esy_fs_flash_t;

/**
 * @brief Validate every logical block against physical sector geometry.
 * No mutation occurs. Returns INVALID_ARG for alignment/overflow/partial sectors,
 * STATE for a repeated init/copy, or the public HAL query error. Failure preserves
 * zero state. Geometry must remain stable until deinit; synchronous non-ISR only.
 */
esy_err_t esy_fs_flash_init(esy_fs_flash_t *flash, const esy_fs_flash_config_t *config);
/**
 * @brief Return native littlefs device callbacks and geometry; no media access.
 * Output unchanged on failure. Result has read_size=1 and queried prog_size;
 * caller sets cache_size, lookahead_size and block_cycles before backend init.
 * Callbacks borrow flash through backend deinit. HAL calls complete synchronously,
 * so sync is an ordering no-op; durability relies on the active Flash HAL port.
 */
esy_err_t esy_fs_flash_get_config(esy_fs_flash_t *flash, struct lfs_config *config);
/**
 * @brief Clear a medium after all backend borrowers are deinitialized.
 * Zero deinit succeeds; copied state returns STATE. Borrower lifetime is an owner
 * precondition; no global HAL resource or persistent data is changed.
 */
esy_err_t esy_fs_flash_deinit(esy_fs_flash_t *flash);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FS_FLASH_H */
