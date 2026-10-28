/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_ESP32_BOOT_PRIVATE_H
#define ESY_HB_ESP32_BOOT_PRIVATE_H

#include "esy_common/defs/image/esy_boot_defs.h"
#include "esy_common/defs/base/esy_common_defs.h"

#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_ota_ops.h"

/* Native boot queries, activation and handoff share the same validation. */
static inline bool native_boot_is_ota_partition(const esp_partition_t *partition) {
  return partition != NULL && partition->type == ESP_PARTITION_TYPE_APP &&
         partition->flash_chip == esp_flash_default_chip &&
         (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
          partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1);
}

static inline bool native_boot_has_two_ota_images(void) {
  uint32_t count = 0u;
  esp_partition_iterator_t iterator = esp_partition_find(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (iterator != NULL) {
    const esp_partition_t *partition = esp_partition_get(iterator);
    if (partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
        partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
      if (!native_boot_is_ota_partition(partition)) {
        esp_partition_iterator_release(iterator);
        return false;
      }
      ++count;
    }
    iterator = esp_partition_next(iterator);
  }
  return count == ESY_BOOTLOADER_SLOT_COUNT;
}

static inline const esp_partition_t *native_boot_find_image(uint32_t address) {
  for (uint32_t i = 0u; i < ESY_BOOTLOADER_SLOT_COUNT; ++i) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + i), NULL);
    if (native_boot_is_ota_partition(partition) &&
        partition->address == address) return partition;
  }
  return NULL;
}

static inline esy_err_t native_boot_get_runtime(esy_boot_runtime_t *runtime) {
  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
  /* Raw Flash I/O cannot encrypt or decrypt the image/metadata contract. */
  if (esp_flash_encryption_enabled()) return ESY_ERR_NOT_SUPPORTED;
  /* Count all OTA entries; SDK prefix counting alone can miss an ota_3 gap. */
  if (!native_boot_has_two_ota_images()) return ESY_ERR_NOT_SUPPORTED;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!native_boot_is_ota_partition(running) || running->encrypted)
    return ESY_ERR_STATE;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t result = esp_ota_get_state_partition(running, &state);
  if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) return ESY_ERR_IO;
  if (state != ESP_OTA_IMG_VALID && state != ESP_OTA_IMG_UNDEFINED &&
      state != ESP_OTA_IMG_PENDING_VERIFY && state != ESP_OTA_IMG_NEW)
    return ESY_ERR_STATE;
  const esp_partition_t *next = esp_ota_get_boot_partition();
  if (next == NULL) return ESY_ERR_STATE;
  *runtime = (esy_boot_runtime_t){
      .mode = ESY_BOOT_MODE_MANAGED,
      .running_address = running->address,
      .update_allowed = state != ESP_OTA_IMG_PENDING_VERIFY &&
                        state != ESP_OTA_IMG_NEW &&
                        next->address == running->address,
  };
  return ESY_OK;
}

static inline esy_err_t native_boot_set_pending_image(uint32_t address,
                                                      uint32_t image_size) {
  if (image_size == 0u) return ESY_ERR_INVALID_ARG;
  esy_boot_runtime_t runtime;
  esy_err_t status = native_boot_get_runtime(&runtime);
  if (status != ESY_OK) return status;
  if (!runtime.update_allowed || runtime.running_address == address)
    return ESY_ERR_STATE;
  const esp_partition_t *target = native_boot_find_image(address);
  if (target == NULL || image_size > target->size) return ESY_ERR_INVALID_ARG;
  if (target->encrypted) return ESY_ERR_NOT_SUPPORTED;
  /* SDK validation includes its native image format and configured security. */
  const esp_err_t result = esp_ota_set_boot_partition(target);
  if (result == ESP_OK) return ESY_OK;
  if (result == ESP_ERR_OTA_VALIDATE_FAILED) return ESY_ERR_HASH_FAIL;
  return ESY_ERR_IO;
}

#endif /* ESY_HB_ESP32_BOOT_PRIVATE_H */
