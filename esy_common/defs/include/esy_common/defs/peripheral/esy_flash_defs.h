/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FLASH_DEFS_H
#define ESY_FLASH_DEFS_H

#include <stdint.h>

/**
 * @brief Physical Flash geometry exposed by the active hardware port.
 *
 * Addresses use the same coordinate system as the Flash read/write/erase APIs:
 * Pico and STM32F4 use mapped absolute addresses; ESP32 uses physical offsets
 * from zero. The geometry describes one contiguous address range. These
 * caller-owned values contain no hardware or allocation state. The SDK/board
 * configuration must remain unchanged while Flash clients use this geometry.
 */
typedef struct {
  uint32_t base_address; /**< First accessible address in this Flash device. */
  uint32_t size_bytes; /**< Accessible capacity in bytes; always nonzero. */
  uint32_t program_size; /**< Required address and length granularity for writes. */
} esy_flash_geometry_t;

/**
 * @brief One complete, physically erasable Flash sector.
 *
 * Sector sizes may differ within a device. Sectors partition the complete
 * geometry range without gaps or overlaps. Addresses use the coordinate system
 * of @ref esy_flash_geometry_t; callers own the returned value.
 */
typedef struct {
  uint32_t start_address; /**< Inclusive address of the sector's first byte. */
  uint32_t size_bytes; /**< Sector size in bytes; always nonzero. */
} esy_flash_sector_t;

#endif /* ESY_FLASH_DEFS_H */
