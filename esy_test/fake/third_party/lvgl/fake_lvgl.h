/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_LVGL_H
#define FAKE_LVGL_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool fail_display_create;
  bool fail_indev_create;
  uint32_t init_count;
  uint32_t display_create_count;
  uint32_t display_delete_count;
  uint32_t indev_create_count;
  uint32_t indev_delete_count;
  uint32_t flush_ready_count;
  void (*flush_ready_hook)(lv_display_t *display); /**< Optional interleaving hook. */
  uint32_t timer_handler_count;
  void (*timer_handler_hook)(void); /**< Optional rendering interleaving hook. */
  uint32_t timer_handler_return_ms;
  lv_display_t *last_display;
  lv_indev_t *last_indev;
} fake_lvgl_state_t;

void fake_lvgl_reset(void);
fake_lvgl_state_t *fake_lvgl_get_state(void);

void fake_lvgl_invoke_flush(lv_display_t *display, const lv_area_t *area,
                            uint8_t *px_map);
void fake_lvgl_invoke_flush_wait(lv_display_t *display);
void fake_lvgl_invoke_input_read(lv_indev_t *indev, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_LVGL_H */
