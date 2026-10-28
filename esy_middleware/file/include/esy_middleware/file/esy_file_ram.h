/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESY_FILE_RAM_H
#define ESY_FILE_RAM_H

#include "esy_middleware/file/esy_file.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief RAM content borrowed exclusively while the provider is initialized. */
typedef struct {
    uint8_t *data_buffer; /**< Writable content buffer, disjoint from handles and inputs. */
    size_t capacity; /**< Positive content bytes, at most UINT32_MAX. */
} esy_file_ram_static_resources_t;
/** @brief Copied initial RAM file state; no persistent metadata is implied. */
typedef struct {
    const esy_file_ram_static_resources_t *static_resources; /**< Required content storage. */
    bool exists; /**< Whether initial_length bytes already form a file. */
    size_t initial_length; /**< At most capacity; zero when absent. */
} esy_file_ram_config_t;
/** @brief Caller-owned {0} provider; fields private, no allocation or hidden storage. */
typedef struct {
    esy_file_ram_static_resources_t resources; /**< Private copied storage binding. */
    uint32_t length; /**< Private committed length. */
    bool exists; /**< Private existence flag. */
    bool initialized; /**< Private lifecycle flag. */
} esy_file_ram_t;

/** @brief Bind RAM without clearing its bytes; repeated init returns STATE.
 * @return OK, INVALID_ARG, NO_MEM or STATE; failure preserves the handle.
 * One owner serializes all access. Metadata lives only until deinit/reset;
 * the owner explicitly supplies initial state again on reinit. */
esy_err_t esy_file_ram_init(esy_file_ram_t *ram, const esy_file_ram_config_t *config);
/** @brief Copy all file operations borrowing RAM; missing/detached provider returns STATE.
 * @return INVALID_ARG for NULL, STATE before init, otherwise OK. */
esy_err_t esy_file_ram_get_ops(esy_file_ram_t *ram, esy_file_ops_t *ops);
/** @brief Detach after all file borrowers stop; never persists or clears content.
 * @return INVALID_ARG for NULL, otherwise OK including repeated zero deinit. */
esy_err_t esy_file_ram_deinit(esy_file_ram_t *ram);

#ifdef __cplusplus
}
#endif
#endif /* ESY_FILE_RAM_H */
