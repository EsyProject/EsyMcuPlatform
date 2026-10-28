/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_lvgl.h"

#include <string.h>

#ifndef FAKE_LVGL_DISPLAY_MAX
#define FAKE_LVGL_DISPLAY_MAX 4u
#endif

#ifndef FAKE_LVGL_INDEV_MAX
#define FAKE_LVGL_INDEV_MAX 4u
#endif

struct lv_display_s {
  bool used;
  uint16_t width;
  uint16_t height;
  lv_color_format_t color_format;
  void *buf1;
  void *buf2;
  uint32_t buf_size;
  lv_display_render_mode_t render_mode;
  void (*flush_cb)(lv_display_t *, const lv_area_t *, uint8_t *);
  void *driver_data;
  void (*flush_wait_cb)(lv_display_t *);
};

struct lv_indev_s {
  bool used;
  lv_indev_type_t type;
  void (*read_cb)(lv_indev_t *, lv_indev_data_t *);
  void *driver_data;
};

typedef struct {
  fake_lvgl_state_t state;
  lv_display_t displays[FAKE_LVGL_DISPLAY_MAX];
  lv_indev_t indevs[FAKE_LVGL_INDEV_MAX];
  uint32_t (*tick_cb)(void);
} fake_lvgl_ctx_t;
static fake_lvgl_ctx_t s_ctx;

void fake_lvgl_reset(void) {
  memset(&s_ctx.state, 0, sizeof(s_ctx.state));
  memset(s_ctx.displays, 0, sizeof(s_ctx.displays));
  memset(s_ctx.indevs, 0, sizeof(s_ctx.indevs));
  s_ctx.tick_cb = NULL;
}

fake_lvgl_state_t *fake_lvgl_get_state(void) { return &s_ctx.state; }

void lv_init(void) { s_ctx.state.init_count++; }

void lv_tick_set_cb(uint32_t (*cb)(void)) { s_ctx.tick_cb = cb; }

lv_display_t *lv_display_create(uint16_t width, uint16_t height) {
  if (s_ctx.state.fail_display_create) {
    return NULL;
  }

  for (uint32_t i = 0u; i < FAKE_LVGL_DISPLAY_MAX; i++) {
    if (!s_ctx.displays[i].used) {
      memset(&s_ctx.displays[i], 0, sizeof(s_ctx.displays[i]));
      s_ctx.displays[i].used = true;
      s_ctx.displays[i].width = width;
      s_ctx.displays[i].height = height;
      s_ctx.state.display_create_count++;
      s_ctx.state.last_display = &s_ctx.displays[i];
      return &s_ctx.displays[i];
    }
  }

  return NULL;
}

void lv_display_delete(lv_display_t *display) {
  if ((display == NULL) || !display->used) {
    return;
  }

  memset(display, 0, sizeof(*display));
  s_ctx.state.display_delete_count++;
}

void lv_display_set_color_format(lv_display_t *display,
                                 lv_color_format_t fmt) {
  if (display != NULL) {
    display->color_format = fmt;
  }
}

void lv_display_set_default(lv_display_t *display) { (void)display; }
void lv_display_set_flush_wait_cb(lv_display_t *display, void (*cb)(lv_display_t *)) {
  display->flush_wait_cb = cb;
}
void fake_lvgl_invoke_flush_wait(lv_display_t *display) {
  if (display->flush_wait_cb) display->flush_wait_cb(display);
}

void lv_display_set_buffers(lv_display_t *display, void *buf1, void *buf2,
                            uint32_t size, lv_display_render_mode_t mode) {
  if (display != NULL) {
    display->buf1 = buf1;
    display->buf2 = buf2;
    display->buf_size = size;
    display->render_mode = mode;
  }
}

void lv_display_set_flush_cb(lv_display_t *display,
                             void (*cb)(lv_display_t *, const lv_area_t *,
                                        uint8_t *)) {
  if (display != NULL) {
    display->flush_cb = cb;
  }
}

void lv_display_set_driver_data(lv_display_t *display, void *driver_data) {
  if (display != NULL) {
    display->driver_data = driver_data;
  }
}

void *lv_display_get_driver_data(lv_display_t *display) {
  return (display != NULL) ? display->driver_data : NULL;
}

void lv_display_flush_ready(lv_display_t *display) {
  if (s_ctx.state.flush_ready_hook != NULL) s_ctx.state.flush_ready_hook(display);
  s_ctx.state.flush_ready_count++;
}

uint32_t lv_timer_handler(void) {
  s_ctx.state.timer_handler_count++;
  if (s_ctx.state.timer_handler_hook != NULL) s_ctx.state.timer_handler_hook();
  if (s_ctx.tick_cb != NULL) {
    (void)s_ctx.tick_cb();
  }
  return s_ctx.state.timer_handler_return_ms;
}

lv_indev_t *lv_indev_create(void) {
  if (s_ctx.state.fail_indev_create) {
    return NULL;
  }

  for (uint32_t i = 0u; i < FAKE_LVGL_INDEV_MAX; i++) {
    if (!s_ctx.indevs[i].used) {
      memset(&s_ctx.indevs[i], 0, sizeof(s_ctx.indevs[i]));
      s_ctx.indevs[i].used = true;
      s_ctx.state.indev_create_count++;
      s_ctx.state.last_indev = &s_ctx.indevs[i];
      return &s_ctx.indevs[i];
    }
  }

  return NULL;
}

void lv_indev_delete(lv_indev_t *indev) {
  if ((indev == NULL) || !indev->used) {
    return;
  }

  memset(indev, 0, sizeof(*indev));
  s_ctx.state.indev_delete_count++;
}

void lv_indev_set_type(lv_indev_t *indev, lv_indev_type_t type) {
  if (indev != NULL) {
    indev->type = type;
  }
}

void lv_indev_set_read_cb(lv_indev_t *indev,
                          void (*cb)(lv_indev_t *, lv_indev_data_t *)) {
  if (indev != NULL) {
    indev->read_cb = cb;
  }
}

void lv_indev_set_driver_data(lv_indev_t *indev, void *driver_data) {
  if (indev != NULL) {
    indev->driver_data = driver_data;
  }
}

void *lv_indev_get_driver_data(lv_indev_t *indev) {
  return (indev != NULL) ? indev->driver_data : NULL;
}

void fake_lvgl_invoke_flush(lv_display_t *display, const lv_area_t *area,
                            uint8_t *px_map) {
  if ((display != NULL) && (display->flush_cb != NULL)) {
    display->flush_cb(display, area, px_map);
  }
}

void fake_lvgl_invoke_input_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if ((indev != NULL) && (indev->read_cb != NULL)) {
    indev->read_cb(indev, data);
  }
}

__attribute__((constructor)) static void fake_lvgl_ctor(void) {
  fake_lvgl_reset();
}
