/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_flash.h
 * @brief HAL Flash unified interface.
 *
 * This header defines the HAL Flash abstraction used by middleware
 * and applications (e.g., OTA, storage).
 *
 * Contract:
 * - All functions returning @ref esy_err_t return ESY_OK on success, negative values on failure.
 * - All APIs are synchronous/blocking; during erase/program the implementation may enter critical
 *   sections or temporarily disable interrupts.
 * - In general, callers must ensure the target region is erased before programming. Erase/program
 *   alignment and minimum granularity are platform-dependent.
 * - Typical usage: init() -> erase()/write()/read() -> deinit().
 *
 * Concurrency and safety:
 * - When accessed from multiple cores/threads, mutual exclusion and ordering must be ensured by
 *   the HAL or upper layer (this interface does not provide internal locking).
 * - Power loss/reset during erase/program may leave data inconsistent; upper layers should use
 *   verification and rollback strategies when needed.
 */

#ifndef ESY_HAL_FLASH_H
#define ESY_HAL_FLASH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_flash_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Flash mutation callback executed while the platform is flash-safe.
 */
typedef void (*esy_hal_flash_operation_t)(void *arg);

/**
 * @brief Platform flash-safe execution hook.
 *
 * The hook must execute @p operation exactly once when it returns ESY_OK. It
 * may disable interrupts, coordinate peer cores, or use other platform
 * mechanisms required to make erase/program operations safe.
 */
typedef esy_err_t (*esy_hal_flash_safe_execute_t)(
    esy_hal_flash_operation_t operation, void *arg);

/**
 * @brief Initialize the Flash module.
 *
 * Sets up platform-specific resources and controller state required for Flash operations.
 * Repeated calls are allowed; re-entrancy and reference counting behavior are implementation-defined.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure.
 */
extern esy_err_t esy_hal_flash_init(void);

/**
 * @brief Install a flash-safe execution hook and report whether it is supported.
 *
 * @param[in] safe_execute Hook to install. NULL restores the port's default
 * behavior, including on ports without custom hook support.
 * @retval ESY_OK The requested hook/default behavior is installed.
 * @retval ESY_ERR_NOT_SUPPORTED A non-NULL hook is unsupported by this port.
 * @retval <0 Registration failed; the previously installed behavior is unchanged.
 *
 * Call from startup or task context, serialized against registration and all
 * flash operations. The callback code must remain valid until replaced or reset;
 * it executes synchronously in the flash caller's context. Repeated registration
 * is allowed. Check the result before relying on custom safety or activity guards.
 * Pico supports custom hooks; STM32F4 and ESP32 currently reject non-NULL hooks.
 */
extern esy_err_t esy_hal_flash_set_safe_execute(
    esy_hal_flash_safe_execute_t safe_execute);

/**
 * @brief Deinitialize the Flash module.
 *
 * Releases resources and restores peripheral/controller state if needed. If Flash access is needed
 * again, call @ref esy_hal_flash_init.
 *
 * @note This function is typically idempotent, but the exact behavior is implementation-defined.
 */
extern void esy_hal_flash_deinit(void);

/**
 * @brief Read bytes from Flash into a buffer.
 *
 * @param[in]  address Start address to read from (port's Flash coordinate system).
 * @param[out] buffer  Destination buffer pointer (must be writable and non-NULL).
 * @param[in]  length  Number of bytes to read.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (invalid address/range, access fault, etc.).
 *
 * @note Some platforms have poor performance or restrictions for unaligned accesses.
 *       Ensure @p address and @p length meet the platform constraints.
 */
extern esy_err_t esy_hal_flash_read(uint32_t address, void *buffer, size_t length);

