/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_H
#define ESY_REMOTE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/remote_manager/esy_remote_manager_config.h"
#include "esy_middleware/remote_manager/esy_remote_manager_ota.h"
#include "esy_middleware/remote_manager/esy_remote_manager_coredump.h"
#include "esy_middleware/remote_manager/esy_remote_manager_stats.h"
#include "esy_middleware/remote_manager/esy_remote_manager_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_remote_manager.h
 * @brief Caller-driven management operations independent of wire/storage formats.
 *
 * One task or main-loop owner externally serializes every API and all provider
 * access. Never call from ISR/fault context. No task, lock, timeout, dynamic
 * allocation, transport or storage lifecycle is created. Each dispatch performs
 * at most one authorization and one operation callback, synchronously. Callback
 * cost belongs to the caller's execution budget. A callback may not reenter
 * the manager; the busy guard rejects dispatch/init/deinit during a dispatch.
 * Request, response and output storage must be mutually disjoint and must not
 * overlap the handle or provider-owned state. These are caller preconditions.
 */

/** @brief Implemented management command groups; unrelated to wire domains. */
typedef enum {
    ESY_REMOTE_MANAGER_GROUP_SYSTEM = 0, /**< Manager capability discovery. */
    ESY_REMOTE_MANAGER_GROUP_CONFIG = 1, /**< Provider-owned configuration. */
    ESY_REMOTE_MANAGER_GROUP_OTA = 2, /**< Synchronous typed image transactions. */
    ESY_REMOTE_MANAGER_GROUP_COREDUMP = 4, /**< Fault-record collection. */
    ESY_REMOTE_MANAGER_GROUP_STATS = 5, /**< Read-only runtime measurements. */
    ESY_REMOTE_MANAGER_GROUP_DIAGNOSTICS = 6, /**< Structured health and explicit self-tests. */
} esy_remote_manager_group_t;

/** @brief Commands in ESY_REMOTE_MANAGER_GROUP_SYSTEM. */
typedef enum {
    ESY_REMOTE_MANAGER_SYSTEM_DISCOVER = 1, /**< Query available operations. */
} esy_remote_manager_system_command_t;

/** @brief Capabilities derived from implemented operations, after authorization. */
typedef enum {
    ESY_REMOTE_MANAGER_CAP_DISCOVER = 1u << 0, /**< Capability query. */
    ESY_REMOTE_MANAGER_CAP_CONFIG_GET = 1u << 1, /**< Configuration read. */
    ESY_REMOTE_MANAGER_CAP_CONFIG_SET = 1u << 2, /**< Configuration modification. */
    ESY_REMOTE_MANAGER_CAP_CONFIG_REMOVE = 1u << 3, /**< Configuration removal. */
    ESY_REMOTE_MANAGER_CAP_CONFIG_APPLY = 1u << 4, /**< Explicit runtime apply. */
    ESY_REMOTE_MANAGER_CAP_CONFIG_SAVE = 1u << 5, /**< Explicit persistence. */
    ESY_REMOTE_MANAGER_CAP_OTA_INFO = 1u << 6, /**< Current image query. */
    ESY_REMOTE_MANAGER_CAP_OTA_BEGIN = 1u << 7, /**< Begin an image transaction. */
    ESY_REMOTE_MANAGER_CAP_OTA_STATUS = 1u << 8, /**< Upgrade status query. */
    ESY_REMOTE_MANAGER_CAP_COREDUMP_INFO = 1u << 10, /**< Fault record query. */
    ESY_REMOTE_MANAGER_CAP_COREDUMP_READ = 1u << 11, /**< Fault record range read. */
    ESY_REMOTE_MANAGER_CAP_COREDUMP_CLEAR = 1u << 12, /**< Authorized record discard. */
    ESY_REMOTE_MANAGER_CAP_OTA_WRITE = 1u << 13, /**< Write contiguous bytes. */
    ESY_REMOTE_MANAGER_CAP_OTA_FINISH = 1u << 14, /**< Verify the complete image. */
    ESY_REMOTE_MANAGER_CAP_OTA_ABORT = 1u << 15, /**< Abort image preparation. */
    ESY_REMOTE_MANAGER_CAP_OTA_ACTIVATE = 1u << 16, /**< Activate a ready image. */
    ESY_REMOTE_MANAGER_CAP_OTA_SLOT = 1u << 17, /**< Enumerate logical image slots. */
    ESY_REMOTE_MANAGER_CAP_OTA_CONFIRM = 1u << 18, /**< Confirm an identified running image. */
    ESY_REMOTE_MANAGER_CAP_OTA_ROLLBACK = 1u << 19, /**< Persist an identified fallback target. */
    ESY_REMOTE_MANAGER_CAP_STATS_ENUMERATE = 1u << 20, /**< Discover one metric by index. */
    ESY_REMOTE_MANAGER_CAP_STATS_READ = 1u << 21, /**< Read one metric by exact name. */
    ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_ENUMERATE = 1u << 22, /**< Discover diagnostic metadata. */
    ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_READ = 1u << 23, /**< Query current health. */
    ESY_REMOTE_MANAGER_CAP_DIAGNOSTICS_RUN = 1u << 24, /**< Explicit self-test routing. */
} esy_remote_manager_capability_t;

