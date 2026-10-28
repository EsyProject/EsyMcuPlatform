/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_flash.h
 * @brief Flash HAL backend interface (blocking, caller-managed concurrency).
 *
 * Design notes:
 * - All APIs return @ref esy_err_t: 0 indicates success, negative values indicate failure.
 * - All operations are synchronous and blocking; implementations may disable interrupts
 *   or enter critical sections.
 * - Flash must be erased before being written; alignment and erase granularity are
 *   the responsibility of the caller.
 * - Typical call sequence:
 *   `init()` → `erase()` / `write()` / `read()` → `deinit()`.
 *
 * @note
 * Multi-core or multi-threaded concurrent access must be serialized by the caller.
 *
 * @warning
 * Power loss or reset during erase/write may corrupt data. Upper layers should
 * implement validation and rollback mechanisms if required.
 */

#ifndef ESY_HB_FLASH_H
#define ESY_HB_FLASH_H

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
typedef void (*esy_hb_flash_operation_t)(void *arg);

/**
 * @brief Platform or OS provided flash-safe execution hook.
 *
 * The default HAL backend implementation disables interrupts only on the current
 * core. Multi-core RTOS ports may install a stronger hook that also keeps other
 * cores from executing out of XIP flash during erase/program operations.
 */
typedef esy_err_t (*esy_hb_flash_safe_execute_t)(
    esy_hb_flash_operation_t operation, void *arg);

/**
 * @brief Initialize the Flash module.
 *
 * Configures timing, unlocks Flash control registers, and prepares the
 * underlying controller for erase/write operations.
 *
 * @retval ESY_OK Success
 * @retval <0     Negative error code on failure
 */
extern esy_err_t esy_hb_flash_init(void);

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
extern esy_err_t esy_hb_flash_set_safe_execute(
    esy_hb_flash_safe_execute_t safe_execute);

/**
 * @brief Deinitialize the Flash module.
 *
 * Releases underlying resources, disables clocks if applicable,
 * and locks the Flash controller if required by the platform.
 */
extern void esy_hb_flash_deinit(void);

/**
 * @brief Read data from Flash memory.
 *
 * @param[in]  address Start address in the port's Flash coordinate system
 * @param[out] buffer  Output buffer
 * @param[in]  length  Number of bytes to read
 * @retval ESY_OK Success
 * @retval <0     Negative error code (invalid address, out of range, etc.)
 *
 * @note
 * Reads are typically not alignment-restricted, but on some platforms
 * unaligned access may be slower or may trigger a fault.
 */
extern esy_err_t esy_hb_flash_read(uint32_t address,
                                       void *buffer,
                                       size_t length);

/**
 * @brief Write data to Flash memory.
 *
 * @param[in] address Start address in the port's Flash coordinate system
 * @param[in] data    Input data buffer
 * @param[in] length  Number of bytes to write
 * @retval ESY_OK Success
 * @retval <0     Negative error code (not erased, misaligned, out of range, etc.)
 *
 * @warning
 * The target region MUST be erased (all bits set to 1) before writing.
 *
 * @note
 * Query esy_hb_flash_get_geometry() for program_size. Both the write address
 * and length must be multiples of that granularity.
 * Pico requires the entire source range in the main on-chip SRAM mapping.
 * XIP Flash, external memory and SRAM aliases are rejected with INVALID_ARG
 * before invoking the flash-safe hook or programming the target. Copy such
 * sources into caller-owned SRAM first. The backend does not stage source data;
 * keep it readable and unchanged until this synchronous call returns.
 */
extern esy_err_t esy_hb_flash_write(uint32_t address,
                                        const void *data,
                                        size_t length);

/**
 * @brief Erase Flash sectors.
 *
 * @param[in] address Start address (must be sector-aligned)
 * @param[in] length  Exact number of bytes to erase; must span complete sectors
 * @retval ESY_OK Success
 * @retval <0     Negative error code (misaligned, out of range, etc.)
 *
 * @note
 * Query esy_hb_flash_get_sector() for each sector boundary. Sector sizes may
 * differ. Partial sectors are rejected; the range is never rounded or expanded.
 *
 * @warning
 * Erase is a destructive operation and resets all bits in the target region to 1.
 */
extern esy_err_t esy_hb_flash_erase(uint32_t address,
                                        size_t length);

/**
 * @brief Check whether an address lies within a valid Flash region.
 *
 * @param[in] address Address in the port's Flash coordinate system
 * @retval true  Address is valid Flash memory
 * @retval false Address is invalid or out of range
 */
extern bool esy_hb_flash_is_valid_address(uint32_t address);

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
extern esy_err_t esy_hb_flash_get_geometry(esy_flash_geometry_t *geometry);

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
 * lifetime contract as esy_hb_flash_get_geometry(). Sector sizes need not
 * be uniform; use start_address and size_bytes for all erase boundary checks.
 */
extern esy_err_t esy_hb_flash_get_sector(uint32_t address,
                                      esy_flash_sector_t *sector);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_FLASH_H */
