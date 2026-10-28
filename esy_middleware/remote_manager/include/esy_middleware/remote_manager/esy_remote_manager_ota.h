/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_REMOTE_MANAGER_OTA_H
#define ESY_REMOTE_MANAGER_OTA_H

#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/image/esy_image_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Normalized OTA argument content sizes, independent of envelope bytes. */
#define ESY_REMOTE_MANAGER_OTA_MANIFEST_BYTES 104u
/** @brief WRITE argument prefix: session identifier and byte offset. */
#define ESY_REMOTE_MANAGER_OTA_WRITE_PREFIX_BYTES 8u
/** @brief STATUS/FINISH/ABORT/ACTIVATE session argument size. */
#define ESY_REMOTE_MANAGER_OTA_SESSION_BYTES 4u
/** @brief SLOT argument content: zero-based enumeration index. */
#define ESY_REMOTE_MANAGER_OTA_SLOT_INDEX_BYTES 4u
/** @brief CONFIRM/ROLLBACK identity content: logical slot, version and SHA-256. */
#define ESY_REMOTE_MANAGER_OTA_IMAGE_ID_BYTES 40u

/** @brief Complete management-controlled image transaction commands. */
typedef enum {
    ESY_REMOTE_MANAGER_OTA_INFO = 1, /**< Query current image and update limits. */
    ESY_REMOTE_MANAGER_OTA_BEGIN = 2, /**< Accept a manifest and create a session. */
    ESY_REMOTE_MANAGER_OTA_STATUS = 3, /**< Query a session, or current when zero. */
    ESY_REMOTE_MANAGER_OTA_WRITE = 4, /**< Submit a contiguous bounded chunk. */
    ESY_REMOTE_MANAGER_OTA_FINISH = 5, /**< Verify all received image bytes. */
    ESY_REMOTE_MANAGER_OTA_ABORT = 6, /**< Cancel an unactivated image session. */
    ESY_REMOTE_MANAGER_OTA_ACTIVATE = 7, /**< Activate an already verified image. */
    ESY_REMOTE_MANAGER_OTA_SLOT = 8, /**< Enumerate one logical image slot. */
    ESY_REMOTE_MANAGER_OTA_CONFIRM = 9, /**< Confirm the identified running image. */
    ESY_REMOTE_MANAGER_OTA_ROLLBACK = 10, /**< Select an identified fallback image. */
} esy_remote_manager_ota_command_t;

/**
 * @brief Exact image identity, independent of upload sessions and physical addresses.
 * Slot identifiers are provider-defined and need not equal enumeration indices.
 * Version is an opaque owner-defined code, including zero. SHA-256 identifies
 * image bytes; it does not authenticate the caller or replace signature policy.
 */
typedef struct {
    uint32_t slot; /**< Logical slot identifier, including zero. */
    uint32_t version; /**< Owner-defined image version. */
    uint8_t sha256[ESY_IMAGE_SHA256_BYTES]; /**< Digest of the complete image. */
} esy_remote_manager_image_id_t;

/** @brief Independent slot properties reported by the image provider. */
typedef enum {
    ESY_REMOTE_MANAGER_IMAGE_PRESENT = 1u << 0, /**< Identity describes a stored image. */
    ESY_REMOTE_MANAGER_IMAGE_RUNNING = 1u << 1, /**< Currently executing image. */
    ESY_REMOTE_MANAGER_IMAGE_PENDING = 1u << 2, /**< Persisted next-boot selection. */
    ESY_REMOTE_MANAGER_IMAGE_CONFIRMED = 1u << 3, /**< Boot owner accepted this image. */
    ESY_REMOTE_MANAGER_IMAGE_BOOTABLE = 1u << 4, /**< Provider validated boot eligibility. */
} esy_remote_manager_image_flag_t;

/**
 * @brief One copied slot snapshot, without Flash addresses or provider handles.
 * For a nonempty inventory, index is less than slot_count and capacity is
 * positive. Without PRESENT, version, digest, image_size and all flags are zero;
 * the slot identifier and capacity still describe an empty slot. With PRESENT,
 * image_size is positive and at most capacity. BOOTABLE means the provider
 * validated integrity, authentication and boot-policy requirements; it must not
 * be inferred merely from readable metadata. A zero-slot inventory is returned
 * for SLOT index zero as an entirely zero snapshot. Other out-of-range indices
 * return INVALID_ARG. Each SLOT call is an independent live snapshot: serialize
 * inventory changes or restart enumeration when slot_count/order changes.
 */
typedef struct {
    uint32_t slot_count; /**< Number of enumerable slots, with no fixed maximum. */
    uint32_t index; /**< Zero-based enumeration position, not a slot identifier. */
    esy_remote_manager_image_id_t image; /**< Slot identity; version/hash absent when empty. */
    uint32_t capacity; /**< Maximum image bytes for this slot. */
    uint32_t image_size; /**< Stored image bytes, zero when empty. */
    uint32_t flags; /**< Bitwise combination of esy_remote_manager_image_flag_t. */
} esy_remote_manager_image_slot_t;