/** @brief Normalized borrowed request; valid until dispatch returns. */
typedef struct {
    uint32_t request_id; /**< Correlation only; grants no authentication/deduplication. */
    uint16_t group; /**< Management group; unknown values return NOT_SUPPORTED. */
    uint16_t command; /**< Group-local command identifier. */
    const char *key; /**< Bounded key span; NULL allowed only at length 0. */
    size_t key_length; /**< Key bytes excluding any terminator. */
    const uint8_t *value; /**< Opaque value span; NULL allowed only at length 0. */
    size_t value_length; /**< Value byte count. */
    esy_image_manifest_t manifest; /**< OTA BEGIN manifest; zero otherwise. */
    uint32_t session_id; /**< OTA transaction; STATUS zero queries current. */
    uint32_t offset; /**< OTA WRITE or COREDUMP READ byte offset. */
    uint32_t length; /**< COREDUMP READ requested bytes; zero for other commands. */
    uint32_t slot_index; /**< OTA SLOT enumeration position; zero otherwise. */
    esy_remote_manager_image_id_t image; /**< OTA CONFIRM/ROLLBACK identity; zero otherwise. */
    uint32_t stat_index; /**< STATS ENUMERATE position; zero for all other commands. */
    esy_coredump_record_id_t record_id; /**< COREDUMP READ/CLEAR generation; zero for other commands. */
    uint32_t diagnostic_index; /**< DIAGNOSTICS ENUMERATE position; zero otherwise. */
} esy_remote_manager_request_t;

/** @brief Normalized operation result, initialized by dispatch on every call. */
typedef struct {
    esy_err_t status; /**< Same result as dispatch; no transport-completion meaning. */
    uint32_t capabilities; /**< Nonzero only for successful DISCOVER. */
    size_t value_length; /**< Output data bytes, or required capacity on NO_MEM. */
    bool found; /**< GET distinguishes absent keys from empty values. */
    esy_remote_manager_ota_info_t ota_info; /**< Successful OTA INFO snapshot. */
    esy_image_status_t ota_status; /**< Successful OTA transaction snapshot. */
    esy_remote_manager_image_slot_t image_slot; /**< Successful SLOT/CONFIRM/ROLLBACK snapshot. */
    esy_remote_manager_coredump_info_t coredump_info; /**< Successful dump INFO snapshot. */
    esy_remote_manager_stats_snapshot_t stats; /**< Successful statistics measurement. */
    esy_diagnostics_snapshot_t diagnostic; /**< Successful diagnostic metadata/assessment. */
} esy_remote_manager_response_t;

/**
 * @brief Required authorization hook for every structurally valid request.
 * @return OK permits execution; a negative error denies without provider calls.
 * The owner supplies authenticated session/permission context separately from
 * the untrusted request. No credentials or identity are inferred by this module.
 * The hook and its context remain valid until successful deinit.
 */
typedef esy_err_t (*esy_remote_manager_authorize_t)(
    void *context, const esy_remote_manager_request_t *request);

