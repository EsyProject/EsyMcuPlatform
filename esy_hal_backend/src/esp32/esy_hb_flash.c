/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_flash.h"

#include "esp_flash.h"
#include "spi_flash_chip_driver.h"

typedef struct {
  bool initialized;
} esy_hb_flash_ctx_t;

static esy_hb_flash_ctx_t s_ctx;

static esy_err_t query_geometry(esy_flash_geometry_t *geometry,
                                  uint32_t *erase_size) {
  if (esp_flash_default_chip == NULL ||
      esp_flash_default_chip->chip_drv == NULL) {
    return ESY_ERR_STATE;
  }
  uint32_t capacity = 0u;
  if (esp_flash_get_size(NULL, &capacity) != ESP_OK) return ESY_ERR_IO;
  const uint32_t sector_size = esp_flash_default_chip->chip_drv->sector_size;
  if (capacity == 0u || sector_size == 0u ||
      (capacity % sector_size) != 0u) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  const esy_flash_geometry_t result = {
      .base_address = 0u,
      .size_bytes = capacity,
      /* The ordinary SDK write API accepts byte-aligned data and addresses. */
      .program_size = 1u,
  };
  *geometry = result;
  *erase_size = sector_size;
  return ESY_OK;
}

static bool flash_range_is_valid(const esy_flash_geometry_t *geometry,
                                  uint32_t address, size_t length) {
  return length != 0u && address < geometry->size_bytes &&
         length <= (size_t)(geometry->size_bytes - address);
}

esy_err_t esy_hb_flash_init(void) {
  if (s_ctx.initialized) {
    return ESY_OK;
  }

  esy_flash_geometry_t geometry;
  esy_err_t ret = esy_hb_flash_get_geometry(&geometry);
  if (ret != ESY_OK) return ret;
  s_ctx.initialized = true;
  return ESY_OK;
}

void esy_hb_flash_deinit(void) {
  s_ctx.initialized = false;
}

esy_err_t esy_hb_flash_set_safe_execute(
    esy_hb_flash_safe_execute_t safe_execute) {
  return (safe_execute == NULL) ? ESY_OK : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_flash_read(uint32_t address, void *buffer, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  if (buffer == NULL) return ESY_ERR_INVALID_ARG;
  esy_flash_geometry_t geometry;
  esy_err_t ret = esy_hb_flash_get_geometry(&geometry);
  if (ret != ESY_OK) return ret;
  if (!flash_range_is_valid(&geometry, address, length)) {
    return ESY_ERR_INVALID_ARG;
  }
  return esp_flash_read(NULL, buffer, address, (uint32_t)length) == ESP_OK
             ? ESY_OK : ESY_ERR_IO;
}

esy_err_t esy_hb_flash_erase(uint32_t address, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  esy_flash_geometry_t geometry;
  uint32_t sector_size;
  esy_err_t ret = query_geometry(&geometry, &sector_size);
  if (ret != ESY_OK) return ret;
  if (!flash_range_is_valid(&geometry, address, length) ||
      (address % sector_size) != 0u || (length % sector_size) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  return esp_flash_erase_region(NULL, address, (uint32_t)length) == ESP_OK
             ? ESY_OK : ESY_ERR_IO;
}

esy_err_t esy_hb_flash_write(uint32_t address, const void *data, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  if (data == NULL) return ESY_ERR_INVALID_ARG;
  esy_flash_geometry_t geometry;
  esy_err_t ret = esy_hb_flash_get_geometry(&geometry);
  if (ret != ESY_OK) return ret;
  if (!flash_range_is_valid(&geometry, address, length)) {
    return ESY_ERR_INVALID_ARG;
  }
  return esp_flash_write(NULL, data, address, (uint32_t)length) == ESP_OK
             ? ESY_OK : ESY_ERR_IO;
}

bool esy_hb_flash_is_valid_address(uint32_t address) {
  esy_flash_geometry_t geometry;
  return esy_hb_flash_get_geometry(&geometry) == ESY_OK &&
         flash_range_is_valid(&geometry, address, 1u);
}

esy_err_t esy_hb_flash_get_geometry(esy_flash_geometry_t *geometry) {
  if (geometry == NULL) return ESY_ERR_INVALID_ARG;
  esy_flash_geometry_t result;
  uint32_t sector_size;
  esy_err_t ret = query_geometry(&result, &sector_size);
  if (ret != ESY_OK) return ret;
  *geometry = result;
  return ESY_OK;
}

esy_err_t esy_hb_flash_get_sector(uint32_t address, esy_flash_sector_t *sector) {
  if (sector == NULL) return ESY_ERR_INVALID_ARG;
  esy_flash_geometry_t geometry;
  uint32_t sector_size;
  esy_err_t ret = query_geometry(&geometry, &sector_size);
  if (ret != ESY_OK) return ret;
  if (!flash_range_is_valid(&geometry, address, 1u)) return ESY_ERR_INVALID_ARG;
  const esy_flash_sector_t result = {
      .start_address = (address / sector_size) * sector_size,
      .size_bytes = sector_size,
  };
  *sector = result;
  return ESY_OK;
}