/**
 * @brief Program (write) bytes into Flash.
 *
 * The target region is expected to be erased prior to programming, and @p address/@p length must
 * satisfy platform programming granularity and alignment constraints.
 *
 * @param[in] address Start address to write to (port's Flash coordinate system).
 * @param[in] data    Source data pointer (must be readable and non-NULL).
 * @param[in] length  Number of bytes to program.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (not erased, misaligned, out-of-range, hardware error, etc.).
 *
 * @warning Misaligned programming may fail. Perform alignment and boundary checks before calling.
 * @note Query esy_hal_flash_get_geometry() for the required program_size.
 *       Both address and length must be multiples of program_size.
 *       Pico requires the entire source range in the main on-chip SRAM mapping;
 *       XIP Flash, external memory and SRAM aliases return INVALID_ARG before
 *       entering the flash-safe hook or programming. Copy those sources into
 *       caller-owned SRAM first. The backend does not stage data; keep the source
 *       readable and unchanged until this synchronous call returns.
 */
extern esy_err_t esy_hal_flash_write(uint32_t address, const void *data, size_t length);

/**
 * @brief Erase a Flash region.
 *
 * The range must contain only complete sectors. Its start must equal a sector
 * start and its end must equal a sector end. Partial sectors are rejected; the
 * implementation never expands the requested range. Query each sector because
 * sector sizes may differ within one device.
 *
 * @param[in] address Sector start in the port's Flash coordinate system.
 * @param[in] length  Exact erase length in bytes; must span complete sectors.
 *
 * @retval ESY_OK Success.
 * @retval <0     Failure (misaligned, out-of-range, hardware error, etc.).
 *
 * @warning Erase is destructive: all bits in the affected sectors are set to 1.
 *          Ensure data migration/backup and higher-layer verification are handled appropriately.
 */
extern esy_err_t esy_hal_flash_erase(uint32_t address, size_t length);

/**
 * @brief Check whether an address lies within a valid Flash-accessible range.
 *
 * This function checks address validity according to platform-defined Flash mapping.
 *
 * @param[in] address Address in the port's Flash coordinate system.
 *
 * @retval true  Address is valid.
 * @retval false Address is invalid.
 */
extern bool esy_hal_flash_is_valid_address(uint32_t address);

/**
 * @brief Query the active device's physical Flash geometry.
 *
 * @param[out] geometry Caller-owned output; unchanged on any failure.
 * @retval ESY_OK Geometry is available.
 * @retval ESY_ERR_INVALID_ARG NULL output pointer.
 * @retval ESY_ERR_NOT_SUPPORTED The port cannot determine a supported geometry.
 * @retval <0 Platform query failed.
 *
 * This read-only, synchronous query is allowed before Flash init, after the
 * platform SDK and board configuration are ready. Call from startup/task context
 * under the same serialized owner as other Flash APIs; not from an ISR. It does
 * not allocate memory, unlock Flash, or mutate the controller. All geometry
 * fields are nonzero except base_address, which may be zero. Pico/STM32F4 use
 * mapped absolute addresses; ESP32 uses physical offsets from zero. Product
 * image and metadata layouts are separate from this physical description.
 */
extern esy_err_t esy_hal_flash_get_geometry(esy_flash_geometry_t *geometry);

/**
 * @brief Query the complete erase sector containing an address.
 *
 * @param[in] address Address in the active port's Flash coordinate system.
 * @param[out] sector Caller-owned output; unchanged on any failure.
 * @retval ESY_OK Sector start and size are available.
 * @retval ESY_ERR_INVALID_ARG NULL output or address outside the Flash device.
 * @retval ESY_ERR_NOT_SUPPORTED The port cannot determine a supported geometry.
 * @retval <0 Platform query failed.
 *
 * Uses the same pre-init, serialized owner, non-ISR context and read-only
 * lifetime contract as esy_hal_flash_get_geometry(). Sector sizes need not
 * be uniform; use start_address and size_bytes for all erase boundary checks.
 */
extern esy_err_t esy_hal_flash_get_sector(uint32_t address,
                                       esy_flash_sector_t *sector);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_FLASH_H */
