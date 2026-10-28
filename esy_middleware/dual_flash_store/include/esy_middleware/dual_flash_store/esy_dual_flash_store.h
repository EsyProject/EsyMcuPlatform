/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_DUAL_FLASH_STORE_H
#define ESY_DUAL_FLASH_STORE_H

/**
 * @file esy_dual_flash_store.h
 * @brief Transactional storage of one latest binary record in two Flash regions.
 *
 * "Dual" refers to independently erasable data regions, not Flash chips or OTA
 * firmware image slots. Both regions belong to the same logical store.
 *
 * Calls are synchronous, blocking, and externally serialized in startup/task
 * context. They are forbidden in ISR/fault context; context is a caller
 * precondition, not dynamically detected. Serialize all handles and other Flash
 * users, including OTA, boot metadata and future filesystems. Regions must be
 * exclusively reserved, disjoint from images, metadata and filesystem volumes.
 * The caller initializes Flash and establishes its safe execution policy;
 * this module neither owns that policy nor initializes/deinitializes Flash.
 *
 * There is no allocation, OS dependency, worker, retry or timeout. HAL duration,
 * interrupt/core coordination and watchdog budgets remain caller obligations.
 * Each operation scans both regions and checks complete committed payloads.
 * Incomplete/corrupt records are ignored; any HAL read error or recognizable
 * unsupported format fails closed. A failed write/clear may already have
 * committed. Every subsequent operation rescans Flash, so inspect state before
 * deciding to retry. CRC detects corruption, not malicious modification.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of independently erasable record regions required per store. */
#define ESY_DUAL_FLASH_STORE_REGION_COUNT 2u
/** @brief Opaque handle capacity; not the persistent format or an ABI size promise. */
#define ESY_DUAL_FLASH_STORE_STORAGE_BYTES 128u

/**
 * @brief Caller-owned handle, initialized with `{0}` before its first init.
 *
 * Do not copy or move a live handle. Deinit releases borrowed RAM only and
 * restores the zero state. Keep the handle and work buffer alive until deinit;
 * do not clear them to bypass a live instance's lifecycle.
 */
typedef struct {
  union {
    uint8_t bytes[ESY_DUAL_FLASH_STORE_STORAGE_BYTES]; /**< Private bytes. */
    max_align_t alignment; /**< Natural alignment for private state. */
  } storage; /**< Opaque implementation storage. */
} esy_dual_flash_store_t;

/** @brief A caller-reserved range in the HAL Flash address coordinate system. */
typedef struct {
  uint32_t address; /**< First byte, aligned to a sector and program unit. */
  uint32_t size_bytes; /**< Complete sectors; also a multiple of program_size. */
} esy_dual_flash_store_region_t;

/** @brief Caller-owned RAM workspace borrowed for an initialized handle's lifetime. */
typedef struct {
  uint8_t *work_buffer; /**< Writable, HAL-programmable RAM; disjoint from handle. */
  size_t work_buffer_size; /**< At least HAL geometry program_size bytes. */
} esy_dual_flash_store_static_resources_t;

/** @brief Configuration copied by init; descriptor objects need not remain alive. */
typedef struct {
  esy_dual_flash_store_region_t regions[ESY_DUAL_FLASH_STORE_REGION_COUNT]; /**< Disjoint ranges. */
  const esy_dual_flash_store_static_resources_t *static_resources; /**< Required workspace. */
} esy_dual_flash_store_config_t;

/** @brief Latest committed state; an empty store can retain a clear generation. */
typedef struct {
  bool has_record; /**< True for a committed data record, including zero bytes. */
  uint32_t length; /**< Payload length, zero when no data record is present. */
  uint32_t capacity; /**< Largest payload accepted by either configured region. */
  uint64_t generation; /**< Zero when no valid transaction exists; never wraps. */
} esy_dual_flash_store_info_t;

/**
 * @brief Validate geometry, borrow workspace and scan the existing records.
 * @param[in,out] store Zero-initialized or successfully deinitialized handle.
 * @param[in] config Configuration; regions must cover whole physical sectors.
 * @retval ESY_OK Initialized; existing Flash contents were not changed.
 * @retval ESY_ERR_INVALID_ARG NULL, overlapping/out-of-range storage or bad alignment.
 * @retval ESY_ERR_NO_MEM Workspace/regions cannot accommodate the format.
 * @retval ESY_ERR_STATE Handle already initialized, copied or otherwise not zero.
 * @retval ESY_ERR_NOT_SUPPORTED Geometry/recognizable persistent version unsupported.
 * @retval ESY_ERR_CRC_FAIL Two valid generations are not consecutive.
 * @retval <0 HAL failure, propagated without treating unreadable storage as empty.
 * @note Failure preserves the handle; a failed first init can be retried.
 * Geometry, reserved ranges and workspace must remain fixed until deinit.
 */
