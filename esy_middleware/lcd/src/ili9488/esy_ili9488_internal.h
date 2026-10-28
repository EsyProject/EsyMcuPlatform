/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ILI9488_INTERNAL_H
#define ESY_ILI9488_INTERNAL_H

#include "esy_middleware/lcd/esy_ili9488.h"
#include <stdatomic.h>

#define ILI9488_CTX_MAGIC 0x494C4938u

typedef struct esy_ili9488_ctx_s esy_ili9488_ctx_t;

typedef enum {
  ILI9488_STATE_DETACHED = 0,
  ILI9488_STATE_IDLE,
  ILI9488_STATE_FLUSHING,
  ILI9488_STATE_CLEANUP_FAILED
} esy_ili9488_state_t;

struct esy_ili9488_ctx_s {
  uint32_t magic;
  esy_spi_t *spi;
  esy_gpio_t *cs_gpio;
  esy_gpio_t *dc_gpio;
  esy_gpio_t *rst_gpio;
  bool bgr;
  esy_ili9488_parameter_format_t parameter_format;
  uint16_t width;
  uint16_t height;
  esy_color_format_t color_format;
  esy_lcd_dir_t dir;
  const esy_ili9488_panel_profile_t *panel_profile;
  esy_ili9488_backlight_set_cb_t backlight_set_cb;
  void *backlight_set_cb_arg;

  esy_spi_submit_prepared_t spi_submit_cb;
  void *spi_submit_cb_arg;

  esy_lvgl_flush_done_cb_t lvgl_flush_done_cb;
  void *lvgl_flush_done_cb_arg;

  /* One producer owns the request until completion publishes IDLE. */
  uint16_t x0, y0, x1, y1;
  atomic_int state;
  bool suspended;
  bool backlight_enabled;
};

#endif /* ESY_ILI9488_INTERNAL_H */
