/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lvgl/esy_lvgl_touch.h"

static esy_err_t touch_read(uint16_t *x, uint16_t *y, bool *pressed, void *arg) {
  return esy_touch_read_input((esy_touch_t *)arg, x, y, pressed);
}

esy_err_t esy_lvgl_touch_input_init(esy_lvgl_pointer_input_t *input,
                                    esy_touch_t *touch) {
  if (input == NULL || touch == NULL) return ESY_ERR_INVALID_ARG;
  if (!esy_touch_is_attached(touch)) return ESY_ERR_STATE;
  *input = (esy_lvgl_pointer_input_t){.read = touch_read, .arg = touch};
  return ESY_OK;
}
