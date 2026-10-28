/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_boot.h"
#include "esy_hal_backend/esy_hb_system.h"

esy_err_t esy_hb_boot_get_runtime(esy_boot_runtime_t *runtime) {
  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
  const uintptr_t pc = esy_hb_system_get_pc();
  if (pc > UINT32_MAX) return ESY_ERR_NOT_SUPPORTED;
  *runtime = (esy_boot_runtime_t){
      .mode = ESY_BOOT_MODE_DIRECT,
      .running_address = (uint32_t)pc,
      .update_allowed = true,
  };
  return ESY_OK;
}

esy_err_t esy_hb_boot_validate_layout(const esy_bootloader_layout_t *layout) {
  return layout == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_boot_set_pending_image(uint32_t address, uint32_t image_size) {
  (void)address;
  return image_size == 0u ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_boot_confirm_running_image(void) {
  return ESY_ERR_NOT_SUPPORTED;
}
