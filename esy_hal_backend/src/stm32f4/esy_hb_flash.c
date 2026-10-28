/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_flash.h"

#include <string.h>

#include "stm32f4xx_hal.h"

#if !defined(STM32F405xx) && !defined(STM32F407xx) && \
    !defined(STM32F415xx) && !defined(STM32F417xx)
#error "STM32 Flash supports only audited STM32F405/407/415/417 single-bank layouts"
#endif

#define FLASH_PROGRAM_SIZE    4u
#define FLASH_KIB_BYTES       1024u
#define FLASH_DENSITY_512_KIB 512u
#define FLASH_DENSITY_1M_KIB  1024u

typedef struct {
  uint32_t start;
  uint32_t end;
  uint32_t sector;
} flash_sector_info_t;

static const flash_sector_info_t k_sector_table[] = {
    {0x08000000UL, 0x08004000UL, FLASH_SECTOR_0},
    {0x08004000UL, 0x08008000UL, FLASH_SECTOR_1},
    {0x08008000UL, 0x0800C000UL, FLASH_SECTOR_2},
    {0x0800C000UL, 0x08010000UL, FLASH_SECTOR_3},
    {0x08010000UL, 0x08020000UL, FLASH_SECTOR_4},
    {0x08020000UL, 0x08040000UL, FLASH_SECTOR_5},
    {0x08040000UL, 0x08060000UL, FLASH_SECTOR_6},
    {0x08060000UL, 0x08080000UL, FLASH_SECTOR_7},
    {0x08080000UL, 0x080A0000UL, FLASH_SECTOR_8},
    {0x080A0000UL, 0x080C0000UL, FLASH_SECTOR_9},
    {0x080C0000UL, 0x080E0000UL, FLASH_SECTOR_10},
    {0x080E0000UL, 0x08100000UL, FLASH_SECTOR_11},
};

typedef struct {
  bool initialized;
} esy_hb_flash_ctx_t;

static esy_hb_flash_ctx_t s_ctx;

static bool flash_range_is_valid(uint32_t address, size_t length) {
  esy_flash_geometry_t geometry;
  if (length == 0u || esy_hb_flash_get_geometry(&geometry) != ESY_OK ||
      address < geometry.base_address) {
    return false;
  }
  uint32_t offset = address - geometry.base_address;
  return offset < geometry.size_bytes &&
         length <= (size_t)(geometry.size_bytes - offset);
}

static bool unlock_and_clear_errors(void) {
  if (HAL_FLASH_Unlock() != HAL_OK) return false;

  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR |
                         FLASH_FLAG_PGSERR);
  return true;
}

static esy_err_t finish_mutation(HAL_StatusTypeDef status) {
  HAL_StatusTypeDef lock_status = HAL_FLASH_Lock();
  if (status == HAL_TIMEOUT) return ESY_ERR_TIMEOUT;
  if (status == HAL_BUSY) return ESY_ERR_BUSY;
  if (status != HAL_OK || lock_status != HAL_OK) return ESY_ERR_IO;
  return ESY_OK;
}

esy_err_t esy_hb_flash_init(void) {
  esy_flash_geometry_t geometry;
  esy_err_t ret = esy_hb_flash_get_geometry(&geometry);
  if (ret != ESY_OK) return ret;
  s_ctx.initialized = true;
  return ESY_OK;
}

void esy_hb_flash_deinit(void) {
  s_ctx.initialized = false;
  HAL_FLASH_Lock();
}

