/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_flash.h"

#include "esy_hal_backend/esy_hb_flash.h"

esy_err_t esy_hal_flash_init(void) { return esy_hb_flash_init(); }

esy_err_t esy_hal_flash_set_safe_execute(
    esy_hal_flash_safe_execute_t safe_execute) {
  return esy_hb_flash_set_safe_execute(safe_execute);
}

void esy_hal_flash_deinit(void) { esy_hb_flash_deinit(); }

esy_err_t esy_hal_flash_read(uint32_t address, void *buffer, size_t length) {
  if (!buffer || length == 0u) return ESY_ERR_INVALID_ARG;
  return esy_hb_flash_read(address, buffer, length);
}

esy_err_t esy_hal_flash_write(uint32_t address, const void *data,
                             size_t length) {
  if (!data || length == 0u) return ESY_ERR_INVALID_ARG;
  return esy_hb_flash_write(address, data, length);
}

esy_err_t esy_hal_flash_erase(uint32_t address, size_t length) {
  if (length == 0u) return ESY_ERR_INVALID_ARG;
  return esy_hb_flash_erase(address, length);
}

bool esy_hal_flash_is_valid_address(uint32_t address) {
  return esy_hb_flash_is_valid_address(address);
}

esy_err_t esy_hal_flash_get_geometry(esy_flash_geometry_t *geometry) {
  if (!geometry) return ESY_ERR_INVALID_ARG;
  esy_flash_geometry_t value;
  esy_err_t result = esy_hb_flash_get_geometry(&value);
  if (result == ESY_OK) *geometry = value;
  return result;
}

esy_err_t esy_hal_flash_get_sector(uint32_t address,
                                    esy_flash_sector_t *sector) {
  if (!sector) return ESY_ERR_INVALID_ARG;
  esy_flash_sector_t value;
  esy_err_t result = esy_hb_flash_get_sector(address, &value);
  if (result == ESY_OK) *sector = value;
  return result;
}
