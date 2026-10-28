/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_bootloader.h"

#include <string.h>

static fake_esy_bootloader_state_t s_ctx;

void fake_esy_bootloader_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.init_result = ESY_OK;
  s_ctx.jump_to_app_result = false;
  s_ctx.selected_slot = 0;
  s_ctx.running_slot = 0;
  s_ctx.flash_slot = 1;
  s_ctx.flash_slot_addr = 0x1000u;
  s_ctx.flash_slot_size = 256u;
  s_ctx.read_meta_result = ESY_OK;
  s_ctx.write_meta_result = ESY_OK;
}

fake_esy_bootloader_state_t *fake_esy_bootloader_get_state(
    void) {
  return &s_ctx;
}

esy_err_t esy_bootloader_init(
    const esy_bootloader_config_t *config) {
  (void)config;
  s_ctx.init_count++;
  return s_ctx.init_result;
}

bool esy_bootloader_jump_to_app(void) {
  s_ctx.jump_to_app_count++;
  return s_ctx.jump_to_app_result;
}

int esy_bootloader_get_selected_slot(void) {
  return s_ctx.selected_slot;
}

int esy_bootloader_get_running_slot(void) {
  return s_ctx.running_slot;
}

int esy_bootloader_get_flash_slot(void) {
  return s_ctx.flash_slot;
}

uint32_t esy_bootloader_get_flash_slot_addr(void) {
  return s_ctx.flash_slot_addr;
}

uint32_t esy_bootloader_get_flash_slot_size(void) {
  return s_ctx.flash_slot_size;
}

esy_err_t esy_bootloader_read_meta_data(
    int slot, esy_firmware_header_t *header) {
  s_ctx.read_meta_count++;
  s_ctx.last_read_meta_slot = slot;
  if (!header) {
    return ESY_ERR_INVALID_ARG;
  }
  *header = s_ctx.read_meta_header;
  return s_ctx.read_meta_result;
}

esy_err_t esy_bootloader_write_meta_data(
    int slot, const esy_firmware_header_t *header) {
  s_ctx.write_meta_count++;
  s_ctx.last_write_meta_slot = slot;
  if (!header) {
    return ESY_ERR_INVALID_ARG;
  }
  s_ctx.last_write_meta_header = *header;
  return s_ctx.write_meta_result;
}

esy_err_t esy_bootloader_mark_success(void) {
  s_ctx.mark_success_count++;
  return s_ctx.mark_success_result;
}

esy_err_t esy_bootloader_activate_image(int slot) {
  s_ctx.activate_count++;
  s_ctx.last_activate_slot = slot;
  s_ctx.meta_writes_at_activation = s_ctx.write_meta_count;
  return s_ctx.activate_result;
}
