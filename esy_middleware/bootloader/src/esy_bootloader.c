/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/bootloader/esy_bootloader.h"

#include <string.h>

#include "esy_hal/esy_hal_boot.h"
#include "esy_common/utils/integrity/esy_crc32.h"
#include "esy_common/utils/base/esy_endian.h"
#include "esy_common/utils/integrity/esy_sha256.h"
#include "esy_hal/esy_hal_flash.h"

#define ESY_BOOT_SLOT_COUNT             ((int)ESY_BOOTLOADER_SLOT_COUNT)
#define ESY_BOOT_META_WRITE_MAX_RETRIES 3u
#define ESY_BOOT_VERIFY_CHUNK_BYTES     256u

typedef struct {
  int8_t selected_slot;
} esy_bootloader_ctx_t;

static esy_bootloader_ctx_t s_ctx = {.selected_slot = -1};

_Static_assert(sizeof(esy_firmware_header_t) == ESY_FIRMWARE_METADATA_SIZE,
               "firmware metadata size mismatch");

/* All regions must own complete sectors; no erase may touch a neighbour. */
static bool region_has_complete_sectors(uint32_t address, uint32_t size) {
  const uint64_t end = (uint64_t)address + size;
  esy_flash_sector_t first;
  esy_flash_sector_t last;
  /* HAL geometry partitions a contiguous range, so only its ends need checks. */
  return esy_hal_flash_get_sector(address, &first) == ESY_OK &&
         first.start_address == address && first.size_bytes > 0u &&
         (uint64_t)first.start_address + first.size_bytes <= end &&
         esy_hal_flash_get_sector((uint32_t)(end - 1u), &last) == ESY_OK &&
         last.start_address >= address && last.size_bytes > 0u &&
         (uint64_t)last.start_address + last.size_bytes == end;
}

static bool regions_overlap(uint32_t a, uint32_t a_size,
                            uint32_t b, uint32_t b_size) {
  return (uint64_t)a < (uint64_t)b + b_size &&
         (uint64_t)b < (uint64_t)a + a_size;
}

static bool bootloader_get_layout(esy_bootloader_layout_t *layout) {
  esy_flash_geometry_t geometry;
  if (!layout || esy_hal_boot_get_layout(layout) != ESY_OK ||
      esy_hal_flash_get_geometry(&geometry) != ESY_OK) return false;

  const uint64_t flash_end =
      (uint64_t)geometry.base_address + geometry.size_bytes;
  if (geometry.size_bytes == 0u || geometry.program_size == 0u ||
      flash_end > (uint64_t)UINT32_MAX + 1u || layout->slot_size == 0u ||
      ESY_FIRMWARE_METADATA_SIZE % geometry.program_size != 0u ||
      layout->slot_size % geometry.program_size != 0u) return false;

  for (uint32_t i = 0u; i < ESY_BOOTLOADER_SLOT_COUNT; ++i) {
    if (layout->meta_size[i] < ESY_FIRMWARE_METADATA_SIZE ||
        layout->meta_size[i] % geometry.program_size != 0u) return false;
    const uint32_t addresses[] = {layout->slot_addr[i], layout->meta_addr[i]};
    const uint32_t sizes[] = {layout->slot_size, layout->meta_size[i]};
    for (uint32_t region = 0u; region < 2u; ++region) {
      if (addresses[region] < geometry.base_address ||
          (uint64_t)addresses[region] + sizes[region] > flash_end ||
          addresses[region] % geometry.program_size != 0u ||
          !region_has_complete_sectors(addresses[region], sizes[region])) {
        return false;
      }
    }
    for (uint32_t j = 0u; j < ESY_BOOTLOADER_SLOT_COUNT; ++j) {
      if (regions_overlap(layout->slot_addr[i], layout->slot_size,
                          layout->meta_addr[j], layout->meta_size[j])) return false;
      if (i != j &&
          (regions_overlap(layout->slot_addr[i], layout->slot_size,
                           layout->slot_addr[j], layout->slot_size) ||
           regions_overlap(layout->meta_addr[i], layout->meta_size[i],
                           layout->meta_addr[j], layout->meta_size[j]))) return false;
    }
  }
  return true;
}

static bool is_valid_state(uint8_t state) {
  return state == ESY_FIRMWARE_STATE_TRY_BOOT ||
         state == ESY_FIRMWARE_STATE_UNCONFIRMED ||
         state == ESY_FIRMWARE_STATE_CONFIRMED;
}

static int running_slot_for(const esy_bootloader_layout_t *layout,
                            const esy_boot_runtime_t *runtime) {
  for (int slot = 0; slot < ESY_BOOT_SLOT_COUNT; ++slot) {
    if (runtime->running_address >= layout->slot_addr[slot] &&
        runtime->running_address <
            (uint64_t)layout->slot_addr[slot] + layout->slot_size) {
      return slot;
    }
  }
  return -1;
}

