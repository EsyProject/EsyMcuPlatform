/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lvgl/esy_lvgl.h"
#ifdef WITH_TOUCH
#include "esy_middleware/lvgl/esy_lvgl_touch.h"
#endif
#include "fake_esy_os_task.h"
#include "fake_esy_os_queue.h"
#include "fake_esy_hal_system.h"
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#ifdef REAL_LVGL
#include "lvgl.h"
#endif

typedef struct {
  esy_lvgl_t storage;
  esy_lvgl_t *handle;
  esy_os_task_static_storage_t task_storage;
  esy_os_queue_static_storage_t queue_storage;
  ESY_ALIGNED_STORAGE(stack, 4096u);
  ESY_ALIGNED_STORAGE(draw, 128u);
  ESY_ALIGNED_STORAGE(draw2, 128u);
  esy_lvgl_event_t events[4];
  esy_lvgl_static_resources_t resources;
  esy_lvgl_config_t config;
  esy_lcd_t lcd;
#ifdef WITH_TOUCH
  esy_touch_t touch;
  esy_lvgl_pointer_input_t pointer_input;
#endif
  fake_esy_os_task_entry_t *task;
  void (*done)(void *);
  void *done_arg;
  unsigned transfers, completed, stopped, waits;
  bool posted;
  jmp_buf parked;
} consumer_ctx_t;
static consumer_ctx_t s_ctx;

static esy_err_t flush(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
    const uint8_t *pixels, void (*done)(void *), void *done_arg, void *arg) {
  (void)x0; (void)y0; (void)x1; (void)y1; (void)arg;
  assert(!s_ctx.done);
  assert(pixels && done);
  if (!s_ctx.transfers) assert(pixels[0] == 0xf8 && pixels[1] == 0x00);
  s_ctx.done=done; s_ctx.done_arg=done_arg; ++s_ctx.transfers;
  return ESY_OK;
}
#ifdef WITH_TOUCH
static esy_err_t touch_poll(void *arg) { (void)arg; return ESY_OK; }
static esy_err_t touch_sample(esy_touch_data_t *sample, void *arg) {
  (void)arg;
  *sample = (esy_touch_data_t){.x=3u, .y=4u, .is_pressed=true};
  return ESY_OK;
}
#endif
static void operation(void *arg) {
  (void)arg;
#ifdef REAL_LVGL
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x0000ff), 0);
#endif
}
static void completion(esy_lvgl_event_result_t result, void *arg) {
  (void)arg; assert(result == ESY_LVGL_EVENT_EXECUTED); ++s_ctx.completed;
}
static esy_err_t ui_start(void *arg) {
  (void)arg;
#ifdef REAL_LVGL
  lv_obj_remove_style_all(lv_screen_active());
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0xff0000), 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
#endif
  return ESY_OK;
}
static void ui_stop(void *arg) { (void)arg; ++s_ctx.stopped; }
static void parked(void) { longjmp(s_ctx.parked, 1); }
static void wait(void) {
  assert(++s_ctx.waits < 1000u);
  fake_esy_hal_system_advance_tick(5u);
  if (s_ctx.done) {
    void (*done)(void *)=s_ctx.done; s_ctx.done=NULL;
    /* Simulate a transport task that can only run when the GUI blocks. */
    fake_esy_os_task_set_current(NULL);
    done(s_ctx.done_arg);
    fake_esy_os_task_set_current(s_ctx.task->task_handle);
  }
  if (!s_ctx.posted) {
    esy_lvgl_event_t event={.callback=operation,.complete=completion};
    assert(esy_lvgl_send_event(s_ctx.handle,&event)==ESY_OK);
    s_ctx.posted=true;
  } else if (s_ctx.completed) {
    assert(esy_lvgl_request_stop(s_ctx.handle)==ESY_OK);
  }
}
int main(void) {
  for (unsigned cycle=0u; cycle<2u; ++cycle) {
    memset(&s_ctx,0,sizeof(s_ctx));
    fake_esy_os_task_reset(); fake_esy_os_queue_reset(); fake_esy_hal_system_reset();
    s_ctx.resources=(esy_lvgl_static_resources_t){.context=&s_ctx.storage,
      .task_stack_buffer=s_ctx.stack.bytes,.task_stack_buffer_size=sizeof(s_ctx.stack.bytes),
      .task_control_storage=&s_ctx.task_storage,.event_queue_buffer=s_ctx.events,.event_queue_depth=4u,
      .event_queue_control_storage=&s_ctx.queue_storage,.draw_buffer1=s_ctx.draw.bytes,
      .draw_buffer_size=sizeof(s_ctx.draw.bytes)};
    if (cycle) s_ctx.resources.draw_buffer2=s_ctx.draw2.bytes;
    s_ctx.lcd.ctx=&s_ctx; s_ctx.lcd.flush=flush;
    esy_lvgl_default_config(&s_ctx.config);
    s_ctx.config.width=32u; s_ctx.config.height=16u; s_ctx.config.lcd=&s_ctx.lcd;
#ifdef WITH_TOUCH
    s_ctx.touch.ctx=&s_ctx; s_ctx.touch.poll=touch_poll; s_ctx.touch.get_sample=touch_sample;
    assert(esy_lvgl_touch_input_init(&s_ctx.pointer_input, &s_ctx.touch)==ESY_OK);
    s_ctx.config.pointer_input=&s_ctx.pointer_input;
#endif
    s_ctx.config.on_start=ui_start; s_ctx.config.on_stop=ui_stop;
    s_ctx.config.static_resources=&s_ctx.resources;
    assert(esy_lvgl_create(&s_ctx.config,&s_ctx.handle)==ESY_OK);
    assert(esy_lvgl_start(s_ctx.handle)==ESY_OK);
    s_ctx.task=fake_esy_os_task_get_last_created();
    fake_esy_os_task_set_park_hook(parked); fake_esy_os_task_set_notify_take_hook(wait);
    fake_esy_os_task_set_current(s_ctx.task->task_handle);
    if (setjmp(s_ctx.parked)==0) s_ctx.task->entry(s_ctx.task->arg);
    fake_esy_os_task_set_current(NULL);
    esy_lvgl_status_t status;
    assert(esy_lvgl_get_status(s_ctx.handle,&status)==ESY_OK);
    assert(status.state==ESY_LVGL_STOPPED && status.last_error==ESY_OK);
    assert(s_ctx.completed==1u && s_ctx.stopped==1u && !s_ctx.done);
#ifdef REAL_LVGL
    assert(s_ctx.transfers>1u);
#endif
    assert(esy_lvgl_destroy(s_ctx.handle)==ESY_OK);
  }
  return 0;
}