esy_err_t esy_hb_flash_set_safe_execute(
    esy_hb_flash_safe_execute_t safe_execute) {
  return (safe_execute == NULL) ? ESY_OK : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_flash_read(uint32_t address, void *buffer, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  if (!buffer || !flash_range_is_valid(address, length)) {
    return ESY_ERR_INVALID_ARG;
  }

  memcpy(buffer, (const void *)address, length);
  return ESY_OK;
}

esy_err_t esy_hb_flash_write(uint32_t address, const void *data, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  if (!data || !flash_range_is_valid(address, length) ||
      ((address - FLASH_BASE) % FLASH_PROGRAM_SIZE) != 0u ||
      (length % FLASH_PROGRAM_SIZE) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  if (!unlock_and_clear_errors()) return ESY_ERR_IO;

  const uint8_t *src = (const uint8_t *)data;
  for (size_t offset = 0u; offset < length; offset += sizeof(uint32_t)) {
    uint32_t word;
    memcpy(&word, &src[offset], sizeof(word));
    HAL_StatusTypeDef status = HAL_FLASH_Program(
        FLASH_TYPEPROGRAM_WORD, address + (uint32_t)offset, word);
    if (status != HAL_OK) return finish_mutation(status);
  }
  return finish_mutation(HAL_OK);
}

static const flash_sector_info_t *get_sector_info(uint32_t address) {
  for (size_t i = 0; i < sizeof(k_sector_table) / sizeof(k_sector_table[0]); ++i) {
    if (address >= k_sector_table[i].start && address < k_sector_table[i].end) {
      return &k_sector_table[i];
    }
  }

  return NULL;
}

esy_err_t esy_hb_flash_erase(uint32_t address, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  if (!flash_range_is_valid(address, length)) {
    return ESY_ERR_INVALID_ARG;
  }

  const flash_sector_info_t *first_sector = get_sector_info(address);
  if (!first_sector || first_sector->start != address) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Validate the complete destructive range before unlocking the controller. */
  uint32_t remaining = (uint32_t)length;
  uint32_t count = 0u;
  for (uint32_t addr = address; remaining != 0u;) {
    const flash_sector_info_t *sector_info = get_sector_info(addr);
    if (sector_info == NULL || sector_info->start != addr ||
        (sector_info->end - sector_info->start) > remaining) {
      return ESY_ERR_INVALID_ARG;
    }
    remaining -= sector_info->end - sector_info->start;
    addr = sector_info->end;
    count++;
  }

  if (!unlock_and_clear_errors()) return ESY_ERR_IO;
  FLASH_EraseInitTypeDef erase = {
      .TypeErase = FLASH_TYPEERASE_SECTORS,
      .VoltageRange = FLASH_VOLTAGE_RANGE_3,
      .Sector = first_sector->sector,
      .NbSectors = count,
  };
  uint32_t sector_error = UINT32_MAX;
  return finish_mutation(HAL_FLASHEx_Erase(&erase, &sector_error));
}

bool esy_hb_flash_is_valid_address(uint32_t address) {
  return flash_range_is_valid(address, 1u);
}

esy_err_t esy_hb_flash_get_geometry(esy_flash_geometry_t *geometry) {
  if (geometry == NULL) return ESY_ERR_INVALID_ARG;
  /* RM0090 density is a factory-programmed 16-bit KiB value, not FLASH_END. */
  const uint32_t size_kib =
      *(const volatile uint16_t *)(uintptr_t)FLASHSIZE_BASE;
  if (size_kib != FLASH_DENSITY_512_KIB && size_kib != FLASH_DENSITY_1M_KIB) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  const esy_flash_geometry_t result = {
      .base_address = FLASH_BASE,
      .size_bytes = size_kib * FLASH_KIB_BYTES,
      .program_size = FLASH_PROGRAM_SIZE,
  };
  *geometry = result;
  return ESY_OK;
}

esy_err_t esy_hb_flash_get_sector(uint32_t address, esy_flash_sector_t *sector) {
  if (sector == NULL) return ESY_ERR_INVALID_ARG;
  esy_flash_geometry_t geometry;
  esy_err_t ret = esy_hb_flash_get_geometry(&geometry);
  if (ret != ESY_OK) return ret;
  if (address < geometry.base_address ||
      (address - geometry.base_address) >= geometry.size_bytes) {
    return ESY_ERR_INVALID_ARG;
  }
  const flash_sector_info_t *sector_info = get_sector_info(address);
  if (sector_info == NULL ||
      (sector_info->end - geometry.base_address) > geometry.size_bytes) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  const esy_flash_sector_t result = {
      .start_address = sector_info->start,
      .size_bytes = sector_info->end - sector_info->start,
  };
  *sector = result;
  return ESY_OK;
}
