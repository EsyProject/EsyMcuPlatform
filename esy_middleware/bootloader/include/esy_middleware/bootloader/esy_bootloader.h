/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_bootloader.h
 * @brief Bootloader middleware interface.
 *
 * This header defines the reusable image and state contract for a dual-slot bootloader.
 * It provides:
 * - Slot selection helpers (selected slot, running slot, and update/flash slot)
 * - Firmware metadata (header) read/write
 * - Jump-to-application entry
 *
 * Typical dual-slot flow:
 * - Bootloader selects a slot to boot.
 * - OTA writes the new image into the "flash slot" (the inactive one).
 * - OTA marks the new image metadata as TRY_BOOT.
 * - After reboot, bootloader attempts to boot TRY_BOOT firmware and may mark it
 *   UNCONFIRMED/CONFIRMED based on platform policy and application
 * confirmation.
 *
 * Contract:
 * - All functions returning @ref esy_err_t return ESY_OK on success and a
 *   negative error code on failure.
 * - Slot indices are typically 0 or 1; passing other values is invalid unless
 *   a platform explicitly extends this contract.
 */

#ifndef ESY_BOOTLOADER_H
#define ESY_BOOTLOADER_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/image/esy_boot_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Firmware metadata structure size in bytes. */
#define ESY_FIRMWARE_METADATA_SIZE           256u
#define ESY_FIRMWARE_METADATA_MAGIC          0x4553594Du
#define ESY_FIRMWARE_METADATA_FORMAT_VERSION 1u
#define ESY_FIRMWARE_SHA256_BYTES            32u
#define ESY_FIRMWARE_SIGNATURE_BYTES         64u
#define ESY_FIRMWARE_SIGNED_MESSAGE_BYTES    40u

/**
 * @brief Firmware state recorded in slot metadata.
 *
 * The bootloader uses this state to decide boot strategy and rollback behavior.
 */
typedef enum {
  ESY_FIRMWARE_STATE_TRY_BOOT =
      0, /**< Image is newly written; bootloader should try booting it. */
  ESY_FIRMWARE_STATE_UNCONFIRMED =
      1, /**< Image booted successfully once but not confirmed by app yet. */
  ESY_FIRMWARE_STATE_CONFIRMED =
      2 /**< Image is confirmed as good; rollback should not occur. */
} esy_firmware_state_t;

/**
 * @brief Firmware metadata structure stored at a fixed location in each slot.
 *
 * Layout notes:
 * - The structure is padded to exactly @ref ESY_FIRMWARE_METADATA_SIZE bytes.
 * - The metadata CRC32 protects this complete structure with
 *   @ref metadata_crc32 treated as zero during calculation.
 * - The signature covers the canonical byte sequence
 *   `version_le32 || firmware_size_le32 || image_sha256`.
 */
typedef struct {
  uint32_t magic;          /**< @ref ESY_FIRMWARE_METADATA_MAGIC. */
  uint16_t format_version; /**< Metadata layout version. */
  uint8_t state;     /**< Firmware state (see @ref esy_firmware_state_t). */
  uint8_t reserved0; /**< Reserved; must be zero. */
  uint32_t version;  /**< Firmware version number. */
  uint32_t firmware_size; /**< Firmware image size in bytes. */
  uint8_t image_sha256[ESY_FIRMWARE_SHA256_BYTES]; /**< Firmware SHA-256. */
  uint8_t signature[ESY_FIRMWARE_SIGNATURE_BYTES]; /**< Detached signature. */
  uint32_t metadata_crc32; /**< CRC32 of the metadata structure. */
  uint8_t padding[ESY_FIRMWARE_METADATA_SIZE - 116u];
} esy_firmware_header_t;

/**
 * @brief Verify a firmware metadata signature.
 *
 * @param[in] context   Caller-provided verification context.
 * @param[in] digest    SHA-256 digest of the canonical signed metadata.
 * @param[in] signature Raw ECDSA P-256 signature encoded as `r || s`.
 *
 * @retval true Signature is valid.
 * @retval false Signature is invalid.
 */
typedef bool (*esy_bootloader_verify_signature_fn)(
    void *context, const uint8_t digest[ESY_FIRMWARE_SHA256_BYTES],
    const uint8_t signature[ESY_FIRMWARE_SIGNATURE_BYTES]);

/**
 * @brief Bootloader initialization configuration.
 */
typedef struct {
  esy_bootloader_verify_signature_fn
      verify_signature; /**< Product-owned signature verification callback. */
  void *verify_context; /**< Context passed to @ref verify_signature. */
} esy_bootloader_config_t;

/**
 * @brief Initialize the bootloader module.
 *
 * Loads selected slot information (and other required boot metadata) and
 * prepares internal state for subsequent queries/jump.
 *
 * Synchronous startup/task context only, with externally serialized Flash and
 * metadata access. Each call discards the previous selection. Direct mode
 * verifies both images and persists TRY_BOOT to UNCONFIRMED before selecting
 * each trial candidate. If promotion fails, that candidate is excluded for
 * this call; another trial must also be promoted before selection. If no trial
 * can be promoted, existing authenticated nontrial candidates remain eligible.
 * Managed mode verifies
 * only the currently running image and preserves the native bootloader's
 * selection and rollback state. Failure leaves no selected slot and
 * retains no configuration or callback context. The callback executes during
 * this call and must not reenter bootloader APIs. No OS resources are created.
 *
 * @param[in] config Bootloader configuration with a verification callback.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration.
 * @retval <0     Failure (platform-specific).
 */
