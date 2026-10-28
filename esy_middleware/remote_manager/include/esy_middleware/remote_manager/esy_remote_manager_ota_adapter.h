/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESY_REMOTE_MANAGER_OTA_ADAPTER_H
#define ESY_REMOTE_MANAGER_OTA_ADAPTER_H

#include "esy_middleware/remote_manager/esy_remote_manager_ota.h"
#include "esy_middleware/ota/esy_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Borrowed OTA core and owner-supplied current-image information. */
typedef struct {
    esy_ota_t *ota; /**< Already initialized core; outlives adapter and manager. */
    esy_err_t (*info)(void *context, esy_remote_manager_ota_info_t *info); /**< Required owner query. */
    void *info_context; /**< Borrowed owner context, may be NULL. */
    esy_remote_manager_image_ops_t images; /**< Optional image inventory/control provider; copied by value. */
} esy_remote_manager_ota_adapter_config_t;

/** @brief Caller-owned adapter requiring {0}; no provider/core lifecycle is owned. */
typedef struct {
    esy_remote_manager_ota_adapter_config_t config; /**< Private copied binding. */
    bool initialized; /**< Private lifecycle flag. */
} esy_remote_manager_ota_adapter_t;

/**
 * @brief Bind an initialized OTA core without starting or changing an upload.
 * All calls share one serialized non-ISR manager/core owner. The owner's info
 * callback returns limits consistent with the OTA core and transport budget;
 * max_chunk_bytes excludes WRITE's eight argument bytes and all envelope bytes.
 * Authentication, BEGIN admission/version/signature policy and post-ACTIVATE
 * response confirmation/reset are supplied by the owner, never this adapter.
 * Optional images callbacks are copied directly, retaining their independent
 * borrowed context. They may operate without an upload session. Their provider
 * owns health admission, image authentication, boot-state serialization and
 * durable confirmation/rollback semantics, including partial-failure recovery.
 * Neither callback registration nor detach changes image or provider lifecycle.
 * @return OK; INVALID_ARG for missing dependencies; STATE for repeated init or
 * detached core; another core status error. Failure leaves the adapter unchanged.
 */
esy_err_t esy_remote_manager_ota_adapter_init(
    esy_remote_manager_ota_adapter_t *adapter,
    const esy_remote_manager_ota_adapter_config_t *config);
/** @brief Copy normalized ops; adapter/core stay borrowed until manager deinit.
 * @return INVALID_ARG for NULL, STATE before init, otherwise OK. */
esy_err_t esy_remote_manager_ota_adapter_get_ops(
    esy_remote_manager_ota_adapter_t *adapter, esy_remote_manager_ota_ops_t *ops);
/** @brief Detach after manager delivery stops; never aborts or deinitializes OTA.
 * @return INVALID_ARG for NULL, otherwise OK, including repeated deinit. */
esy_err_t esy_remote_manager_ota_adapter_deinit(
    esy_remote_manager_ota_adapter_t *adapter);

#ifdef __cplusplus
}
#endif
#endif /* ESY_REMOTE_MANAGER_OTA_ADAPTER_H */
