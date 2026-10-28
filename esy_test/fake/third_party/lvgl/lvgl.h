/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_LVGL_H
#define TEST_LVGL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LVGL_VERSION_MAJOR 9
#define LVGL_VERSION_MINOR 6
#define LVGL_VERSION_PATCH 0
#define LV_OS_NONE 0
#define LV_USE_OS LV_OS_NONE
#define LV_STDLIB_BUILTIN 0
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_ADR 0
#define LV_COLOR_DEPTH 16
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_NO_TIMER_READY UINT32_MAX

typedef int32_t lv_coord_t;

typedef enum {
  LV_COLOR_FORMAT_UNKNOWN = 0,
  LV_COLOR_FORMAT_RGB565 = 0x12
} lv_color_format_t;

typedef enum {
  LV_DISPLAY_RENDER_MODE_PARTIAL = 0,
  LV_DISPLAY_RENDER_MODE_DIRECT,
  LV_DISPLAY_RENDER_MODE_FULL
} lv_display_render_mode_t;

typedef enum {
  LV_INDEV_TYPE_POINTER = 0
} lv_indev_type_t;

typedef enum {
  LV_INDEV_STATE_RELEASED = 0,
  LV_INDEV_STATE_PRESSED
} lv_indev_state_t;

typedef struct {
  lv_coord_t x1;
  lv_coord_t y1;
  lv_coord_t x2;
  lv_coord_t y2;
} lv_area_t;

typedef struct {
  lv_coord_t x;
  lv_coord_t y;
} lv_point_t;

typedef struct {
  lv_point_t point;
  lv_indev_state_t state;
} lv_indev_data_t;

typedef struct lv_display_s lv_display_t;
typedef struct lv_indev_s lv_indev_t;

void lv_init(void);
void lv_tick_set_cb(uint32_t (*cb)(void));
lv_display_t *lv_display_create(uint16_t width, uint16_t height);
void lv_display_delete(lv_display_t *display);
void lv_display_set_color_format(lv_display_t *display, lv_color_format_t fmt);
void lv_display_set_default(lv_display_t *display);
void lv_display_set_buffers(lv_display_t *display, void *buf1, void *buf2,
                            uint32_t size, lv_display_render_mode_t mode);
void lv_display_set_flush_cb(lv_display_t *display,
                             void (*cb)(lv_display_t *, const lv_area_t *,
                                        uint8_t *));
void lv_display_set_flush_wait_cb(lv_display_t *display, void (*cb)(lv_display_t *));
void lv_display_set_driver_data(lv_display_t *display, void *driver_data);
void *lv_display_get_driver_data(lv_display_t *display);
void lv_display_flush_ready(lv_display_t *display);
uint32_t lv_timer_handler(void);

lv_indev_t *lv_indev_create(void);
void lv_indev_delete(lv_indev_t *indev);
void lv_indev_set_type(lv_indev_t *indev, lv_indev_type_t type);
void lv_indev_set_read_cb(lv_indev_t *indev,
                          void (*cb)(lv_indev_t *, lv_indev_data_t *));
void lv_indev_set_driver_data(lv_indev_t *indev, void *driver_data);
void *lv_indev_get_driver_data(lv_indev_t *indev);

#ifdef __cplusplus
}
#endif

#endif /* TEST_LVGL_H */
