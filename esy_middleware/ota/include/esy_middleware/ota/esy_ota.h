/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_OTA_H
#define ESY_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/coordination/esy_activity_guard_defs.h"
#include "esy_common/defs/image/esy_image_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_ota.h
 * @brief Caller-owned, encoding-independent sequential image upload sessions.
 *
 * One non-ISR task/main-loop owner serializes every API and provider access.
 * Calls are synchronous; provider duration belongs to the caller's execution
 * budget. No task, transport, scheduler, timer, Flash device or allocation is
 * created. Callbacks must not reenter this handle. Requests/output storage,
 * the handle, provider state and backing buffers must be mutually disjoint.
 * Only normalized manifests and byte spans cross this boundary.
 */

/**
 * @brief Storage/install operations, copied during init; context stays borrowed.
 *
 * begin claims an upload and validates the manifest before destructive work.
 * write accepts the entire block at the supplied offset on OK, including any
 * provider-owned cache; it need not be durable yet. A write failure may have
 * changed storage and terminates the whole session. finish flushes and verifies
 * the complete stored image without publishing boot selection. activate is
 * optional and separate; OK completes activation and releases provider borrows,
 * but never resets the MCU. Failed lifecycle operations may retain resources.
 * abort must release any acquired resources, including failed begin/finish or
 * activation; it is idempotent. Abort failure retains all provider/context/
 * backing-storage borrows for retry. It does not promise persistent rollback.
 * One core exclusively borrows a provider; do not share it with another live
 * core or call the descriptor directly while borrowed. Cleanup includes abort
 * after a failed begin, so partial provider acquisition remains releasable.
 * All callbacks are synchronous, non-ISR, retain no request pointers and must
 * not reenter this handle. No operation implies signature authentication.
 */
typedef struct {
    esy_err_t (*begin)(void *context, const esy_image_manifest_t *manifest); /**< Claim upload. */
    esy_err_t (*write)(void *context, uint32_t offset, const uint8_t *data,
                       size_t length); /**< Accept one complete sequential block. */
    esy_err_t (*finish)(void *context); /**< Flush/readback-verify without activation. */
    esy_err_t (*activate)(void *context); /**< Optional explicit installation activation. */
    esy_err_t (*abort)(void *context); /**< Idempotent checked cleanup. */
    void *context; /**< Borrowed provider context; NULL is permitted. */
} esy_ota_provider_t;

/** @brief Caller-owned memory borrowed through successful deinit. */
typedef struct {
    uint8_t *last_chunk_buffer; /**< Exact bytes of the most recently accepted block. */
    size_t last_chunk_capacity; /**< At least max_chunk_bytes. */
} esy_ota_static_resources_t;

/** @brief Declare named, caller-owned last-block storage. */
#define ESY_OTA_STATIC_STORAGE(name, chunk_bytes) \
    struct { uint8_t last_chunk_buffer[(chunk_bytes)]; } name
/** @brief Bind aggregate last-block storage; descriptor is copied during init. */
#define ESY_OTA_BIND_STATIC_RESOURCES(resources, storage) \
    do { \
        (resources)->last_chunk_buffer = (storage).last_chunk_buffer; \
        (resources)->last_chunk_capacity = sizeof((storage).last_chunk_buffer); \
    } while (0)

/** @brief Explicit configuration; provider/resource lifecycles remain owned by caller. */
typedef struct {
    esy_ota_provider_t provider; /**< Required begin/write/finish/abort; activate optional. */
    uint32_t max_image_bytes; /**< Positive manifest size budget. */
    uint32_t max_chunk_bytes; /**< Positive sequential block byte limit. */
    const esy_ota_static_resources_t *static_resources; /**< Required exact-repeat storage. */
    uint32_t (*clock_ms)(void *context); /**< Continuous modulo-2^32 millisecond clock. */
    void *clock_context; /**< Borrowed clock context. */
    esy_err_t (*new_session_id)(void *context, uint32_t *session_id); /**< Required nonzero fresh ID. */
    void *session_id_context; /**< Borrowed ID allocator context. */
    uint32_t progress_timeout_ms; /**< Zero disables; otherwise at most INT32_MAX. */
    const esy_activity_guard_t *activity_guard; /**< Optional borrowed active-upload guard. */
} esy_ota_config_t;

/**
 * @brief Caller-owned handle; initialize with {0}; fields are private by contract.
 * Do not copy/move/reset a live handle. Config/resource descriptors are copied;
 * provider, callbacks, guard and backing buffers remain alive until deinit
 * succeeds, including after cleanup failure. The guard is held in RECEIVING
 * and READY. Cleanup-required resources are retained, never cleared implicitly.
 */
typedef struct {
    esy_ota_config_t config; /**< Private copied configuration. */
    esy_ota_static_resources_t resources; /**< Private copied backing descriptor. */
    esy_image_manifest_t manifest; /**< Private accepted identity. */
    esy_image_status_t snapshot; /**< Private cached state. */
    uint32_t last_chunk_offset; /**< Private repeat identity. */
    size_t last_chunk_length; /**< Private exact-repeat length. */
    uint32_t last_progress_ms; /**< Private continuous clock sample. */
    bool initialized; /**< Private lifecycle state. */
    bool busy; /**< Private reentrancy guard, not a lock. */
    bool provider_owned; /**< Private cleanup obligation. */
    bool guard_held; /**< Private activity-guard borrow. */
    bool deadline_active; /**< Private one-shot progress wait enable. */
} esy_ota_t;