/**
 * @brief Optional boot-image administration, independent of the upload engine.
 *
 * One non-ISR owner serializes these synchronous callbacks with all uploads,
 * boot-state writers and provider lifecycle operations. All pointers are
 * borrowed only until return. The provider must not reenter the manager and
 * must enforce its actual boot-state/Flash safety, health and signature policy.
 * NULL callbacks are unsupported and are not advertised. The context and every
 * dependency outlive all copied operations; the manager owns no lifecycle.
 *
 * SLOT reads one slot without mutation. CONFIRM compares the complete requested
 * identity with the running image before changing state; a stale identity
 * returns STATE without writes. Success returns that RUNNING|CONFIRMED image.
 * Repeating confirmation for the same image is idempotent and must complete
 * any native confirmation left unfinished by a prior partial failure.
 *
 * ROLLBACK compares the complete identity, verifies a nonrunning target against
 * current boot/trust policy and durably selects it without rebooting. Success
 * returns that BOOTABLE|PENDING image. Preserve its selection after subsequent
 * confirmation/reboots; blindly calling an upload ACTIVATE operation is not
 * sufficient. Repeating the same committed selection while the target remains
 * nonrunning is idempotent; after reboot, rollback to the running image is
 * rejected rather than rearming it. Reject
 * conflicting uploads, trial protection or pending boot transitions with STATE
 * or BUSY rather than destroying fallback data. No implicit confirm, erase,
 * upload abort, downgrade permission or reset is supplied by the manager.
 *
 * Errors may follow persistent partial effects. No generic rollback/replay is
 * possible: reconcile authoritative boot state before further mutations or
 * retries, even when SLOT metadata still appears unchanged. Response delivery
 * and any reboot after successful ROLLBACK remain consumer-owned. These calls
 * introduce no timeout or scheduling model; callback cost belongs to the owner.
 */
typedef struct {
    esy_err_t (*slot)(void *context, uint32_t index,
                      esy_remote_manager_image_slot_t *slot); /**< Read one snapshot. */
    esy_err_t (*confirm)(void *context, const esy_remote_manager_image_id_t *image,
                         esy_remote_manager_image_slot_t *slot); /**< Confirm exact running image. */
    esy_err_t (*rollback)(void *context, const esy_remote_manager_image_id_t *image,
                          esy_remote_manager_image_slot_t *slot); /**< Persist fallback selection. */
    void *context; /**< Borrowed boot-image owner, independent of upload context. */
} esy_remote_manager_image_ops_t;

/** @brief Logical image information; excludes physical Flash addresses. */
typedef struct {
    uint32_t current_version; /**< Owner-defined current version code. */
    int32_t running_slot; /**< Logical running slot; -1 when not applicable. */
    uint32_t max_image_bytes; /**< Owner/provider image size limit. */
    uint32_t max_chunk_bytes; /**< Data bytes per WRITE, excluding its prefix. */
} esy_remote_manager_ota_info_t;

/**
 * @brief Optional normalized image-owner operations copied by manager init.
 *
 * Calls execute synchronously on the serialized manager owner, never ISR.
 * The provider borrows all input/output pointers only until return. It must not
 * reenter the manager. BEGIN/WRITE/FINISH/ABORT/ACTIVATE return the resulting
 * status on success. Errors can follow a partial side effect; the owner queries
 * STATUS with the known session to reconcile next_offset and chooses retries.
 * STATUS session zero requests the current snapshot; mutations require nonzero
 * session IDs, except BEGIN which creates one. NULL operations are unsupported.
 * Authentication, admission, update policy, scheduling, response delivery and
 * reset remain with the owner. No sender, task or provider lifecycle is owned.
 * Optional image operations may remain zero for upload-only providers. The
 * upload context is the final member; initialize every positional field or use
 * named fields. Rebuild consumers after changing this public structure layout.
 */
typedef struct {
    esy_err_t (*info)(void *context, esy_remote_manager_ota_info_t *info); /**< Image/limits. */
    esy_err_t (*begin)(void *context, const esy_image_manifest_t *manifest,
                       esy_image_status_t *status); /**< Open a transaction. */
    esy_err_t (*status)(void *context, uint32_t session_id,
                        esy_image_status_t *status); /**< Copy status. */
    esy_err_t (*write)(void *context, uint32_t session_id, uint32_t offset,
                       const uint8_t *data, size_t length,
                       esy_image_status_t *status); /**< Submit one chunk. */
    esy_err_t (*finish)(void *context, uint32_t session_id,
                        esy_image_status_t *status); /**< Verify complete bytes. */
    esy_err_t (*abort)(void *context, uint32_t session_id,
                       esy_image_status_t *status); /**< Cancel preparation. */
    esy_err_t (*activate)(void *context, uint32_t session_id,
                          esy_image_status_t *status); /**< Activate ready image. */
    esy_remote_manager_image_ops_t images; /**< Optional session-independent boot operations. */
    void *context; /**< Borrowed upload owner context; NULL is permitted. */
} esy_remote_manager_ota_ops_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_REMOTE_MANAGER_OTA_H */
