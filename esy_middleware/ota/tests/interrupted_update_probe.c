/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esy_common/utils/integrity/esy_sha256.h"
#include "esy_middleware/ota/esy_ota_flash.h"
#include "esy_middleware/bootloader/esy_bootloader.h"
#include "fake_esy_hal_boot.h"
#include "fake_esy_hal_flash.h"
#include "fake_esy_hal_system.h"

#define PROBE_FLASH_BASE 0x1000u
#define PROBE_FLASH_BYTES 4096u
#define PROBE_SLOT_A 0x1000u
#define PROBE_SLOT_B 0x1800u
#define PROBE_META_A 0x1700u
#define PROBE_META_B 0x1F00u
#define PROBE_SLOT_BYTES 0x700u
#define PROBE_SECTOR_BYTES 16u
#define PROBE_IMAGE_BYTES 32u

#define REQUIRE(expression)                                                    \
  do {                                                                         \
    if (!(expression)) {                                                       \
      fprintf(stderr, "Requirement failed at line %d: %s\n", __LINE__,          \
              #expression);                                                    \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef struct {
  uint8_t flash[PROBE_FLASH_BYTES];
} interrupted_update_probe_ctx_t;

static interrupted_update_probe_ctx_t s_ctx;

/* Signature cryptography belongs to the provider; this probe exercises hashing
 * and selection with authenticated metadata accepted by its test verifier. */
static bool verify_signature(void *context, const uint8_t *digest,
                             const uint8_t *signature) {
  (void)context;
  (void)digest;
  (void)signature;
  return true;
}

int main(void) {
  fake_esy_hal_boot_reset();
  fake_esy_hal_flash_reset();
  fake_esy_hal_system_reset();
  fake_esy_hal_flash_state_t *flash = fake_esy_hal_flash_get_state();
  flash->storage_base_addr = PROBE_FLASH_BASE;
  flash->backing_storage = s_ctx.flash;
  flash->backing_storage_size = sizeof(s_ctx.flash);
  flash->sector_size = PROBE_SECTOR_BYTES;
  flash->size_bytes = PROBE_FLASH_BYTES;
  flash->program_size = PROBE_SECTOR_BYTES;
  fake_esy_hal_boot_get_state()->layout = (esy_bootloader_layout_t){
      .slot_addr = {PROBE_SLOT_A, PROBE_SLOT_B},
      .meta_addr = {PROBE_META_A, PROBE_META_B},
      .slot_size = PROBE_SLOT_BYTES,
      .meta_size = {ESY_FIRMWARE_METADATA_SIZE, ESY_FIRMWARE_METADATA_SIZE}};
  fake_esy_hal_boot_get_state()->runtime.running_address = PROBE_SLOT_A + PROBE_SECTOR_BYTES;

  uint8_t old_image[PROBE_IMAGE_BYTES] = {0};
  esy_firmware_header_t current = {
      .version = 2u,
      .firmware_size = sizeof(old_image),
      .state = ESY_FIRMWARE_STATE_UNCONFIRMED};
  esy_sha256(old_image, sizeof(old_image), current.image_sha256);
  esy_firmware_header_t fallback = current;
  fallback.version = 1u;
  fallback.state = ESY_FIRMWARE_STATE_CONFIRMED;
  REQUIRE(esy_bootloader_write_meta_data(0, &current) == ESY_OK);
  REQUIRE(esy_bootloader_write_meta_data(1, &fallback) == ESY_OK);
  REQUIRE(esy_bootloader_read_meta_data(1, &fallback) == ESY_OK);

  esy_bootloader_config_t boot = {.verify_signature = verify_signature};
  REQUIRE(esy_bootloader_init(&boot) == ESY_OK);
  REQUIRE(esy_bootloader_get_selected_slot() == 1);

  uint8_t new_image[PROBE_IMAGE_BYTES];
  memset(new_image, 0xA5, sizeof(new_image));
  esy_image_manifest_t candidate = {
      .version = 3u, .image_size = sizeof(new_image)};
  esy_sha256(new_image, sizeof(new_image), candidate.image_sha256);
  uint8_t cache[PROBE_SECTOR_BYTES];
  esy_ota_flash_t writer = {0};
  esy_ota_provider_t provider;
  esy_ota_flash_static_resources_t resources = {cache, sizeof(cache)};
  esy_ota_flash_config_t cfg = {.static_resources = &resources};
  REQUIRE(esy_ota_flash_init(&writer, &cfg) == ESY_OK);
  REQUIRE(esy_ota_flash_get_provider(&writer, &provider) == ESY_OK);
  uint32_t writes_before = flash->write_count;
  uint32_t erases_before = flash->erase_count;
  REQUIRE(provider.begin(provider.context, &candidate) == ESY_OK);
  REQUIRE(provider.write(provider.context, 0u, new_image, 1u) == ESY_OK);
  REQUIRE(flash->write_count == writes_before);
  REQUIRE(flash->erase_count == erases_before);

  /* Discard RAM session state as a reset would, leaving Flash unchanged. */
  REQUIRE(esy_ota_flash_deinit(&writer) == ESY_OK);
  REQUIRE(esy_ota_flash_init(&writer, &cfg) == ESY_OK);
  REQUIRE(esy_ota_flash_get_provider(&writer, &provider) == ESY_OK);
  esy_firmware_header_t stored;
  REQUIRE(esy_bootloader_read_meta_data(1, &stored) == ESY_OK);
  REQUIRE(memcmp(&stored, &fallback, sizeof(stored)) == 0);
  REQUIRE(esy_bootloader_init(&boot) == ESY_OK);
  REQUIRE(esy_bootloader_get_selected_slot() == 1);

  /* Once a sector changes, old metadata cannot authorize that partial image. */
  REQUIRE(provider.begin(provider.context, &candidate) == ESY_OK);
  REQUIRE(provider.write(provider.context, 0u, new_image, PROBE_SECTOR_BYTES) == ESY_OK);
  REQUIRE(esy_ota_flash_deinit(&writer) == ESY_OK);
  REQUIRE(esy_ota_flash_init(&writer, &cfg) == ESY_OK);
  REQUIRE(esy_ota_flash_get_provider(&writer, &provider) == ESY_OK);
  REQUIRE(esy_bootloader_read_meta_data(1, &stored) == ESY_OK);
  REQUIRE(memcmp(&stored, &fallback, sizeof(stored)) == 0);
  REQUIRE(esy_bootloader_init(&boot) == ESY_OK);
  REQUIRE(esy_bootloader_get_selected_slot() == 0);

  /* Native rollback ownership prevents erasing its fallback before acceptance. */
  fake_esy_hal_boot_state_t *native = fake_esy_hal_boot_get_state();
  native->runtime.mode = ESY_BOOT_MODE_MANAGED;
  native->runtime.update_allowed = false;
  writes_before = flash->write_count;
  erases_before = flash->erase_count;
  REQUIRE(provider.begin(provider.context, &candidate) == ESY_ERR_FAIL);
  REQUIRE(flash->write_count == writes_before);
  REQUIRE(flash->erase_count == erases_before);
  REQUIRE(esy_bootloader_mark_success() == ESY_OK);
  REQUIRE(native->confirm_count == 1u);

  /* Simulate native acceptance, a complete update and the next native boot. */
  native->runtime.update_allowed = true;
  REQUIRE(provider.begin(provider.context, &candidate) == ESY_OK);
  REQUIRE(provider.write(provider.context, 0u, new_image, sizeof(new_image)) == ESY_OK);
  REQUIRE(provider.finish(provider.context) == ESY_OK);
  REQUIRE(native->pending_count == 0u);
  REQUIRE(esy_bootloader_read_meta_data(1, &stored) == ESY_OK);
  REQUIRE(memcmp(&stored, &fallback, sizeof(stored)) == 0);
  REQUIRE(provider.activate(provider.context) == ESY_OK);
  REQUIRE(native->pending_count == 1u);
  REQUIRE(native->pending_address == PROBE_SLOT_B);
  REQUIRE(native->pending_size == sizeof(new_image));
  REQUIRE(esy_bootloader_init(&boot) == ESY_OK);
  REQUIRE(esy_bootloader_get_selected_slot() == 0);
  REQUIRE(esy_bootloader_read_meta_data(1, &stored) == ESY_OK);
  REQUIRE(stored.state == ESY_FIRMWARE_STATE_TRY_BOOT);
  native->runtime.running_address = PROBE_SLOT_B;
  native->runtime.update_allowed = false;
  REQUIRE(esy_bootloader_init(&boot) == ESY_OK);
  REQUIRE(esy_bootloader_get_selected_slot() == 1);
  REQUIRE(esy_bootloader_get_flash_slot() == -1);
  REQUIRE(esy_bootloader_mark_success() == ESY_OK);
  REQUIRE(native->confirm_count == 2u);
  REQUIRE(esy_bootloader_read_meta_data(1, &stored) == ESY_OK);
  REQUIRE(stored.state == ESY_FIRMWARE_STATE_CONFIRMED);
  return 0;
}
