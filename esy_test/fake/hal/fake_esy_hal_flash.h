/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_HAL_FLASH_H
#define FAKE_ESY_HAL_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_hal/esy_hal_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_ESY_HAL_FLASH_STORAGE_BYTES 256u

typedef struct {
  esy_hal_flash_safe_execute_t safe_execute; /**< Captured hook; tests invoke it explicitly. */
  esy_err_t safe_execute_result; /**< Registration result; failure preserves the hook. */
  uint32_t safe_execute_set_count; /**< Number of hook registrations. */
  uint32_t sector_size; /**< Sector size before the optional transition. */
  uint32_t sector_size_after; /**< Nonzero enables a second sector size. */
  uint32_t sector_size_after_address; /**< First sector boundary of the second region. */
  uint32_t storage_base_addr; /**< Geometry base and backing storage origin. */
  uint32_t size_bytes; /**< Physical geometry capacity, independently configurable. */
  uint32_t program_size; /**< Required write address and length granularity. */
  esy_err_t geometry_result; /**< Forced geometry error; output remains unchanged. */
  esy_err_t sector_result; /**< Forced sector error; output remains unchanged. */
  uint8_t *backing_storage; /**< Optional caller-owned image storage; NULL uses storage.
    Existing bytes are retained when bound; tests initialize them explicitly. */
  size_t backing_storage_size; /**< Capacity of backing_storage when non-NULL. */
  esy_err_t init_result;
  esy_err_t read_result;
  esy_err_t write_result;
  esy_err_t erase_result;
  uint32_t erase_failures_remaining; /**< Fail this many erases before erase_result applies. */
  esy_err_t erase_transient_result; /**< Countdown failure code; defaults to ESY_ERR_IO. */
  uint32_t fail_read_call; /**< Absolute read_count to fail once; zero disables. */
  uint32_t fail_write_call; /**< Absolute write_count to fail once; zero disables. */
  uint32_t fail_erase_call; /**< Absolute erase_count to fail once; zero disables. */
  size_t read_fail_bytes; /**< Prefix copied before the selected read fails. */
  size_t write_fail_bytes; /**< Prefix programmed before the selected write fails. */
  size_t erase_fail_bytes; /**< Prefix erased before the selected erase fails. */
  esy_err_t injected_error; /**< Selected-call error; reset default and ESY_OK map to ESY_ERR_IO. */
  uint8_t *programmed_units; /**< Optional caller-owned one-byte marks per physical
    program unit, starting at floor(storage_base_addr / program_size). Nonzero
    rejects repeated programming until the whole unit is erased. Tests initialize
    marks to match the image and retain them across simulated resets. NULL disables. */
  size_t programmed_units_size; /**< Capacity of programmed_units in bytes. */
  bool valid_address;

  uint32_t geometry_count;
  uint32_t sector_count;
  uint32_t last_sector_address;
  uint32_t init_count;
  uint32_t deinit_count;
  uint32_t read_count;
  uint32_t write_count;
  uint32_t erase_count;

  uint32_t last_read_addr;
  size_t last_read_len;
  uint32_t last_write_addr;
  size_t last_write_len;
  uint32_t last_erase_addr;
  size_t last_erase_len;
  uint32_t last_valid_address;
  uint8_t last_write_data[64];
  uint8_t storage[FAKE_ESY_HAL_FLASH_STORAGE_BYTES];
} fake_esy_hal_flash_state_t;

/** @brief Reset controls and erase internal storage to 0xFF.
 * Successful erase changes exactly the validated sectors to 0xFF. Programming
 * permits only 1-to-0 bit changes; a 0-to-1 attempt returns ESY_ERR_IO without
 * changing any bytes. Read, program and erase preflight the complete physical
 * geometry and backing capacity; validation failures leave bytes unchanged.
 * Permanent result controls and transient erase failures take precedence and
 * leave bytes unchanged. Selected-call injection applies only after validation;
 * fail-byte counts select zero, partial or complete effects before returning the
 * injected error (SIZE_MAX always selects the complete operation). A counter
 * includes rejected calls; a selected invalid call is not deferred. Optional
 * program-unit marks are capacity-checked before mutation, including erase.
 * Reset unbinds caller-owned storage and marks without modifying either. This
 * synchronous model does not simulate asynchronous completion or timing. */
void fake_esy_hal_flash_reset(void);
fake_esy_hal_flash_state_t *fake_esy_hal_flash_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_FLASH_H */