/**
 * @brief Copy a validated configuration into a zero/deinitialized handle.
 * @return OK, INVALID_ARG for missing resources/callbacks/budgets or timeout
 * above INT32_MAX; STATE on repeat init; BUSY during an active callback.
 * No provider lifecycle operation runs. Init failure preserves the handle.
 * clock_ms is required only for a nonzero timeout. Time wraps every 49.7 days;
 * while RECEIVING/READY the owner observes at intervals no greater than timeout
 * and prevents clock-stopping sleep. Provider/Flash blocking time is included.
 * BEGIN starts timing before guard/provider acquisition. BEGIN/WRITE/FINISH
 * sample again on completion against the previous deadline before refreshing
 * once; provider blocking is not exempt. Queries/repeated commands never renew it.
 * Terminal/cancelled waits remain disabled across arbitrary complete clock cycles.
 */
esy_err_t esy_ota_init(esy_ota_t *ota, const esy_ota_config_t *config);
/**
 * @brief Abort an unfinished upload and detach all borrows after checked cleanup.
 * @return OK for zero/detached or success; INVALID_ARG for NULL; BUSY during
 * callbacks; cleanup error otherwise, retaining the initialized handle/resources
 * for deinit retry. Never resets or rolls back published boot state.
 */
esy_err_t esy_ota_deinit(esy_ota_t *ota);
/**
 * @brief Admit a manifest and synchronously prepare RECEIVING before returning OK.
 * Matching manifest in RECEIVING/READY returns the same session without another
 * provider begin or deadline renewal; a different manifest is BUSY. A terminal
 * session may be replaced only after successful cleanup and a fresh nonzero ID.
 * The ID source must avoid reuse while stale input can exist, including across
 * connection rebinds; IDs are correlation, never authorization. Manifest fields
 * are compared explicitly, without struct padding. Owner policy precedes this API.
 */
esy_err_t esy_ota_begin(esy_ota_t *ota, const esy_image_manifest_t *manifest,
                        esy_image_status_t *status);
/**
 * @brief Accept next_offset or an exact repeat of the immediately preceding block.
 * Returns STATE for another offset/session or changed repeat bytes; no write
 * runs in those cases. Invalid lengths return INVALID_ARG. A write error may
 * be partial, terminates the upload and attempts checked provider cleanup.
 * Duplicate writes do not refresh time or call the provider. next_offset means
 * accepted bytes, not guaranteed durable bytes until successful finish.
 * The exact cached final block may also be replayed in READY/ACTIVATED without
 * reopening or renewing the session; no other terminal WRITE is accepted.
 */
esy_err_t esy_ota_write(esy_ota_t *ota, uint32_t session_id, uint32_t offset,
                        const uint8_t *data, size_t length,
                        esy_image_status_t *status);
/** @brief Verify a complete image once, enter READY, retain resources/deadline; repeats return cached success. */
esy_err_t esy_ota_finish(esy_ota_t *ota, uint32_t session_id,
                         esy_image_status_t *status);
/**
 * @brief Explicitly activate a READY image once; repeats return cached result.
 * The deadline is checked before entering the persistent provider operation.
 * Once activation succeeds its phase is ACTIVATED even if the synchronous call
 * crossed that deadline; published boot state is never disguised as a timeout
 * or cancellation. Guard-release failure retains ACTIVATED with an error result
 * and resources for deinit retry. The core never resets the MCU.
 */
esy_err_t esy_ota_activate(esy_ota_t *ota, uint32_t session_id,
                           esy_image_status_t *status);
/**
 * @brief Abort any non-ACTIVATED session using checked provider/guard cleanup.
 * Repeated ABORTED succeeds. An ACTIVATED session returns STATE. Cleanup failure
 * reports FAILED and retains resources so abort/deinit can retry. No persistent
 * rollback is implied. An ID mismatch does not affect the current upload.
 */
esy_err_t esy_ota_abort(esy_ota_t *ota, uint32_t session_id,
                        esy_image_status_t *status);
/** @brief Copy current state on the owner; performs no provider work or deadline renewal. */
esy_err_t esy_ota_status(esy_ota_t *ota, esy_image_status_t *status);
/**
 * @brief Observe the continuous clock and expire RECEIVING/READY at the boundary.
 * Performs at most one timeout transition and one checked provider/guard cleanup.
 * Owner calls regularly within the declared observation interval. No real-time
 * bound exists beyond callback/provider cost. Returns TIMEOUT on new expiry,
 * cleanup error when cleanup fails, otherwise OK; detached/NULL return STATE/
 * INVALID_ARG and reentry returns BUSY. Later polls do not reactivate a terminal
 * wait, including after an entire counter cycle. Operations also check expiry
 * before accepting new progress. status outputs, when provided, are required
 * by every operation other than poll/deinit and are reset before validation.
 */
esy_err_t esy_ota_poll(esy_ota_t *ota);

#ifdef __cplusplus
}
#endif
#endif /* ESY_OTA_H */
