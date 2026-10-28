/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_boot.h"

#include "esy_hb_boot_private.h"

#include "esp_system.h"

#define NATIVE_BOOT_CUSTOM_DATA_SUBTYPE_MIN 0x40u
#define NATIVE_BOOT_CUSTOM_DATA_SUBTYPE_MAX 0x7Fu

esy_err_t esy_hb_boot_get_runtime(esy_boot_runtime_t *runtime) {
  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  return ESY_ERR_NOT_SUPPORTED;
#else
  return native_boot_get_runtime(runtime);
#endif
}

static bool metadata_has_reserved_partition(uint32_t address, uint32_t size) {
  esp_partition_iterator_t iterator = esp_partition_find(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (iterator != NULL) {
    const esp_partition_t *partition = esp_partition_get(iterator);
    const bool contained = partition != NULL && !partition->encrypted &&
        !partition->readonly && partition->flash_chip == esp_flash_default_chip &&
        partition->subtype >= NATIVE_BOOT_CUSTOM_DATA_SUBTYPE_MIN &&
        partition->subtype <= NATIVE_BOOT_CUSTOM_DATA_SUBTYPE_MAX &&
        address >= partition->address &&
        (uint64_t)address + size <= (uint64_t)partition->address + partition->size;
    if (contained) {
      esp_partition_iterator_release(iterator);
      return true;
    }
    iterator = esp_partition_next(iterator);
  }
  return false;
}

esy_err_t esy_hb_boot_validate_layout(const esy_bootloader_layout_t *layout) {
  if (layout == NULL || layout->slot_size == 0u)
    return ESY_ERR_INVALID_ARG;
  for (uint32_t i = 0u; i < ESY_BOOTLOADER_SLOT_COUNT; ++i) {
    if (layout->meta_size[i] == 0u) return ESY_ERR_INVALID_ARG;
  }
  esy_boot_runtime_t runtime;
  esy_err_t status = esy_hb_boot_get_runtime(&runtime);
  if (status != ESY_OK) return status;
  const esp_partition_t *otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
  if (otadata == NULL || otadata->encrypted || otadata->readonly ||
      otadata->flash_chip != esp_flash_default_chip || otadata->erase_size == 0u ||
      otadata->size / otadata->erase_size < 2u) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < ESY_BOOTLOADER_SLOT_COUNT; ++i) {
    const esp_partition_t *image = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + i), NULL);
    if (!native_boot_is_ota_partition(image) || image->encrypted ||
        image->readonly || image->address != layout->slot_addr[i] ||
        image->size != layout->slot_size ||
        !metadata_has_reserved_partition(layout->meta_addr[i], layout->meta_size[i]))
      return ESY_ERR_INVALID_ARG;
  }
  return ESY_OK;
}

esy_err_t esy_hb_boot_set_pending_image(uint32_t address, uint32_t image_size) {
  esy_boot_runtime_t runtime;
  esy_err_t status = esy_hb_boot_get_runtime(&runtime);
  if (status != ESY_OK) return status;
  return native_boot_set_pending_image(address, image_size);
}

esy_err_t esy_hb_boot_confirm_running_image(void) {
  esy_boot_runtime_t runtime;
  esy_err_t status = esy_hb_boot_get_runtime(&runtime);
  if (status != ESY_OK) return status;
  const esp_partition_t *next = esp_ota_get_boot_partition();
  if (next == NULL || next->address != runtime.running_address)
    return ESY_ERR_STATE;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t result = esp_ota_get_state_partition(running, &state);
  /* An initially flashed ota_0 may have no otadata state yet. */
  if (result == ESP_ERR_NOT_FOUND) return ESY_OK;
  if (result != ESP_OK) return ESY_ERR_IO;
  if (state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED) return ESY_OK;
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return ESY_ERR_STATE;
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK ? ESY_OK : ESY_ERR_IO;
}

void esy_hb_boot_jump_to_app(uint32_t app_address) {
  const esp_partition_t *image = native_boot_find_image(app_address);
  if (image == NULL || image->encrypted || esp_flash_encryption_enabled() ||
      !native_boot_has_two_ota_images()) return;
  const esp_partition_t *next = esp_ota_get_boot_partition();
  if (next != NULL && next->address == app_address) {
    /* Preserve native trial state when activation already selected this image. */
    esp_restart();
  }
  if (native_boot_set_pending_image(app_address, image->size) != ESY_OK) return;
  esp_restart();
}
