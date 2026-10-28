/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_ESY_BOOTLOADER_H
#define FAKE_ESY_BOOTLOADER_H

#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/bootloader/esy_bootloader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  esy_err_t init_result;
  esy_err_t mark_success_result; /**< Configurable confirmation status. */
  uint32_t mark_success_count; /**< Number of confirmation requests. */
  bool jump_to_app_result;
  int selected_slot;
  int running_slot;
  int flash_slot;
  uint32_t flash_slot_addr;
  uint32_t flash_slot_size;
  esy_err_t read_meta_result;
  esy_err_t write_meta_result;
  esy_err_t activate_result; /**< Configurable inactive-image activation status. */
  uint32_t activate_count; /**< Number of image activation requests. */
  int last_activate_slot; /**< Last slot submitted for activation. */
  uint32_t meta_writes_at_activation; /**< Metadata writes before activation. */

  uint32_t init_count;
  uint32_t jump_to_app_count;
  uint32_t read_meta_count;
  uint32_t write_meta_count;

  int last_read_meta_slot;
  int last_write_meta_slot;
  esy_firmware_header_t read_meta_header;
  esy_firmware_header_t last_write_meta_header;
} fake_esy_bootloader_state_t;

void fake_esy_bootloader_reset(void);
fake_esy_bootloader_state_t *fake_esy_bootloader_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_BOOTLOADER_H */
