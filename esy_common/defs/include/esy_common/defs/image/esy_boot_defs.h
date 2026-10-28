/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_BOOT_DEFS_H
#define ESY_BOOT_DEFS_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Slot count supported by the existing two-slot boot layout contract. */
#define ESY_BOOTLOADER_SLOT_COUNT 2u

/** @brief Owner of persistent application selection and trial-boot state. */
typedef enum {
  ESY_BOOT_MODE_DIRECT = 0, /**< Esy bootloader selects and enters an image. */
  ESY_BOOT_MODE_MANAGED = 1 /**< Native SDK bootloader owns selection/rollback. */
} esy_boot_mode_t;

/** @brief Current boot provider state, copied synchronously without ownership. */
typedef struct {
  esy_boot_mode_t mode; /**< Selection mechanism for this port. */
  uint32_t running_address; /**< Address inside the running image in Flash coordinates. */
  bool update_allowed; /**< False while the inactive image is needed for rollback. */
} esy_boot_runtime_t;

/**
 * @brief Physical image and metadata regions supplied by the board through HAL.
 *
 * This value type contains no board, vendor or OS objects. It describes resource
 * placement only; Flash geometry comes from HAL and selection policy belongs to the
 * bootloader middleware. The caller owns each returned copy.
 */
typedef struct {
  uint32_t slot_addr[ESY_BOOTLOADER_SLOT_COUNT]; /**< Image base for each slot. */
  uint32_t meta_addr[ESY_BOOTLOADER_SLOT_COUNT]; /**< Metadata base for each slot. */
  uint32_t slot_size; /**< Capacity of each image region in bytes. */
  /**
   * @brief Independently erasable capacity of each metadata region in bytes.
   *
   * Each entry belongs to the slot with the same index and must span complete
   * physical erase sectors. Capacities may differ between slots and are not
   * the serialized metadata record length.
   */
  uint32_t meta_size[ESY_BOOTLOADER_SLOT_COUNT];
} esy_bootloader_layout_t;

#endif /* ESY_BOOT_DEFS_H */
