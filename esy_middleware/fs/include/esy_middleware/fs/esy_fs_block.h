/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FS_BLOCK_H
#define ESY_FS_BLOCK_H

#include "esy_middleware/fs/esy_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Current block medium state; no implicit hardware initialization. */
typedef struct {
    bool ready; /**< True only while the initialized, same medium is accessible. */
    bool read_only; /**< True when writes must be rejected. */
} esy_fs_block_status_t;

/**
 * @brief Borrowed synchronous block device, typically an SD HAL adapter.
 *
 * A single non-ISR owner serializes all access with the filesystem and owns
 * hardware initialization, insertion/removal detection, DMA completion and
 * shutdown. Callbacks block until complete and return ESY_OK or a negative
 * error; no partial sector success is permitted. Buffers need no special DMA
 * alignment: a hardware adapter must supply any required bounce storage.
 * status must detect removed/uninitialized media even when filesystem data is
 * cached. Never transparently substitute a different card while mounted;
 * latch not-ready until the owner has closed all objects and unmounted.
 * sync commits all previous writes according to the actual medium guarantees.
 * This block address space is exclusive to its initialized backend. Another
 * filesystem mount or USB host must not access overlapping sectors concurrently.
 * Geometry is immutable from backend init through deinit. All callbacks and
 * context remain valid until backend deinit. This interface owns no hardware.
 */
typedef struct {
    void *context; /**< Borrowed device state. NULL is allowed by callback contract. */
    esy_err_t (*status)(void *context, esy_fs_block_status_t *status); /**< Query readiness/protection. */
    esy_err_t (*read)(void *context, uint64_t sector, uint32_t count, void *buffer); /**< Read whole sectors. */
    esy_err_t (*write)(void *context, uint64_t sector, uint32_t count, const void *buffer); /**< Write whole sectors. */
    esy_err_t (*sync)(void *context); /**< Wait for media write completion. */
    uint64_t sector_count; /**< Number of addressable sectors; nonzero. */
    uint32_t sector_size; /**< Sector bytes, supported by selected provider configuration. */
    uint32_t erase_block_sectors; /**< Erase/allocation alignment hint in sectors; power of two. */
} esy_fs_block_ops_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_FS_BLOCK_H */
