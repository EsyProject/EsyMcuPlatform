/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_OTA_FLASH_H
#define ESY_OTA_FLASH_H

#include <stddef.h>
#include <stdint.h>
#include "esy_middleware/ota/esy_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @file esy_ota_flash.h
 * @brief Optional native Flash/bootloader provider for typed image uploads.
 */

/** @brief Caller-owned provider handle capacity, checked against its private layout. */
#define ESY_OTA_FLASH_STORAGE_BYTES 256u

/**
 * @brief Caller-owned Flash provider, requiring {0} before first init.
 * Never copy/move/reset a live handle. One serialized non-ISR owner controls
 * provider/core/boot confirmation and every other user of the Flash controller.
 * Stop and successfully deinitialize the borrowing OTA core before deinit.
 * Flash operations may block, mask interrupts or stall instruction fetch.
 */
typedef struct {
    union {
        max_align_t alignment; /**< Private storage alignment. */
        uint8_t bytes[ESY_OTA_FLASH_STORAGE_BYTES]; /**< Private provider state. */
    } storage; /**< Private in-place state. */
} esy_ota_flash_t;

/** @brief Caller-owned native programming cache, borrowed until provider deinit. */
typedef struct {
    uint8_t *cache_buffer; /**< Writable cache disjoint from core's last-block memory. */
    size_t cache_size; /**< Nonzero, at most UINT32_MAX; a multiple of program size. */
} esy_ota_flash_static_resources_t;

/** @brief Descriptor copied by init; backing cache remains borrowed. */
typedef struct {
    const esy_ota_flash_static_resources_t *static_resources; /**< Required cache. */
} esy_ota_flash_config_t;

/**
 * @brief Borrow the cache without initializing Flash or inspecting boot metadata.
 * @return OK, INVALID_ARG for missing/invalid cache, STATE for repeated init.
 * Failure preserves the handle; no external resources are acquired.
 */
esy_err_t esy_ota_flash_init(esy_ota_flash_t *flash,
                             const esy_ota_flash_config_t *config);
/**
 * @brief Detach a quiescent provider without modifying Flash or boot selection.
 * @return INVALID_ARG for NULL, otherwise OK, including repeated zero deinit.
 * An active OTA borrower must first be aborted/deinitialized by its owner.
 */
esy_err_t esy_ota_flash_deinit(esy_ota_flash_t *flash);
/**
 * @brief Copy provider operations borrowing an initialized Flash handle.
 * @return OK, INVALID_ARG for NULL, STATE for an uninitialized provider.
 * Begin validates the complete inactive-slot geometry before destructive work.
 * Write is sequential and may retain bytes in RAM. Finish flushes and verifies
 * SHA-256 by reading stored image bytes, retaining the slot/cache until explicit
 * activate or abort. Activate writes TRY_BOOT metadata then submits boot target;
 * these persistent changes are not atomic and failure may already have effects.
 * Neither finish nor activate authenticates the signature or resets the MCU.
 * Abort clears only transient provider state, not persistent data/boot records.
 * Every activation attempt latches publication: later begin returns STATE even
 * after abort, so an image awaiting reboot cannot be overwritten. The owner must
 * reconcile boot state before deinit/reinit clears this latch; deinit itself
 * does not perform or establish that reconciliation.
 * Returned ops, Flash configuration, cache and handle must remain valid through
 * successful OTA deinit. Reconcile activation failures before another upload.
 */
esy_err_t esy_ota_flash_get_provider(esy_ota_flash_t *flash,
                                     esy_ota_provider_t *provider);

#ifdef __cplusplus
}
#endif
#endif /* ESY_OTA_FLASH_H */
