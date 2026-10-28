/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_SETTINGS_H
#define ESY_REMOTE_MANAGER_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/settings/esy_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_remote_manager_settings.h
 * @brief Optional adaptation of one initialized Settings store to config ops.
 *
 * All adapter, manager and Settings access shares one serialized non-ISR owner.
 * GET/SET/REMOVE use a caller-provided key conversion buffer. SAVE calls the
 * synchronous Settings commit through its bound file; latency and persistence
 * follow that file provider. APPLY
 * is unsupported; applying configuration and key/schema/permission validation
 * remain with the owner. Use manager authorization or an owner wrapper to
 * restrict exposed keys. This adapter does not make every product setting part
 * of this store and never initializes, reloads, clears or deinitializes it.
 */

/** @brief Smallest key scratch capacity: one key byte and its NUL terminator. */
#define ESY_REMOTE_MANAGER_SETTINGS_KEY_BUFFER_MIN_BYTES 2u

/** @brief Caller-owned scratch borrowed until adapter deinit. */
typedef struct {
    char *key_buffer; /**< Writable conversion buffer, disjoint from all live objects. */
    size_t key_buffer_size; /**< At least KEY_BUFFER_MIN_BYTES, including the NUL. */
} esy_remote_manager_settings_static_resources_t;

/** @brief Descriptor copied by init; backing resources remain borrowed. */
typedef struct {
    esy_settings_t *settings; /**< Already initialized Settings store; address stays fixed. */
    const esy_remote_manager_settings_static_resources_t *static_resources;
        /**< Required key conversion storage. */
} esy_remote_manager_settings_config_t;

/**
 * @brief Caller-owned adapter requiring {0} before first init; fields are private.
 * Do not copy/move/reset while live. Settings and scratch remain valid through
 * successful deinit. Detach every manager/ops borrower before deinit; this
 * external lifecycle precondition cannot be inferred from copied ops tables.
 */
typedef struct {
    esy_settings_t *settings; /**< Private borrowed store. */
    char *key_buffer; /**< Private borrowed scratch. */
    size_t key_buffer_size; /**< Private scratch capacity. */
    bool initialized; /**< Private lifecycle state. */
} esy_remote_manager_settings_t;

/**
 * @brief Borrow Settings and scratch without modifying persistent state.
 * @return OK, INVALID_ARG for missing/invalid resources, STATE for repeated init
 * or an uninitialized Settings store, or its query error. Failed init preserves
 * the adapter and leaves scratch unchanged. Descriptors may expire after return.
 */
esy_err_t esy_remote_manager_settings_init(
    esy_remote_manager_settings_t *adapter,
    const esy_remote_manager_settings_config_t *config);

/**
 * @brief Release adapter borrows without applying or saving Settings.
 * @return INVALID_ARG for NULL, otherwise OK, including repeated zero deinit.
 */
esy_err_t esy_remote_manager_settings_deinit(
    esy_remote_manager_settings_t *adapter);

/**
 * @brief Copy config operations borrowing this initialized adapter.
 * @return OK, INVALID_ARG for NULL, STATE when not initialized. Required ops is
 * zeroed on failure. It must not overlap adapter/backing storage. Keys need no
 * terminating NUL; keys not fitting scratch return INVALID_ARG before changing
 * Settings. Set the manager key budget to at most key_buffer_size minus one.
 * Keep the adapter alive until every returned ops borrower has detached.
 */
esy_err_t esy_remote_manager_settings_get_ops(
    esy_remote_manager_settings_t *adapter,
    esy_remote_manager_config_ops_t *ops);

#ifdef __cplusplus
}
#endif
#endif /* ESY_REMOTE_MANAGER_SETTINGS_H */
