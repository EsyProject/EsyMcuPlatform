/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_boot.h"

#include <string.h>

static fake_esy_hal_boot_state_t s_ctx;

void fake_esy_hal_boot_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.runtime.update_allowed = true;
}

fake_esy_hal_boot_state_t *fake_esy_hal_boot_get_state(void) { return &s_ctx; }

esy_err_t esy_hal_boot_get_layout(esy_bootloader_layout_t *layout) {
  s_ctx.query_count++;
  if (layout == NULL) return ESY_ERR_INVALID_ARG;
  if (s_ctx.result != ESY_OK) return s_ctx.result;
  *layout = s_ctx.layout;
  return ESY_OK;
}

esy_err_t esy_hal_boot_get_runtime(esy_boot_runtime_t *runtime) {
  s_ctx.runtime_query_count++;
  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
  if (s_ctx.runtime_result != ESY_OK) return s_ctx.runtime_result;
  *runtime = s_ctx.runtime;
  return ESY_OK;
}

esy_err_t esy_hal_boot_set_pending_image(uint32_t address, uint32_t image_size) {
  s_ctx.pending_count++;
  s_ctx.pending_address = address;
  s_ctx.pending_size = image_size;
  return s_ctx.pending_result;
}

esy_err_t esy_hal_boot_confirm_running_image(void) {
  s_ctx.confirm_count++;
  return s_ctx.confirm_result;
}

void esy_hal_boot_jump_to_app(uint32_t app_address) {
  s_ctx.last_jump_address = app_address;
  s_ctx.jump_count++;
}
