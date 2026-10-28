/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_flash.h"

#include <stdbool.h>
#include <string.h>

static fake_esy_hal_flash_state_t s_ctx;

void fake_esy_hal_flash_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  memset(s_ctx.storage, 0xFF, sizeof(s_ctx.storage));
  s_ctx.sector_size = 16u;
  s_ctx.storage_base_addr = 0x1000u;
  s_ctx.size_bytes = FAKE_ESY_HAL_FLASH_STORAGE_BYTES;
  s_ctx.program_size = 1u;
  s_ctx.init_result = ESY_OK;
  s_ctx.read_result = ESY_OK;
  s_ctx.write_result = ESY_OK;
  s_ctx.erase_result = ESY_OK;
  s_ctx.erase_transient_result = ESY_ERR_IO;
  s_ctx.injected_error = ESY_ERR_IO;
  s_ctx.valid_address = true;
}

fake_esy_hal_flash_state_t *fake_esy_hal_flash_get_state(void) {
  return &s_ctx;
}

esy_err_t esy_hal_flash_init(void) {
  s_ctx.init_count++;
  return s_ctx.init_result;
}

void esy_hal_flash_deinit(void) { s_ctx.deinit_count++; }

static esy_err_t flash_storage_range(uint32_t address, size_t length,
                                     size_t *out_offset) {
  esy_flash_geometry_t geometry;
  esy_err_t result = esy_hal_flash_get_geometry(&geometry);
  if (result != ESY_OK) return result;
  if (address < geometry.base_address ||
      address - geometry.base_address >= geometry.size_bytes ||
      length > geometry.size_bytes - (address - geometry.base_address)) {
    return ESY_ERR_INVALID_ARG;
  }
  /* Geometry describes physical addresses; backing offsets use their own
   * configured origin. Validate both complete ranges before accessing bytes. */
  if (address < s_ctx.storage_base_addr) return ESY_ERR_INVALID_ARG;
  size_t offset = address - s_ctx.storage_base_addr;
  size_t capacity = s_ctx.backing_storage ? s_ctx.backing_storage_size : sizeof(s_ctx.storage);
  if (offset > capacity || length > capacity - offset) return ESY_ERR_INVALID_ARG;
  *out_offset = offset;
  return ESY_OK;
}

static esy_err_t flash_programmed_range(uint32_t address, size_t length,
                                        size_t *out_first, size_t *out_count) {
  uint64_t origin = s_ctx.storage_base_addr / s_ctx.program_size;
  uint64_t first = address / s_ctx.program_size - origin;
  uint64_t count = ((uint64_t)address + length - 1u) / s_ctx.program_size -
                   origin - first + 1u;
  if (s_ctx.programmed_units &&
      (first >= s_ctx.programmed_units_size ||
       count > s_ctx.programmed_units_size - (size_t)first)) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_first = (size_t)first;
  *out_count = (size_t)count;
  return ESY_OK;
}

static esy_err_t flash_injected_error(void) {
  return s_ctx.injected_error == ESY_OK ? ESY_ERR_IO : s_ctx.injected_error;
}

esy_err_t esy_hal_flash_read(uint32_t address,
                                  void *buffer,
                                  size_t length) {
  s_ctx.read_count++;
  s_ctx.last_read_addr = address;
  s_ctx.last_read_len = length;
  if (s_ctx.read_result != ESY_OK) {
    return s_ctx.read_result;
  }
  if (!buffer) {
    return ESY_ERR_INVALID_ARG;
  }
  size_t offset;
  esy_err_t result = flash_storage_range(address, length, &offset);
  if (result != ESY_OK) return result;
  uint8_t *storage = s_ctx.backing_storage ? s_ctx.backing_storage : s_ctx.storage;
  bool fail = s_ctx.fail_read_call != 0u && s_ctx.read_count == s_ctx.fail_read_call;
  size_t effected = fail && s_ctx.read_fail_bytes < length ? s_ctx.read_fail_bytes : length;
  memcpy(buffer, &storage[offset], effected);
  return fail ? flash_injected_error() : ESY_OK;
}

