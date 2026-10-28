/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_flash.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must be defined by the selected Pico board"
#endif

#define FLASH_BASE_ADDR        ((uint32_t)XIP_BASE)
#define FLASH_TOTAL_SIZE_BYTES ((uint64_t)PICO_FLASH_SIZE_BYTES)

typedef struct {
  bool initialized;
  esy_hb_flash_safe_execute_t safe_execute;
} esy_hb_flash_ctx_t;

static esy_hb_flash_ctx_t s_ctx;

typedef struct {
  uint32_t offset;
  size_t length;
} flash_erase_op_t;

typedef struct {
  uint32_t offset;
  const uint8_t *data;
  size_t length;
} flash_write_op_t;

static void __not_in_flash_func(flash_erase_op)(void *arg) {
  flash_erase_op_t *op = (flash_erase_op_t *)arg;
  flash_range_erase(op->offset, op->length);
}

static void __not_in_flash_func(flash_write_op)(void *arg) {
  flash_write_op_t *op = (flash_write_op_t *)arg;
  flash_range_program(op->offset, op->data, op->length);
}

static esy_err_t flash_default_safe_execute(
    esy_hb_flash_operation_t operation, void *arg) {
  if (operation == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  uint32_t irq_status = save_and_disable_interrupts();
  operation(arg);
  restore_interrupts(irq_status);
  return ESY_OK;
}

static bool flash_range_is_valid(uint32_t address, size_t length) {
  esy_flash_geometry_t geometry;
  if (length == 0u ||
      esy_hb_flash_get_geometry(&geometry) != ESY_OK ||
      address < geometry.base_address) return false;

  uint32_t offset = address - geometry.base_address;
  if (offset >= geometry.size_bytes) return false;

  return length <= (size_t)(geometry.size_bytes - offset);
}

static bool flash_source_is_valid(const void *data, size_t length) {
  const uintptr_t source = (uintptr_t)data;
  /* XIP is unavailable while programming. Accept only the main on-chip SRAM
   * mapping, including the complete source range, before entering the hook. */
  return source >= (uintptr_t)SRAM_BASE && source < (uintptr_t)SRAM_END &&
         length <= (size_t)((uintptr_t)SRAM_END - source);
}

esy_err_t esy_hb_flash_init(void) {
  esy_flash_geometry_t geometry;
  esy_err_t ret = esy_hb_flash_get_geometry(&geometry);
  if (ret != ESY_OK) return ret;
  if (s_ctx.safe_execute == NULL) {
    s_ctx.safe_execute = flash_default_safe_execute;
  }
  s_ctx.initialized = true;
  return ESY_OK;
}

esy_err_t esy_hb_flash_set_safe_execute(
    esy_hb_flash_safe_execute_t safe_execute) {
  s_ctx.safe_execute = safe_execute ? safe_execute : flash_default_safe_execute;
  return ESY_OK;
}

void esy_hb_flash_deinit(void) {
  s_ctx.initialized = false;
}

esy_err_t esy_hb_flash_read(uint32_t address, void *buffer, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  if (!buffer || !flash_range_is_valid(address, length)) {
    return ESY_ERR_INVALID_ARG;
  }

  const uint8_t *src = (const uint8_t *)address;
  memcpy(buffer, src, length);
  return ESY_OK;
}

esy_err_t esy_hb_flash_erase(uint32_t address, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  if (!flash_range_is_valid(address, length) ||
      ((address - FLASH_BASE_ADDR) % FLASH_SECTOR_SIZE) != 0u ||
      (length % FLASH_SECTOR_SIZE) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  flash_erase_op_t op = {
      .offset = address - FLASH_BASE_ADDR,
      .length = length,
  };
  return s_ctx.safe_execute(flash_erase_op, &op);
}

esy_err_t esy_hb_flash_write(uint32_t address, const void *data, size_t length) {
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  if (!data || !flash_range_is_valid(address, length) ||
      !flash_source_is_valid(data, length) ||
      ((address - FLASH_BASE_ADDR) % FLASH_PAGE_SIZE) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  if ((length % FLASH_PAGE_SIZE) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  flash_write_op_t op = {
      .offset = address - FLASH_BASE_ADDR,
      .data = (const uint8_t *)data,
      .length = length,
  };
  return s_ctx.safe_execute(flash_write_op, &op);
}

bool esy_hb_flash_is_valid_address(uint32_t address) {
  return flash_range_is_valid(address, 1u);
}

esy_err_t esy_hb_flash_get_geometry(esy_flash_geometry_t *geometry) {
  if (geometry == NULL) return ESY_ERR_INVALID_ARG;
  if (FLASH_TOTAL_SIZE_BYTES == 0u || FLASH_TOTAL_SIZE_BYTES > UINT32_MAX ||
      ((uint64_t)FLASH_BASE_ADDR + FLASH_TOTAL_SIZE_BYTES) >
          ((uint64_t)UINT32_MAX + 1u) ||
      (FLASH_TOTAL_SIZE_BYTES % FLASH_SECTOR_SIZE) != 0u) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  const esy_flash_geometry_t result = {
      .base_address = FLASH_BASE_ADDR,
      .size_bytes = (uint32_t)FLASH_TOTAL_SIZE_BYTES,
      .program_size = FLASH_PAGE_SIZE,
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
  const uint32_t offset = address - geometry.base_address;
  const esy_flash_sector_t result = {
      .start_address = geometry.base_address +
                       (offset / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE,
      .size_bytes = FLASH_SECTOR_SIZE,
  };
  *sector = result;
  return ESY_OK;
}
