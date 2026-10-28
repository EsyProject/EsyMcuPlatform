/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_boot.h"

#include "esy_board/esy_board.h"
#include "esy_hal_backend/esy_hb_boot.h"

esy_err_t esy_hal_boot_get_layout(esy_bootloader_layout_t *layout) {
  if (layout == NULL) return ESY_ERR_INVALID_ARG;
  esy_bootloader_layout_t local = {0};
  if (!esy_board_get_bootloader_layout(&local)) return ESY_ERR_NOT_SUPPORTED;
  esy_boot_runtime_t runtime;
  esy_err_t ret = esy_hal_boot_get_runtime(&runtime);
  if (ret != ESY_OK) return ret;
  if (runtime.mode == ESY_BOOT_MODE_MANAGED) {
    ret = esy_hb_boot_validate_layout(&local);
    if (ret != ESY_OK) return ret;
  }
  *layout = local;
  return ESY_OK;
}

esy_err_t esy_hal_boot_get_runtime(esy_boot_runtime_t *runtime) {
  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
  esy_boot_runtime_t local;
  esy_err_t ret = esy_hb_boot_get_runtime(&local);
  if (ret != ESY_OK) return ret;
  if (local.mode != ESY_BOOT_MODE_DIRECT &&
      local.mode != ESY_BOOT_MODE_MANAGED) return ESY_ERR_STATE;
  *runtime = local;
  return ESY_OK;
}

esy_err_t esy_hal_boot_set_pending_image(uint32_t address, uint32_t image_size) {
  if (image_size == 0u ||
      (uint64_t)address + image_size > (uint64_t)UINT32_MAX + 1u)
    return ESY_ERR_INVALID_ARG;
  return esy_hb_boot_set_pending_image(address, image_size);
}

esy_err_t esy_hal_boot_confirm_running_image(void) {
  return esy_hb_boot_confirm_running_image();
}

void esy_hal_boot_jump_to_app(uint32_t app_address) {
  esy_hb_boot_jump_to_app(app_address);
}