static uint32_t meta_calc_crc(const esy_firmware_header_t *header) {
  esy_firmware_header_t copy = *header;
  copy.metadata_crc32 = 0u;
  return ~esy_crc32(0xFFFFFFFFu, &copy, sizeof(copy));
}

static bool meta_validate(const esy_firmware_header_t *header) {
  esy_bootloader_layout_t layout = {0};

  if (!header) return false;
  if (!bootloader_get_layout(&layout)) return false;
  if (header->magic != ESY_FIRMWARE_METADATA_MAGIC ||
      header->format_version != ESY_FIRMWARE_METADATA_FORMAT_VERSION ||
      header->reserved0 != 0u) {
    return false;
  }
  if (!is_valid_state(header->state)) return false;
  if (header->firmware_size == 0u || header->firmware_size > layout.slot_size) {
    return false;
  }
  return header->metadata_crc32 == meta_calc_crc(header);
}

static void meta_pack(esy_firmware_header_t *header) {
  if (!header) return;
  header->magic = ESY_FIRMWARE_METADATA_MAGIC;
  header->format_version = ESY_FIRMWARE_METADATA_FORMAT_VERSION;
  header->reserved0 = 0u;
  header->metadata_crc32 = 0u;
  header->metadata_crc32 = meta_calc_crc(header);
}

