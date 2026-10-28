/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESY_FILE_DUAL_FLASH_STORE_H
#define ESY_FILE_DUAL_FLASH_STORE_H

#include "esy_middleware/file/esy_file.h"
#include "esy_middleware/dual_flash_store/esy_dual_flash_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Copied binding of one already initialized transactional dual Flash store. */
typedef struct {
    esy_dual_flash_store_t *store; /**< Exclusively borrowed store and its workspace. */
} esy_file_dual_flash_store_config_t;
/** @brief Caller-owned {0} adapter; fields private, no provider lifecycle ownership. */
typedef struct {
    esy_dual_flash_store_t *store; /**< Private borrowed binding. */
    bool initialized; /**< Private lifecycle flag. */
} esy_file_dual_flash_store_t;

/**
 * @brief Expose the latest record as one file without changing its format/data.
 * All operations use the dual Flash store's serialized normal execution owner.
 * Provides READ, CREATE, ATOMIC_REPLACE and REMOVE. Atomic replacement uses the
 * existing two-region transaction; logical deletion commits a tombstone and
 * does not promise secure erasure. Dual Flash store retention/fallback and partial
 * commit errors remain authoritative. Keep store/workspace alive and do not
 * access them through another owner while any copied operations remain bound.
 * @return OK, INVALID_ARG, STATE for repeated init or dual Flash store query error.
 * Failure preserves the adapter. No store init/deinit/clear is owned here.
 */
esy_err_t esy_file_dual_flash_store_init(esy_file_dual_flash_store_t *adapter,
                                    const esy_file_dual_flash_store_config_t *config);
/** @brief Copy operations borrowing this adapter; errors leave output unchanged.
 * @return OK, INVALID_ARG for NULL or STATE before init. */
esy_err_t esy_file_dual_flash_store_get_ops(esy_file_dual_flash_store_t *adapter, esy_file_ops_t *ops);
/** @brief Detach after file users stop; never deinitializes/clears the store.
 * @return INVALID_ARG for NULL, otherwise OK including repeated zero deinit. */
esy_err_t esy_file_dual_flash_store_deinit(esy_file_dual_flash_store_t *adapter);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FILE_DUAL_FLASH_STORE_H */