extern esy_err_t esy_bootloader_init(
    const esy_bootloader_config_t *config);

/**
 * @brief Jump to the application in the selected slot (**does not return on
 * success**).
 *
 * This function transfers control to the application reset handler / entry
 * point of the selected slot.
 *
 * @note On successful jump, this function does not return.
 *       If it returns, the jump failed.
 *       Managed-mode application startup already runs the native-selected image;
 *       do not repeat the direct-boot init/jump flow there. Use activation and a
 *       consumer-owned reboot when installing another image.
 *
 * @retval false Jump failed (only returned on failure).
 */
extern bool esy_bootloader_jump_to_app(void);

/**
 * @brief Get the slot selected by the bootloader.
 *
 * The selected slot is the one chosen by @ref esy_bootloader_init.
 *
 * @retval 0 Selected slot is slot 0.
 * @retval 1 Selected slot is slot 1.
 * @retval -1 Failure.
 */
extern int esy_bootloader_get_selected_slot(void);

/**
 * @brief Get the currently running slot index from HAL's Flash image identity.
 *
 * This is intended for application-side OTA code that needs to know which slot
 * is currently executing.
 *
 * @retval 0 Running slot is slot 0.
 * @retval 1 Running slot is slot 1.
 * @retval -1 Runtime identity is unavailable or outside the firmware slots.
 */
extern int esy_bootloader_get_running_slot(void);

/**
 * @brief Get the current flash/update slot index.
 *
 * The flash slot is the inactive slot relative to the currently running slot.
 * A native bootloader may prohibit updates until the running trial image is
 * confirmed; this function then fails before the OTA writer can erase it.
 *
 * @retval 0 Flash slot is slot 0.
 * @retval 1 Flash slot is slot 1.
 * @retval -1 Failure.
 */
extern int esy_bootloader_get_flash_slot(void);

/**
 * @brief Get the base address of the current flash/update slot.
 *
 * The returned address is the start of the slot image region (where the new
 * firmware image should be written).
 *
 * @return Slot base address; returns 0 on failure.
 */
extern uint32_t esy_bootloader_get_flash_slot_addr(void);

/**
 * @brief Get the image capacity of the current flash/update slot.
 *
 * The returned size covers only the slot image region and excludes metadata.
 * OTA callers must reject images larger than this value before erasing or
 * writing flash.
 *
 * @return Slot image capacity in bytes; returns 0 on failure.
 */
extern uint32_t esy_bootloader_get_flash_slot_size(void);

/**
 * @brief Submit an inactive TRY_BOOT image for the next reboot without resetting.
 *
 * Synchronous startup/task context only. Serialize with all OTA, metadata and
 * native boot-state writers. The slot must be inactive, updates must be allowed,
 * and its metadata must pass format, CRC and length validation in TRY_BOOT state.
 * The OTA caller must already have verified the image hash and product signature
 * policy; this call does not retain the init verifier or authenticate the image.
 * Direct boot mode reads this metadata at reboot. Managed mode also asks HAL to
 * persist the native boot target and validate its native image format.
 *
 * Metadata and native boot state are separate persistent records. A failure does
 * not roll metadata back and may leave a committed native selection after an I/O
 * failure. The caller must report failure and reconcile state before retrying.
 *
 * @param[in] slot Inactive image slot, zero or one.
 * @retval ESY_OK The image is submitted; reboot remains consumer-owned.
 * @retval ESY_ERR_INVALID_ARG Invalid slot index.
 * @retval ESY_ERR_STATE Updates are prohibited, the slot is active, or not TRY_BOOT.
 * @retval <0 Runtime, layout, metadata or native boot-state failure.
 */
esy_err_t esy_bootloader_activate_image(int slot);

/**
 * @brief Read firmware metadata from a specified slot.
 *
 * @param[in]  slot   Slot index (typically 0 or 1).
 * @param[out] header Output metadata buffer; must not be NULL.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid slot, flash read error, etc.).
 */
extern esy_err_t esy_bootloader_read_meta_data(
    int slot, esy_firmware_header_t *header);

/**
 * @brief Write firmware metadata to a specified slot.
 *
 * The complete metadata region is erased before programming the fixed header.
 * Every layout region must be disjoint, inside HAL geometry, program aligned
 * and own complete physical sectors. Invalid geometry/layout fails before I/O.
 *
 * @param[in] slot   Slot index (typically 0 or 1).
 * @param[in] header Input metadata; must not be NULL.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid slot, flash write error, etc.).
 */
extern esy_err_t esy_bootloader_write_meta_data(
    int slot, const esy_firmware_header_t *header);

/**
 * @brief Confirm the running image after consumer-owned startup health checks.
 *
 * Synchronous task or single-threaded startup context only. Serialize against
 * all metadata writers and OTA sessions. The consumer chooses when confirmation
 * is safe. This first records CONFIRMED metadata, preserving the version, hash
 * and signature, then confirms the native image in managed mode. Native state
 * remains authoritative for rollback. These persistent changes are not atomic;
 * if native confirmation fails, retry even when metadata is already CONFIRMED.
 * Direct mode performs no native confirmation.
 *
 * @retval ESY_OK Confirmation completed or was already recorded.
 * @retval ESY_ERR_FAIL The execution address is outside the image slots.
 * @retval <0 Layout, metadata or Flash I/O failure.
 */
esy_err_t esy_bootloader_mark_success(void);

#ifdef __cplusplus
}
#endif

#endif /* ESY_BOOTLOADER_H */
