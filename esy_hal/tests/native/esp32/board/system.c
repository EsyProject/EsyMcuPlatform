/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_board/esy_board.h"

void esy_board_init(void) {}

bool esy_board_get_system_config(esy_system_config_t *config) {
  if (config == NULL) return false;
  *config = (esy_system_config_t){.sys_clock_khz = 240000u, .enable_us_tick = true};
  return true;
}

bool esy_board_get_timer_task_inner_config(
    esy_timer_mode_t mode, esy_timer_task_inner_config_t *config) {
  (void)mode;
  if (config == NULL) return false;
  *config = (esy_timer_task_inner_config_t){.source = ESY_TIMER_SOURCE_DEFAULT};
  return true;
}

bool esy_board_get_bootloader_layout(esy_bootloader_layout_t *layout) {
  if (layout == NULL) return false;
  *layout = (esy_bootloader_layout_t){
      .slot_addr = {0x20000u, 0x120000u},
      .meta_addr = {0x220000u, 0x221000u},
      .slot_size = 0x100000u, .meta_size = {0x1000u, 0x1000u}};
  return true;
}