esy_err_t esy_hal_flash_write(uint32_t address,
                                   const void *data,
                                   size_t length) {
  s_ctx.write_count++;
  s_ctx.last_write_addr = address;
  s_ctx.last_write_len = length;
  if (s_ctx.write_result != ESY_OK) {
    return s_ctx.write_result;
  }
  if (!data || length == 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (s_ctx.program_size == 0u) return ESY_ERR_NOT_SUPPORTED;
  if (address % s_ctx.program_size != 0u ||
      length % s_ctx.program_size != 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  size_t offset;
  esy_err_t result = flash_storage_range(address, length, &offset);
  if (result != ESY_OK) return result;
  size_t first_unit, unit_count;
  result = flash_programmed_range(address, length, &first_unit, &unit_count);
  if (result != ESY_OK) return result;
  if (s_ctx.programmed_units) {
    for (size_t i = 0u; i < unit_count; ++i)
      if (s_ctx.programmed_units[first_unit + i] != 0u) return ESY_ERR_IO;
  }
  uint8_t *storage = s_ctx.backing_storage ? s_ctx.backing_storage : s_ctx.storage;
  size_t copy_len = length;
  if (copy_len > sizeof(s_ctx.last_write_data)) copy_len = sizeof(s_ctx.last_write_data);
  memcpy(s_ctx.last_write_data, data, copy_len);
  const uint8_t *source = data;
  /* Validate the complete operation before changing bytes. This deterministic
   * NOR model rejects any attempt to restore a programmed zero bit to one. */
  for (size_t i = 0u; i < length; ++i)
    if ((source[i] & storage[offset + i]) != source[i]) return ESY_ERR_IO;
  bool fail = s_ctx.fail_write_call != 0u && s_ctx.write_count == s_ctx.fail_write_call;
  size_t effected = fail && s_ctx.write_fail_bytes < length ? s_ctx.write_fail_bytes : length;
  memcpy(&storage[offset], data, effected);
  if (s_ctx.programmed_units && effected != 0u) {
    size_t touched = (effected - 1u) / s_ctx.program_size + 1u;
    memset(&s_ctx.programmed_units[first_unit], 1, touched);
  }
  return fail ? flash_injected_error() : ESY_OK;
}

esy_err_t esy_hal_flash_erase(uint32_t address, size_t length) {
  s_ctx.erase_count++;
  s_ctx.last_erase_addr = address;
  s_ctx.last_erase_len = length;
  if (s_ctx.erase_failures_remaining != 0u) {
    --s_ctx.erase_failures_remaining;
    return s_ctx.erase_transient_result;
  }
  if (s_ctx.erase_result != ESY_OK) return s_ctx.erase_result;
  if (length == 0u || length > (uint64_t)UINT32_MAX + 1u - address) {
    return ESY_ERR_INVALID_ARG;
  }
  size_t offset;
  esy_err_t range_result = flash_storage_range(address, length, &offset);
  if (range_result != ESY_OK) return range_result;
  size_t first_unit, unit_count;
  range_result = flash_programmed_range(address, length, &first_unit, &unit_count);
  if (range_result != ESY_OK) return range_result;
  uint64_t cursor = address;
  uint64_t end = cursor + length;
  while (cursor < end) {
    esy_flash_sector_t sector;
    esy_err_t result = esy_hal_flash_get_sector((uint32_t)cursor, &sector);
    if (result != ESY_OK) return result;
    if (sector.start_address != cursor || sector.size_bytes > end - cursor) {
      return ESY_ERR_INVALID_ARG;
    }
    cursor += sector.size_bytes;
  }
  uint8_t *storage = s_ctx.backing_storage ? s_ctx.backing_storage : s_ctx.storage;
  bool fail = s_ctx.fail_erase_call != 0u && s_ctx.erase_count == s_ctx.fail_erase_call;
  size_t effected = fail && s_ctx.erase_fail_bytes < length ? s_ctx.erase_fail_bytes : length;
  memset(&storage[offset], 0xFF, effected);
  if (s_ctx.programmed_units && effected != 0u) {
    uint64_t origin = s_ctx.storage_base_addr / s_ctx.program_size;
    uint64_t first_complete = ((uint64_t)address + s_ctx.program_size - 1u) /
                              s_ctx.program_size;
    uint64_t end_complete = ((uint64_t)address + effected) / s_ctx.program_size;
    if (end_complete > first_complete) {
      memset(&s_ctx.programmed_units[(size_t)(first_complete - origin)], 0,
             (size_t)(end_complete - first_complete));
    }
  }
  return fail ? flash_injected_error() : ESY_OK;
}

esy_err_t esy_hal_flash_get_geometry(esy_flash_geometry_t *geometry) {
  s_ctx.geometry_count++;
  if (!geometry) return ESY_ERR_INVALID_ARG;
  if (s_ctx.geometry_result != ESY_OK) return s_ctx.geometry_result;
  if (s_ctx.size_bytes == 0u || s_ctx.program_size == 0u ||
      (uint64_t)s_ctx.storage_base_addr + s_ctx.size_bytes >
          (uint64_t)UINT32_MAX + 1u) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  *geometry = (esy_flash_geometry_t){
      .base_address = s_ctx.storage_base_addr,
      .size_bytes = s_ctx.size_bytes,
      .program_size = s_ctx.program_size};
  return ESY_OK;
}

bool esy_hal_flash_is_valid_address(uint32_t address) {
  esy_flash_geometry_t geometry;
  s_ctx.last_valid_address = address;
  return s_ctx.valid_address &&
         esy_hal_flash_get_geometry(&geometry) == ESY_OK &&
         address >= geometry.base_address &&
         address - geometry.base_address < geometry.size_bytes;
}

esy_err_t esy_hal_flash_get_sector(uint32_t address,
                                   esy_flash_sector_t *sector) {
  s_ctx.sector_count++;
  s_ctx.last_sector_address = address;
  if (!sector) return ESY_ERR_INVALID_ARG;
  if (s_ctx.sector_result != ESY_OK) return s_ctx.sector_result;
  esy_flash_geometry_t geometry;
  esy_err_t result = esy_hal_flash_get_geometry(&geometry);
  if (result != ESY_OK) return result;
  if (address < geometry.base_address ||
      address - geometry.base_address >= geometry.size_bytes) {
    return ESY_ERR_INVALID_ARG;
  }
  uint32_t base = geometry.base_address;
  uint32_t size = s_ctx.sector_size;
  uint64_t region_end = (uint64_t)base + geometry.size_bytes;
  if (s_ctx.sector_size_after != 0u) {
    uint32_t transition = s_ctx.sector_size_after_address;
    if (size == 0u || transition < base || transition >= region_end ||
        (transition - base) % size != 0u) {
      return ESY_ERR_NOT_SUPPORTED;
    }
    if (address >= transition) {
      base = transition;
      size = s_ctx.sector_size_after;
    } else {
      region_end = transition;
    }
  }
  if (size == 0u) return ESY_ERR_NOT_SUPPORTED;
  uint32_t start = address - (address - base) % size;
  if ((uint64_t)start + size > region_end) return ESY_ERR_NOT_SUPPORTED;
  *sector = (esy_flash_sector_t){.start_address = start, .size_bytes = size};
  return ESY_OK;
}

esy_err_t esy_hal_flash_set_safe_execute(
    esy_hal_flash_safe_execute_t safe_execute) {
  s_ctx.safe_execute_set_count++;
  if (s_ctx.safe_execute_result != ESY_OK) return s_ctx.safe_execute_result;
  s_ctx.safe_execute = safe_execute;
  return ESY_OK;
}
