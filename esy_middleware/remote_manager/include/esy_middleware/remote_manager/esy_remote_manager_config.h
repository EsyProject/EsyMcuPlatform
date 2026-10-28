/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_CONFIG_H
#define ESY_REMOTE_MANAGER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_remote_manager_config.h
 * @brief Storage-independent configuration operations supplied by the owner.
 *
 * Keys are nonempty byte strings without embedded NUL, with explicit lengths;
 * no terminating NUL is required. Values are opaque bytes; their encoding,
 * schema, validation and ownership belong to the provider. Empty values are
 * distinct from absent keys. All callbacks run synchronously on the manager's
 * serialized non-ISR owner and must not reenter that manager. Their execution
 * time contributes to dispatch latency; no real-time or nonblocking guarantee
 * is implied. Callbacks retain no request/output pointers after returning.
 */

/** @brief Commands in ESY_REMOTE_MANAGER_GROUP_CONFIG. */
typedef enum {
    ESY_REMOTE_MANAGER_CONFIG_GET = 1, /**< Read the provider's current value. */
    ESY_REMOTE_MANAGER_CONFIG_SET = 2, /**< Accept a validated logical change. */
    ESY_REMOTE_MANAGER_CONFIG_REMOVE = 3, /**< Remove a logical value. */
    ESY_REMOTE_MANAGER_CONFIG_APPLY = 4, /**< Explicitly apply current changes. */
    ESY_REMOTE_MANAGER_CONFIG_SAVE = 5, /**< Explicitly save current changes. */
} esy_remote_manager_config_command_t;

/**
 * @brief Optional configuration operations, copied by manager init.
 *
 * NULL callbacks advertise unsupported operations. The borrowed context and
 * underlying resources remain alive until successful manager deinit; the
 * manager never initializes, stops or frees them. The provider defines whether
 * SET/REMOVE take effect immediately or stage changes. Success does not imply
 * persistence. APPLY success means the provider's documented application step
 * completed; SAVE success means its documented persistence step completed.
 * Neither operation implies a multi-key transaction. Failed mutations may have
 * taken effect: callers must reconcile state instead of blindly retrying.
 */
typedef struct {
    /**
     * @brief Read a value into caller storage; NULL output is valid at capacity 0.
     * Set found and length on OK/NO_MEM. Missing: OK, false, 0. Empty: OK, true, 0.
     * For NO_MEM set true and the required length, strictly above capacity.
     * Length must not exceed the configured maximum value length. On OK copy
     * exactly length bytes; on error output contents are unspecified.
     */
    esy_err_t (*get)(void *context, const char *key, size_t key_length,
                     uint8_t *output, size_t capacity, size_t *length,
                     bool *found);
    /** @brief Validate and accept one value; NULL value is valid at length 0. */
    esy_err_t (*set)(void *context, const char *key, size_t key_length,
                     const uint8_t *value, size_t length);
    /** @brief Remove one key according to provider-defined absence semantics. */
    esy_err_t (*remove)(void *context, const char *key, size_t key_length);
    /** @brief Apply logical changes; optional and separate from persistence. */
    esy_err_t (*apply)(void *context);
    /** @brief Persist logical changes under the provider's documented contract. */
    esy_err_t (*save)(void *context);
    void *context; /**< Borrowed provider context; NULL is permitted. */
} esy_remote_manager_config_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_REMOTE_MANAGER_CONFIG_H */
