/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_IMAGE_DEFS_H
#define ESY_IMAGE_DEFS_H

#include <stdbool.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Image manifest SHA-256 byte count. */
#define ESY_IMAGE_SHA256_BYTES 32u
/** @brief Image manifest raw signature byte count; authentication is provider policy. */
#define ESY_IMAGE_SIGNATURE_BYTES 64u

/** @brief Normalized image identity; no transport, Flash or bootloader layout. */
typedef struct {
    uint32_t version; /**< Nonzero version accepted by the owner policy. */
    uint32_t image_size; /**< Nonzero complete image length. */
    uint8_t image_sha256[ESY_IMAGE_SHA256_BYTES]; /**< Expected stored-image hash. */
    uint8_t signature[ESY_IMAGE_SIGNATURE_BYTES]; /**< Opaque signature; authentication follows provider/boot policy. */
} esy_image_manifest_t;

/** @brief Observable upload phases; fixed values are independent of encoding. */
typedef enum {
    ESY_IMAGE_IDLE = 0, /**< No upload has been admitted. */
    ESY_IMAGE_RECEIVING = 1, /**< Ready for sequential image blocks. */
    ESY_IMAGE_READY = 2, /**< Complete image verified; activation remains explicit. */
    ESY_IMAGE_ACTIVATED = 3, /**< Activation succeeded; reboot remains owner-controlled. */
    ESY_IMAGE_ABORTED = 4, /**< Upload cancelled and cleanup completed. */
    ESY_IMAGE_FAILED = 5, /**< Operation, timeout or cleanup failed. */
} esy_image_phase_t;

/** @brief Copied session state; request IDs and authentication are separate. */
typedef struct {
    esy_image_phase_t phase; /**< Current lifecycle phase. */
    uint32_t session_id; /**< Nonzero upload identity; zero only before first upload. */
    uint32_t next_offset; /**< Contiguous accepted prefix, potentially still cached. */
    uint32_t total_bytes; /**< Manifest image length. */
    esy_err_t result; /**< Last terminal outcome, valid only when result_valid. */
    bool result_valid; /**< True in ACTIVATED, ABORTED or FAILED. */
} esy_image_status_t;

#ifdef __cplusplus
}
#endif
#endif /* ESY_IMAGE_DEFS_H */