/** @brief Configuration copied by init; no descriptor lifetime requirement. */
typedef struct {
    size_t max_key_length; /**< Positive key byte budget. */
    size_t max_request_value_length; /**< Maximum normalized request argument bytes. */
    size_t max_response_value_length; /**< Maximum opaque response value-buffer bytes. */
    esy_remote_manager_authorize_t authorize; /**< Required admission/permission hook. */
    void *authorize_context; /**< Borrowed context; NULL is permitted. */
    esy_remote_manager_config_ops_t config_ops; /**< Optional provider operations. */
    esy_remote_manager_ota_ops_t ota_ops; /**< Optional upgrade owner. */
    esy_remote_manager_coredump_ops_t coredump_ops; /**< Optional fault-record owner. */
    esy_remote_manager_stats_ops_t stats_ops; /**< Optional read-only measurement owner. */
    esy_remote_manager_diagnostics_ops_t diagnostics_ops; /**< Optional health/test owner. */
} esy_remote_manager_config_t;

/**
 * @brief Caller-owned handle, requiring {0} before first init.
 * Fields are private by contract. Do not copy/move/reset a live handle. Failed
 * init preserves it; successful deinit returns it to zero. No hidden resources
 * survive deinit and no provider lifecycle API is called.
 */
typedef struct {
    esy_remote_manager_config_t config; /**< Private copied configuration. */
    bool initialized; /**< Private lifecycle flag. */
    bool busy; /**< Private synchronous callback guard, not a concurrency lock. */
} esy_remote_manager_t;

/**
 * @brief Initialize zero or successfully deinitialized caller storage.
 * @return OK, INVALID_ARG for NULL/configuration errors, STATE for repeated
 * init, or BUSY if called from an active callback. Acquires no external resources.
 */
esy_err_t esy_remote_manager_init(esy_remote_manager_t *manager,
                                  const esy_remote_manager_config_t *config);

/**
 * @brief Detach callbacks and reset the handle; no implicit apply/save occurs.
 * @return INVALID_ARG for NULL, BUSY during dispatch, otherwise OK (also for
 * zero/deinitialized storage). Stop external delivery before releasing resources.
 */
esy_err_t esy_remote_manager_deinit(esy_remote_manager_t *manager);

/**
 * @brief Execute one normalized request on the serialized owner.
 * @param manager Initialized manager.
 * @param request Borrowed request; key/value spans are bounded by config.
 * @param output Caller-owned data destination; NULL valid only at capacity 0.
 * @param capacity Output capacity; provider sees at most max_response_value_length.
 * @param response Required result; always reset before validation, if non-NULL.
 * @return OK or a negative validation/authorization/operation error, also stored
 * in response.status. Unsupported group/command/provider operation returns
 * NOT_SUPPORTED. Structural errors return INVALID_ARG before authorization.
 * Provider contract violations return STATE. Output is authoritative only on
 * OK; on NO_MEM required value_length is returned without automatic retries.
 * Key/value arguments not used by a known command must have zero lengths.
 * offset is used by OTA WRITE and COREDUMP READ; length only by COREDUMP READ.
 * Other unused normalized fields must be zero. OTA request budgets count
 * manifest content (104), WRITE arguments (8 + data), or session identifier (4).
 * SLOT uses slot_index (4 argument bytes); CONFIRM/ROLLBACK use image identity
 * (40 argument bytes), with no upload session ID. Other unused fields are zero.
 * STATS ENUMERATE uses stat_index (4 argument bytes) and no key/value; STATS
 * READ uses a canonical metric name as key with no value. Name length is also
 * limited by max_key_length. The manager does not collect, reset or persist counters.
 * DIAGNOSTICS ENUMERATE uses diagnostic_index (4 argument bytes), without key/value.
 * READ/RUN use a canonical name key and no value; only RUN starts a self-test.
 * COREDUMP READ/CLEAR require the complete nonzero record_id returned by INFO;
 * their argument budgets are respectively 24 and 16 bytes. INFO and every other
 * command require zero record_id. The provider checks identity atomically with
 * its read/discard on the serialized owner; CRC and request ID cannot substitute.
 * Typed snapshots do not use output capacity or the opaque response-value
 * budget. Each codec separately reserves the complete serialized success
 * response before dispatch; their encoding sizes are not core constraints.
 * No response, pointer or request ID is retained after this synchronous call.
 */
esy_err_t esy_remote_manager_dispatch(
    esy_remote_manager_t *manager, const esy_remote_manager_request_t *request,
    uint8_t *output, size_t capacity, esy_remote_manager_response_t *response);

#ifdef __cplusplus
}
#endif

#endif /* ESY_REMOTE_MANAGER_H */