static esy_err_t calculate_slot_sha256(
    uint32_t slot_addr, uint32_t image_size,
    uint8_t digest[ESY_FIRMWARE_SHA256_BYTES]) {
  esy_sha256_ctx_t sha256;
  uint8_t chunk[ESY_BOOT_VERIFY_CHUNK_BYTES];
  uint32_t offset = 0u;

  if (image_size == 0u || digest == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_sha256_init(&sha256);
  while (offset < image_size) {
    uint32_t remaining = image_size - offset;
    size_t length =
        remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
    esy_err_t ret = esy_hal_flash_read(slot_addr + offset, chunk, length);
    if (ret != ESY_OK) {
      return ret;
    }
    esy_sha256_update(&sha256, chunk, length);
    offset += (uint32_t)length;
  }
  esy_sha256_final(&sha256, digest);
  return ESY_OK;
}

static bool verify_slot_image(const esy_bootloader_config_t *config,
                              uint32_t slot_addr,
                              const esy_firmware_header_t *header) {
  uint8_t image_digest[ESY_FIRMWARE_SHA256_BYTES];
  uint8_t signed_message[ESY_FIRMWARE_SIGNED_MESSAGE_BYTES];
  uint8_t signed_digest[ESY_FIRMWARE_SHA256_BYTES];

  if (header == NULL ||
      calculate_slot_sha256(slot_addr, header->firmware_size, image_digest) !=
          ESY_OK ||
      memcmp(image_digest, header->image_sha256, sizeof(image_digest)) != 0) {
    return false;
  }

  esy_write_u32_le(&signed_message[0], header->version);
  esy_write_u32_le(&signed_message[4], header->firmware_size);
  memcpy(&signed_message[8], header->image_sha256, ESY_FIRMWARE_SHA256_BYTES);
  esy_sha256(signed_message, sizeof(signed_message), signed_digest);

  return config->verify_signature(config->verify_context, signed_digest,
                                  header->signature);
}

static int pick_higher_version_slot(int a, int b,
                                    const esy_firmware_header_t headers[]) {
  if (headers[a].version >= headers[b].version) {
    return a;
  }
  return b;
}

static bool is_try_boot_slot_valid(int slot, const bool valid[],
                                   const esy_firmware_header_t headers[]) {
  return slot >= 0 && slot < ESY_BOOT_SLOT_COUNT && valid[slot] &&
         headers[slot].state == ESY_FIRMWARE_STATE_TRY_BOOT;
}

static esy_err_t promote_try_boot_to_unconfirmed(
    int slot, esy_firmware_header_t *header) {
  if (slot < 0 || slot >= ESY_BOOT_SLOT_COUNT || !header) {
    return ESY_ERR_INVALID_ARG;
  }

  header->state = ESY_FIRMWARE_STATE_UNCONFIRMED;
  for (uint32_t i = 0; i < ESY_BOOT_META_WRITE_MAX_RETRIES; ++i) {
    esy_err_t ret = esy_bootloader_write_meta_data(slot, header);
    if (ret == ESY_OK) {
      return ESY_OK;
    }
  }
  return ESY_ERR_FAIL;
}

esy_err_t esy_bootloader_read_meta_data(
    int slot, esy_firmware_header_t *header) {
  if (slot < 0 || slot >= ESY_BOOT_SLOT_COUNT || !header) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_bootloader_layout_t layout = {0};
  if (!bootloader_get_layout(&layout)) {
    return ESY_ERR_FAIL;
  }

  esy_err_t ret =
      esy_hal_flash_read(layout.meta_addr[slot], header, sizeof(*header));
  if (ret != ESY_OK) {
    return ret;
  }

  return meta_validate(header) ? ESY_OK : ESY_ERR_CRC_FAIL;
}

esy_err_t esy_bootloader_write_meta_data(
    int slot, const esy_firmware_header_t *header) {
  if (slot < 0 || slot >= ESY_BOOT_SLOT_COUNT || !header) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_bootloader_layout_t layout = {0};
  if (!bootloader_get_layout(&layout)) {
    return ESY_ERR_FAIL;
  }

  esy_firmware_header_t write_header = *header;
  if (!is_valid_state(write_header.state) || write_header.firmware_size == 0u ||
      write_header.firmware_size > layout.slot_size) {
    return ESY_ERR_INVALID_ARG;
  }

  meta_pack(&write_header);

  esy_err_t ret = esy_hal_flash_erase(layout.meta_addr[slot], layout.meta_size[slot]);
  if (ret != ESY_OK) return ret;

  ret = esy_hal_flash_write(layout.meta_addr[slot], &write_header,
                               ESY_FIRMWARE_METADATA_SIZE);
  if (ret != ESY_OK) return ret;

  /* Verify after write. */
  esy_firmware_header_t verify_header = {0};
  ret = esy_hal_flash_read(layout.meta_addr[slot], &verify_header,
                              sizeof(verify_header));
  if (ret != ESY_OK) return ret;

  if (memcmp(&verify_header, &write_header, sizeof(verify_header)) != 0) {
    return ESY_ERR_CRC_FAIL; /* Verification failed. */
  }

  return meta_validate(&verify_header) ? ESY_OK : ESY_ERR_CRC_FAIL;
}

esy_err_t esy_bootloader_init(
    const esy_bootloader_config_t *config) {
  esy_bootloader_layout_t layout = {0};
  esy_firmware_header_t header[ESY_BOOT_SLOT_COUNT] = {0};
  bool valid[ESY_BOOT_SLOT_COUNT] = {false, false};

  s_ctx.selected_slot = -1;
  if (config == NULL || config->verify_signature == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!bootloader_get_layout(&layout)) {
    return ESY_ERR_FAIL;
  }

  esy_boot_runtime_t runtime;
  esy_err_t runtime_result = esy_hal_boot_get_runtime(&runtime);
  if (runtime_result != ESY_OK) return runtime_result;
  if (runtime.mode == ESY_BOOT_MODE_MANAGED) {
    const int running = running_slot_for(&layout, &runtime);
    if (running < 0 ||
        esy_bootloader_read_meta_data(running, &header[running]) != ESY_OK ||
        !verify_slot_image(config, layout.slot_addr[running], &header[running])) {
      return ESY_ERR_FAIL;
    }
    s_ctx.selected_slot = (int8_t)running;
    return ESY_OK;
  }
  if (runtime.mode != ESY_BOOT_MODE_DIRECT) return ESY_ERR_NOT_SUPPORTED;

  for (int i = 0; i < ESY_BOOT_SLOT_COUNT; ++i) {
    if (esy_bootloader_read_meta_data(i, &header[i]) == ESY_OK &&
        verify_slot_image(config, layout.slot_addr[i], &header[i])) {
      valid[i] = true;
    }
  }

  for (int attempt = 0; attempt < ESY_BOOT_SLOT_COUNT; ++attempt) {
    int try_slot = -1;
    if (is_try_boot_slot_valid(0, valid, header) &&
        is_try_boot_slot_valid(1, valid, header)) {
      try_slot = pick_higher_version_slot(0, 1, header);
    } else if (is_try_boot_slot_valid(0, valid, header)) {
      try_slot = 0;
    } else if (is_try_boot_slot_valid(1, valid, header)) {
      try_slot = 1;
    }
    if (try_slot < 0) break;

    esy_firmware_header_t promoted = header[try_slot];
    if (promote_try_boot_to_unconfirmed(try_slot, &promoted) == ESY_OK) {
      s_ctx.selected_slot = (int8_t)try_slot;
      return ESY_OK;
    }
    /* A fallback trial must persist its own attempt state before selection. */
    valid[try_slot] = false;
  }

  if (valid[0] && valid[1]) {
    const bool slot0_confirmed =
        header[0].state == ESY_FIRMWARE_STATE_CONFIRMED;
    const bool slot1_confirmed =
        header[1].state == ESY_FIRMWARE_STATE_CONFIRMED;
    if (slot0_confirmed && !slot1_confirmed) {
      s_ctx.selected_slot = 0;
    } else if (slot1_confirmed && !slot0_confirmed) {
      s_ctx.selected_slot = 1;
    } else {
      const int selected = pick_higher_version_slot(0, 1, header);
      s_ctx.selected_slot = (int8_t)selected;
    }
  } else if (valid[0]) {
    s_ctx.selected_slot = 0;
  } else if (valid[1]) {
    s_ctx.selected_slot = 1;
  }

  if (s_ctx.selected_slot < 0) {
    return ESY_ERR_FAIL;
  }

  return ESY_OK;
}

bool esy_bootloader_jump_to_app(void) {
  if (s_ctx.selected_slot < 0) return false;
  esy_bootloader_layout_t layout = {0};
  if (!bootloader_get_layout(&layout)) return false;

  esy_hal_boot_jump_to_app(layout.slot_addr[s_ctx.selected_slot]);
  return false; /* Any return means the handoff failed. */
}

int esy_bootloader_get_selected_slot(void) {
  return s_ctx.selected_slot;
}

int esy_bootloader_get_running_slot(void) {
  esy_bootloader_layout_t layout = {0};
  if (!bootloader_get_layout(&layout)) return -1;
  esy_boot_runtime_t runtime;
  if (esy_hal_boot_get_runtime(&runtime) != ESY_OK) return -1;
  return running_slot_for(&layout, &runtime);
}

int esy_bootloader_get_flash_slot(void) {
  esy_boot_runtime_t runtime;
  esy_bootloader_layout_t layout;
  if (esy_hal_boot_get_runtime(&runtime) != ESY_OK ||
      !runtime.update_allowed || !bootloader_get_layout(&layout)) return -1;
  int running = running_slot_for(&layout, &runtime);
  if (running < 0) return -1;
  return (running == 0) ? 1 : 0;
}

uint32_t esy_bootloader_get_flash_slot_addr(void) {
  int slot = esy_bootloader_get_flash_slot();
  if (slot < 0) return 0;
  esy_bootloader_layout_t layout = {0};
  if (!bootloader_get_layout(&layout)) return 0u;
  return layout.slot_addr[slot];
}

uint32_t esy_bootloader_get_flash_slot_size(void) {
  if (esy_bootloader_get_flash_slot() < 0) return 0u;

  esy_bootloader_layout_t layout = {0};
  if (!bootloader_get_layout(&layout)) return 0u;
  return layout.slot_size;
}

esy_err_t esy_bootloader_activate_image(int slot) {
  if (slot < 0 || slot >= ESY_BOOT_SLOT_COUNT) return ESY_ERR_INVALID_ARG;
  esy_bootloader_layout_t layout;
  if (!bootloader_get_layout(&layout)) return ESY_ERR_FAIL;
  esy_boot_runtime_t runtime;
  esy_err_t ret = esy_hal_boot_get_runtime(&runtime);
  if (ret != ESY_OK) return ret;
  const int running = running_slot_for(&layout, &runtime);
  if (!runtime.update_allowed || running < 0 || running == slot) {
    return ESY_ERR_STATE;
  }
  if (runtime.mode != ESY_BOOT_MODE_DIRECT &&
      runtime.mode != ESY_BOOT_MODE_MANAGED) return ESY_ERR_NOT_SUPPORTED;
  esy_firmware_header_t header;
  ret = esy_bootloader_read_meta_data(slot, &header);
  if (ret != ESY_OK) return ret;
  if (header.state != ESY_FIRMWARE_STATE_TRY_BOOT) return ESY_ERR_STATE;
  return runtime.mode == ESY_BOOT_MODE_MANAGED
             ? esy_hal_boot_set_pending_image(layout.slot_addr[slot],
                                               header.firmware_size)
             : ESY_OK;
}

esy_err_t esy_bootloader_mark_success(void) {
  esy_boot_runtime_t runtime;
  esy_err_t ret = esy_hal_boot_get_runtime(&runtime);
  if (ret != ESY_OK) return ret;
  if (runtime.mode != ESY_BOOT_MODE_DIRECT &&
      runtime.mode != ESY_BOOT_MODE_MANAGED) return ESY_ERR_NOT_SUPPORTED;
  esy_bootloader_layout_t layout;
  if (!bootloader_get_layout(&layout)) return ESY_ERR_FAIL;
  int slot = running_slot_for(&layout, &runtime);
  if (slot < 0) return ESY_ERR_FAIL;
  esy_firmware_header_t header;
  ret = esy_bootloader_read_meta_data(slot, &header);
  if (ret != ESY_OK) return ret;
  if (header.state != ESY_FIRMWARE_STATE_CONFIRMED) {
    header.state = ESY_FIRMWARE_STATE_CONFIRMED;
    ret = esy_bootloader_write_meta_data(slot, &header);
    if (ret != ESY_OK) return ret;
  }
  return runtime.mode == ESY_BOOT_MODE_MANAGED
             ? esy_hal_boot_confirm_running_image()
             : ESY_OK;
}