esy_err_t esy_dual_flash_store_init(esy_dual_flash_store_t *store,
                              const esy_dual_flash_store_config_t *config);

/**
 * @brief Release borrowed RAM without modifying Flash or its global lifecycle.
 * @param[in,out] store Initialized handle or its zero initial state.
 * @retval ESY_OK Released; repeated deinit of a zero handle is harmless.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_STATE Invalid/copied handle.
 */
esy_err_t esy_dual_flash_store_deinit(esy_dual_flash_store_t *store);

/**
 * @brief Rescan Flash and return the latest committed state and capacity.
 * @param[in,out] store Initialized, exclusively owned handle.
 * @param[out] info Output, disjoint from the handle/workspace; unchanged on error.
 * @return ESY_OK, INVALID_ARG, STATE, NOT_SUPPORTED, CRC_FAIL or a HAL error.
 */
esy_err_t esy_dual_flash_store_get_info(esy_dual_flash_store_t *store,
                                  esy_dual_flash_store_info_t *info);

/**
 * @brief Read the complete latest data record after validating both regions.
 * @param[in,out] store Initialized handle.
 * @param[out] buffer Destination; NULL only when capacity is zero.
 * @param[in] capacity Writable bytes at buffer, disjoint from handle/workspace/length.
 * @param[out] length Payload bytes read; required and unchanged on any failure.
 * @retval ESY_OK Complete record read, including a valid zero-length record.
 * @retval ESY_ERR_STATE No data record or invalid/uninitialized handle.
 * @retval ESY_ERR_NO_MEM Destination too small; destination remains unchanged.
 * @retval ESY_ERR_INVALID_ARG Invalid/overlapping output storage.
 * @retval <0 Scan/HAL failure. A final payload read error may partially change buffer.
 */
esy_err_t esy_dual_flash_store_read(esy_dual_flash_store_t *store, void *buffer,
                              size_t capacity, size_t *length);

/**
 * @brief Validate the complete latest record, then copy one exact payload range.
 * @param[in,out] store Initialized, exclusively owned store and workspace.
 * @param[in] offset Payload byte offset, at most the committed length.
 * @param[out] buffer Destination disjoint from store/workspace; NULL only at zero length.
 * @param[in] length Exact bytes to copy, entirely inside the committed payload.
 * @return OK; INVALID_ARG for pointers, overlap or invalid/overflowing range;
 * STATE for absence/invalid handle; another scan/HAL error. Empty reads still
 * validate a present record. A final HAL read failure may partially alter output.
 * @note Serialized normal-context operation; retains no caller output. Used by
 * bound-file adapters without allocating a complete record buffer.
 */
esy_err_t esy_dual_flash_store_read_range(esy_dual_flash_store_t *store, uint32_t offset,
                                     void *buffer, size_t length);

/**
 * @brief Replace the latest record using the alternate region and a final commit.
 * @param[in,out] store Initialized handle.
 * @param[in] data Readable immutable payload; NULL only when length is zero.
 * @param[in] length Bytes to persist, at most get_info().capacity.
 * @retval ESY_OK New record committed and read back successfully.
 * @retval ESY_ERR_NO_MEM Payload exceeds the smaller region's capacity.
 * @retval ESY_ERR_INVALID_ARG Invalid payload or overlap with handle/workspace.
 * @retval ESY_ERR_STATE Invalid handle or exhausted UINT64_MAX generation.
 * @retval <0 Scan/HAL failure; the new record may already be committed.
 * @pre Payload must not alias either configured Flash region. Its bytes must
 * remain readable and unchanged until return. The implementation stages every
 * program unit through the caller's workspace, including Flash-resident input
 * outside the reserved regions when the platform permits reading that input.
 * The latest valid region is retained while its replacement is committed.
 */
esy_err_t esy_dual_flash_store_write(esy_dual_flash_store_t *store, const void *data,
                               size_t length);

/**
 * @brief Logically clear by committing a newer tombstone, without secure erase.
 * @param[in,out] store Initialized handle.
 * @retval ESY_OK Clear committed and verified, or store already logically empty.
 * @retval ESY_ERR_STATE Invalid handle or exhausted generation when a write is needed.
 * @retval <0 Scan/HAL failure; clear may already have committed.
 * @note A tombstone survives interrupted updates and retains its generation.
 * Older bytes remain in Flash; later physical corruption can expose an older
 * valid revision through fallback. This operation is not secure deletion.
 */
esy_err_t esy_dual_flash_store_clear(esy_dual_flash_store_t *store);

#ifdef __cplusplus
}
#endif

#endif /* ESY_DUAL_FLASH_STORE_H */
