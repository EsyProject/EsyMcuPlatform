/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_COREDUMP_STORE_H
#define ESY_REMOTE_MANAGER_COREDUMP_STORE_H

#include <stdbool.h>
#include "esy_middleware/remote_manager/esy_remote_manager_coredump.h"
#include "esy_middleware/coredump/esy_coredump.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_remote_manager_coredump_store.h
 * @brief Optional range-read adapter for an initialized RAM Coredump store.
 *
 * One non-ISR owner serializes adapter/store access without concurrent capture.
 * Store initialization is a caller precondition: get_info uses STATE both for
 * an empty record and an invalid handle, so this adapter cannot distinguish
 * them through the public API. INFO maps STATE to an unavailable record;
 * CRC_FAIL/NOT_SUPPORTED and other errors propagate. Collection never clears
 * records. No persistence, capture or store lifecycle is owned here.
 */

/** @brief Config copied by init; resources remain borrowed. */
typedef struct {
    esy_coredump_t *store; /**< Already initialized store, kept alive until deinit. */
    esy_err_t (*clear)(void *context, const esy_coredump_record_id_t *record_id); /**< Discard the exact authoritative record. */
    void *clear_context; /**< Borrowed clear context; NULL is allowed. */
} esy_remote_manager_coredump_store_config_t;

/**
 * @brief Caller-owned adapter requiring {0}; fields are private by contract.
 * Do not copy/move/reset while live. Detach all copied ops/manager borrowers
 * before deinit. The optional clear callback must discard every authoritative
 * source in its required order, including persistent Flash when present.
 * The adapter checks RAM identity before invoking this callback; the callback
 * must also compare the same ID with any persistent authority before erasing.
 * No default RAM-only clear is installed. Callbacks may block according to
 * their documented provider contract and must not reenter the manager.
 */
typedef struct {
    esy_remote_manager_coredump_store_config_t config; /**< Private copied config. */
    bool initialized; /**< Private lifecycle state. */
} esy_remote_manager_coredump_store_t;

/**
 * @brief Borrow an already initialized store without changing retained bytes.
 * @return OK, INVALID_ARG for NULL inputs/store, STATE for repeated init.
 * Failed init preserves the adapter. The descriptor may expire after return.
 */
esy_err_t esy_remote_manager_coredump_store_init(
    esy_remote_manager_coredump_store_t *adapter,
    const esy_remote_manager_coredump_store_config_t *config);

/**
 * @brief Detach resources without clearing or deinitializing the store.
 * @return INVALID_ARG for NULL, otherwise OK, including repeated zero deinit.
 */
esy_err_t esy_remote_manager_coredump_store_deinit(
    esy_remote_manager_coredump_store_t *adapter);

/**
 * @brief Copy INFO/READ ops and optionally the explicitly provided CLEAR op.
 * @return OK, INVALID_ARG for NULL, STATE when not initialized. Required output
 * ops is zeroed on failure and must be disjoint from adapter/store storage.
 * Returned operations borrow this adapter until every user has detached.
 */
esy_err_t esy_remote_manager_coredump_store_get_ops(
    esy_remote_manager_coredump_store_t *adapter,
    esy_remote_manager_coredump_ops_t *ops);

#ifdef __cplusplus
}
#endif
#endif /* ESY_REMOTE_MANAGER_COREDUMP_STORE_H */
