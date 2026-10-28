/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lvgl/esy_lvgl.h"
#include "fake_esy_os.h"
#include "fake_esy_os_queue.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_system.h"
#include "fake_lvgl.h"
#include "unity.h"
#include <setjmp.h>
#include <string.h>

typedef struct {
  esy_lvgl_t storage;
  esy_lvgl_t *handle;
  esy_os_task_static_storage_t task_storage;
  esy_os_queue_static_storage_t queue_storage;
  ESY_ALIGNED_STORAGE(stack, 2048u);
  ESY_ALIGNED_STORAGE(draw, 128u);
  esy_lvgl_event_t events[4];
  esy_lvgl_static_resources_t resources;
  esy_lvgl_config_t config;
  esy_lcd_t lcd;
  esy_lvgl_pointer_input_t input;
  fake_esy_os_task_entry_t *task;
  jmp_buf parked;
  unsigned started, stopped, executed, completed, cancelled, submitted, waits;
  esy_err_t start_result, flush_result, touch_result;
  bool inline_completion;
  void (*done)(void *);
  void *done_arg;
  void (*timer_hook)(void);
  void (*wait_hook)(void);
} lvgl_test_ctx_t;
static lvgl_test_ctx_t s_ctx;

static void assert_gui(void) {
  TEST_ASSERT_EQUAL_PTR(s_ctx.task->task_handle, fake_esy_os_task_get_current());
}
static esy_err_t ui_start(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  assert_gui();
  s_ctx.started++;
  return s_ctx.start_result;
}
static void ui_stop(void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  assert_gui();
  s_ctx.stopped++;
}
static esy_err_t lcd_flush(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
    const uint8_t *pixels, void (*done)(void *), void *done_arg, void *arg) {
  (void)x0; (void)y0; (void)x1; (void)y1; (void)arg;
  assert_gui();
  TEST_ASSERT_NOT_NULL(pixels);
  s_ctx.submitted++;
  s_ctx.done = done;
  s_ctx.done_arg = done_arg;
  if (s_ctx.inline_completion && s_ctx.flush_result == ESY_OK) done(done_arg);
  return s_ctx.flush_result;
}
static esy_err_t pointer_read(uint16_t *x, uint16_t *y, bool *pressed, void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg);
  assert_gui();
  *x = 3u; *y = 4u; *pressed = true;
  return s_ctx.touch_result;
}
static void stop_request(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_lvgl_request_stop(s_ctx.handle));
}
static void timer_hook(void) { if (s_ctx.timer_hook) s_ctx.timer_hook(); }
static void wait_hook(void) {
  s_ctx.waits++;
  TEST_ASSERT_LESS_THAN(100u, s_ctx.waits);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(20u, s_ctx.task->notify_take_timeout_ms);
  if (s_ctx.wait_hook) s_ctx.wait_hook();
}
static void park_hook(void) { longjmp(s_ctx.parked, 1); }
static void ready_hook(lv_display_t *display) { (void)display; assert_gui(); }
static void execute(void *arg) { TEST_ASSERT_EQUAL_PTR(&s_ctx, arg); assert_gui(); s_ctx.executed++; }
static void complete(esy_lvgl_event_result_t result, void *arg) {
  TEST_ASSERT_EQUAL_PTR(&s_ctx, arg); assert_gui(); s_ctx.completed++;
  if (result == ESY_LVGL_EVENT_CANCELLED) s_ctx.cancelled++;
}
static esy_lvgl_event_t event(void) {
  esy_lvgl_event_t e = {.callback = execute, .complete = complete, .arg = &s_ctx};
  return e;
}
static void create(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
}
static void launch(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_lvgl_start(s_ctx.handle));
  s_ctx.task = fake_esy_os_task_get_last_created();
}
static void run(void) {
  fake_esy_os_task_set_current(s_ctx.task->task_handle);
  if (setjmp(s_ctx.parked) == 0) s_ctx.task->entry(s_ctx.task->arg);
  fake_esy_os_task_set_current(NULL);
}
static esy_lvgl_status_t status(void) {
  esy_lvgl_status_t st;
  TEST_ASSERT_EQUAL(ESY_OK, esy_lvgl_get_status(s_ctx.handle, &st));
  return st;
}
void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_lvgl_reset(); fake_esy_os_reset(); fake_esy_os_queue_reset();
  fake_esy_os_task_reset(); fake_esy_hal_system_reset();
  s_ctx.resources = (esy_lvgl_static_resources_t){
      .context=&s_ctx.storage, .task_stack_buffer=s_ctx.stack.bytes,
      .task_stack_buffer_size=sizeof(s_ctx.stack.bytes), .task_control_storage=&s_ctx.task_storage,
      .event_queue_buffer=s_ctx.events, .event_queue_depth=4u,
      .event_queue_control_storage=&s_ctx.queue_storage,
      .draw_buffer1=s_ctx.draw.bytes, .draw_buffer_size=sizeof(s_ctx.draw.bytes)};
  s_ctx.lcd.ctx = &s_ctx; s_ctx.lcd.flush = lcd_flush;
  s_ctx.input = (esy_lvgl_pointer_input_t){.read=pointer_read, .arg=&s_ctx};
  esy_lvgl_default_config(&s_ctx.config);
  s_ctx.config.width=16u; s_ctx.config.height=16u;
  s_ctx.config.lcd=&s_ctx.lcd; s_ctx.config.static_resources=&s_ctx.resources;
  s_ctx.config.on_start=ui_start; s_ctx.config.on_stop=ui_stop; s_ctx.config.ui_arg=&s_ctx;
  s_ctx.config.event_budget=2u;
  s_ctx.timer_hook=stop_request;
  fake_lvgl_get_state()->timer_handler_hook=timer_hook;
  fake_lvgl_get_state()->flush_ready_hook=ready_hook;
  fake_esy_os_task_set_notify_take_hook(wait_hook);
  fake_esy_os_task_set_park_hook(park_hook);
}
void tearDown(void) {
  if (s_ctx.handle) TEST_ASSERT_EQUAL(ESY_OK, esy_lvgl_destroy(s_ctx.handle));
  TEST_ASSERT_EQUAL_UINT(0u, fake_esy_os_get_state()->malloc_call_count);
}

void test_initialized_macro_and_defaults_define_first_use(void) {
  ESY_LVGL_STATIC_STORAGE(local, 4u, 1024u);
  esy_lvgl_static_resources_t resources;
  ESY_LVGL_BIND_STATIC_RESOURCES(&resources, local);
  resources.draw_buffer1=s_ctx.draw.bytes; resources.draw_buffer_size=sizeof(s_ctx.draw.bytes);
  s_ctx.config.static_resources=&resources;
  create();
  TEST_ASSERT_EQUAL(ESY_LVGL_INITIALIZED, status().state);
  TEST_ASSERT_EQUAL_UINT(0u, fake_lvgl_get_state()->display_create_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lvgl_destroy(s_ctx.handle)); s_ctx.handle=NULL;
  esy_lvgl_default_config(NULL);
  TEST_ASSERT_EQUAL(ESY_CORE_ALL, s_ctx.config.task_core_affinity);
}
void test_invalid_configuration_retains_nothing(void) {
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_create(NULL, &s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_create(&s_ctx.config, NULL));
  s_ctx.resources.draw_buffer_size=31u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
  s_ctx.resources.draw_buffer_size=128u; s_ctx.resources.draw_buffer1++;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
  s_ctx.resources.draw_buffer1--; s_ctx.resources.draw_buffer2=s_ctx.resources.draw_buffer1;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
  s_ctx.resources.draw_buffer2=NULL; s_ctx.config.on_stop=NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
  TEST_ASSERT_NULL(s_ctx.handle);
}
void test_unattached_handles_and_missing_park_support_fail_closed(void) {
  s_ctx.lcd.ctx=NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
  s_ctx.lcd.ctx=&s_ctx; s_ctx.config.pointer_input=&s_ctx.input; s_ctx.input.read=NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
  s_ctx.input.read=pointer_read; fake_esy_os_task_set_park_supported(false);
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
}
void test_queue_and_task_failure_allow_retry(void) {
  fake_esy_os_queue_set_create_result(ESY_ERR_NO_MEM);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
  fake_esy_os_queue_set_create_result(ESY_OK); create();
  fake_esy_os_task_set_create_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_lvgl_start(s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_LVGL_INITIALIZED, status().state);
  fake_esy_os_task_set_create_result(ESY_OK); launch(); run();
}
void test_duplicate_create_start_and_stop_before_start(void) {
  create();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lvgl_create(&s_ctx.config, &s_ctx.handle));
  stop_request(); stop_request();
  TEST_ASSERT_EQUAL(ESY_LVGL_STOPPED, status().state);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lvgl_start(s_ctx.handle));
}
void test_readiness_cleanup_and_recreate_use_gui_owner(void) {
  create(); launch();
  TEST_ASSERT_EQUAL(ESY_LVGL_STARTING, status().state);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_lvgl_start(s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_lvgl_destroy(s_ctx.handle));
  run();
  TEST_ASSERT_EQUAL_UINT(1u,s_ctx.started); TEST_ASSERT_EQUAL_UINT(1u,s_ctx.stopped);
  TEST_ASSERT_EQUAL(ESY_LVGL_STOPPED,status().state);
  s_ctx.task->parked=false;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY,esy_lvgl_destroy(s_ctx.handle));
  s_ctx.task->parked=true;
  TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_destroy(s_ctx.handle));
  TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_destroy(s_ctx.handle));
  create(); launch(); run();
  TEST_ASSERT_EQUAL_UINT(2u,s_ctx.started);
}
void test_stop_before_task_entry_skips_lvgl_setup(void) {
  create(); launch(); stop_request(); run();
  TEST_ASSERT_EQUAL_UINT(0u,s_ctx.started);
  TEST_ASSERT_EQUAL_UINT(0u,fake_lvgl_get_state()->display_create_count);
}
void test_display_allocation_failure_parks_for_owner_cleanup(void) {
  fake_lvgl_get_state()->fail_display_create=true;
  create(); launch(); run();
  TEST_ASSERT_EQUAL(ESY_LVGL_FAILED,status().state);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,status().last_error);
  TEST_ASSERT_EQUAL_UINT(0u,s_ctx.started);
}
void test_input_allocation_failure_deletes_display_on_gui(void) {
  s_ctx.config.pointer_input=&s_ctx.input; fake_lvgl_get_state()->fail_indev_create=true;
  create(); launch(); run();
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM,status().last_error);
  TEST_ASSERT_EQUAL_UINT(1u,fake_lvgl_get_state()->display_delete_count);
}
void test_partial_ui_start_failure_runs_cleanup(void) {
  s_ctx.start_result=ESY_ERR_IO;
  create(); launch(); run();
  TEST_ASSERT_EQUAL(ESY_ERR_IO,status().last_error);
  TEST_ASSERT_EQUAL_UINT(1u,s_ctx.stopped);
}
static void queue_then_stop(void) {
  esy_lvgl_event_t e=event();
  if (fake_lvgl_get_state()->timer_handler_count==1u) {
    TEST_ASSERT_EQUAL(ESY_LVGL_RUNNING,status().state);
    TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_send_event(s_ctx.handle,&e));
    bool yield=true;
    TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_send_event_from_isr(s_ctx.handle,&e,&yield));
    TEST_ASSERT_FALSE(yield);
  } else stop_request();
}
void test_idle_sentinel_is_bounded_and_task_is_notified(void) {
  fake_lvgl_get_state()->timer_handler_return_ms=UINT32_MAX;
  s_ctx.timer_hook=queue_then_stop;
  create(); launch(); run();
  TEST_ASSERT_EQUAL_UINT(2u,s_ctx.executed); TEST_ASSERT_EQUAL_UINT(2u,s_ctx.completed);
  TEST_ASSERT_EQUAL_UINT(0u,s_ctx.cancelled);
  TEST_ASSERT_GREATER_THAN_UINT(0u,s_ctx.task->notify_give_from_isr_count);
}
static void cancel_queue(void) {
  esy_lvgl_event_t e=event();
  for(unsigned i=0;i<4;i++) TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_send_event(s_ctx.handle,&e));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT,esy_lvgl_send_event(s_ctx.handle,&e));
  stop_request();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,esy_lvgl_send_event(s_ctx.handle,&e));
}
void test_stop_cancels_all_accepted_events_exactly_once(void) {
  s_ctx.timer_hook=cancel_queue; create(); launch(); run();
  TEST_ASSERT_EQUAL_UINT(0u,s_ctx.executed); TEST_ASSERT_EQUAL_UINT(4u,s_ctx.completed);
  TEST_ASSERT_EQUAL_UINT(4u,s_ctx.cancelled);
}
static void stop_during_publication(esy_os_task_t *task) {
  (void)task; fake_esy_os_task_set_notify_give_hook(NULL); stop_request();
}
static void submit_racing_stop(void) {
  esy_lvgl_event_t e=event();
  fake_esy_os_task_set_notify_give_hook(stop_during_publication);
  TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_send_event(s_ctx.handle,&e));
}
void test_committed_event_survives_stop_during_wakeup(void) {
  s_ctx.timer_hook=submit_racing_stop; create(); launch(); run();
  TEST_ASSERT_EQUAL_UINT(1u,s_ctx.completed); TEST_ASSERT_EQUAL_UINT(1u,s_ctx.cancelled);
}
static void requeue(void *arg) {
  execute(arg);
  esy_lvgl_event_t e=event(); e.callback=requeue;
  TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_send_event(s_ctx.handle,&e));
}
static void sustained_events(void) {
  if(fake_lvgl_get_state()->timer_handler_count==1u) {
    esy_lvgl_event_t e=event(); e.callback=requeue;
    TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_send_event(s_ctx.handle,&e));
  } else {
    TEST_ASSERT_EQUAL_UINT(s_ctx.config.event_budget,s_ctx.executed);
    stop_request();
  }
}
void test_event_budget_preserves_render_and_stop_progress(void) {
  s_ctx.timer_hook=sustained_events; create(); launch(); run();
  TEST_ASSERT_EQUAL_UINT(2u,s_ctx.executed); TEST_ASSERT_EQUAL_UINT(1u,s_ctx.cancelled);
  TEST_ASSERT_GREATER_THAN_UINT(0u,fake_esy_os_task_get_total_delay_ms());
}
static void complete_transport(void) {
  uint32_t before=fake_lvgl_get_state()->flush_ready_count;
  fake_esy_os_task_set_current(NULL);
  s_ctx.done(s_ctx.done_arg);
  TEST_ASSERT_EQUAL_UINT32(before,fake_lvgl_get_state()->flush_ready_count);
  fake_esy_os_task_set_current(s_ctx.task->task_handle);
}
static void flush_and_stop(void) {
  const uint16_t native[]={0xf800u,0x07e0u};
  const uint8_t wire[]={0xf8u,0x00u,0x07u,0xe0u};
  const uint8_t wire_lsb[]={0x00u,0xf8u,0xe0u,0x07u};
  lv_area_t area={.x1=0,.y1=0,.x2=1,.y2=0};
  memcpy(s_ctx.draw.bytes,native,sizeof(native));
  fake_lvgl_invoke_flush(fake_lvgl_get_state()->last_display,&area,s_ctx.draw.bytes);
  if (!s_ctx.inline_completion && s_ctx.flush_result==ESY_OK) {
    const uint8_t *expected = s_ctx.config.rgb565_byte_order == ESY_LVGL_RGB565_LSB_FIRST
        ? wire_lsb : wire;
    TEST_ASSERT_EQUAL_MEMORY(expected,s_ctx.draw.bytes,sizeof(wire));
    fake_lvgl_invoke_flush_wait(fake_lvgl_get_state()->last_display);
  }
  TEST_ASSERT_EQUAL_MEMORY(native,s_ctx.draw.bytes,sizeof(native));
  TEST_ASSERT_EQUAL_UINT(1u,fake_lvgl_get_state()->flush_ready_count);
  stop_request();
}
void test_deferred_flush_completion_only_wakes_gui(void) {
  s_ctx.timer_hook=flush_and_stop; s_ctx.wait_hook=complete_transport;
  create(); launch(); run();
  TEST_ASSERT_EQUAL_UINT(1u,s_ctx.submitted);
}
void test_inline_flush_completion_restores_buffer(void) {
  s_ctx.inline_completion=true; s_ctx.timer_hook=flush_and_stop;
  create(); launch(); run();
}
void test_lsb_first_flush_sends_low_byte_first_and_restores_native_buffer(void) {
  s_ctx.config.rgb565_byte_order=ESY_LVGL_RGB565_LSB_FIRST;
  s_ctx.timer_hook=flush_and_stop; s_ctx.wait_hook=complete_transport;
  create(); launch(); run();
  TEST_ASSERT_EQUAL_UINT(1u,s_ctx.submitted);
}
void test_lsb_first_inline_completion_restores_buffer(void) {
  s_ctx.config.rgb565_byte_order=ESY_LVGL_RGB565_LSB_FIRST;
  s_ctx.inline_completion=true; s_ctx.timer_hook=flush_and_stop;
  create(); launch(); run();
}
void test_lsb_first_rejected_flush_restores_buffer(void) {
  s_ctx.config.rgb565_byte_order=ESY_LVGL_RGB565_LSB_FIRST;
  s_ctx.flush_result=ESY_ERR_IO; s_ctx.timer_hook=flush_and_stop;
  create(); launch(); run();
  TEST_ASSERT_EQUAL(ESY_ERR_IO,status().last_error);
}
void test_invalid_rgb565_byte_order_is_rejected_before_task_creation(void) {
  TEST_ASSERT_EQUAL(ESY_LVGL_RGB565_MSB_FIRST,s_ctx.config.rgb565_byte_order);
  s_ctx.config.rgb565_byte_order=(esy_lvgl_rgb565_byte_order_t)99;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,esy_lvgl_create(&s_ctx.config,&s_ctx.handle));
  TEST_ASSERT_NULL(s_ctx.handle);
  TEST_ASSERT_NULL(fake_esy_os_task_get_last_created());
}
void test_rejected_flush_restores_buffer_and_latches_error(void) {
  s_ctx.flush_result=ESY_ERR_IO; s_ctx.timer_hook=flush_and_stop;
  create(); launch(); run();
  TEST_ASSERT_EQUAL(ESY_ERR_IO,status().last_error);
}
static void timeout_then_complete(void) {
  if (s_ctx.waits==1u) fake_esy_hal_system_advance_tick(s_ctx.config.flush_timeout_ms);
  else {
    TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT,status().last_error);
    fake_esy_os_task_set_current(NULL);
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY,esy_lvgl_destroy(s_ctx.handle));
    fake_esy_os_task_set_current(s_ctx.task->task_handle);
    complete_transport();
  }
}
void test_timeout_retains_transport_loan_until_actual_completion(void) {
  s_ctx.timer_hook=flush_and_stop; s_ctx.wait_hook=timeout_then_complete;
  create(); launch(); run();
  TEST_ASSERT_EQUAL(ESY_LVGL_FAILED,status().state);
}
static void flush_without_waiting(void) {
  TEST_ASSERT_EQUAL_UINT(1u,fake_lvgl_get_state()->timer_handler_count);
  lv_area_t area={.x1=0,.y1=0,.x2=1,.y2=0};
  fake_lvgl_invoke_flush(fake_lvgl_get_state()->last_display,&area,s_ctx.draw.bytes);
}
void test_idle_pending_flush_times_out_without_another_render(void) {
  s_ctx.timer_hook=flush_without_waiting; s_ctx.wait_hook=timeout_then_complete;
  create(); launch(); run();
  TEST_ASSERT_EQUAL(ESY_LVGL_FAILED,status().state);
  TEST_ASSERT_EQUAL_UINT(1u,s_ctx.submitted);
  TEST_ASSERT_EQUAL_UINT(1u,fake_lvgl_get_state()->flush_ready_count);
}
static void invalid_area(void) {
  lv_area_t area={.x1=-1,.y1=0,.x2=1,.y2=0};
  fake_lvgl_invoke_flush(fake_lvgl_get_state()->last_display,&area,s_ctx.draw.bytes);
}
void test_invalid_flush_area_fails_without_transport_access(void) {
  s_ctx.timer_hook=invalid_area; create(); launch(); run();
  TEST_ASSERT_EQUAL_UINT(0u,s_ctx.submitted);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,status().last_error);
}
static void touch_and_stop(void) {
  lv_indev_data_t data={0};
  fake_lvgl_invoke_input_read(fake_lvgl_get_state()->last_indev,&data);
  TEST_ASSERT_EQUAL(LV_INDEV_STATE_PRESSED,data.state);
  TEST_ASSERT_EQUAL_INT(3,data.point.x); TEST_ASSERT_EQUAL_INT(4,data.point.y);
  s_ctx.touch_result=ESY_ERR_IO;
  fake_lvgl_invoke_input_read(fake_lvgl_get_state()->last_indev,&data);
  TEST_ASSERT_EQUAL(LV_INDEV_STATE_RELEASED,data.state);
  stop_request();
}
void test_pointer_provider_and_failure_map_to_pointer_state(void) {
  s_ctx.config.pointer_input=&s_ctx.input; s_ctx.timer_hook=touch_and_stop;
  create();
  s_ctx.input.read=NULL; s_ctx.input.arg=NULL;
  launch(); run();
  TEST_ASSERT_EQUAL_UINT(1u,fake_lvgl_get_state()->indev_delete_count);
}
void test_invalid_api_calls_and_unready_submission(void) {
  esy_lvgl_event_t e=event(); esy_lvgl_status_t st;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,esy_lvgl_start(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,esy_lvgl_request_stop(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,esy_lvgl_get_status(NULL,&st));
  TEST_ASSERT_EQUAL(ESY_OK,esy_lvgl_destroy(NULL));
  create();
  TEST_ASSERT_EQUAL(ESY_ERR_STATE,esy_lvgl_send_event(s_ctx.handle,&e));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,esy_lvgl_get_status(s_ctx.handle,NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,esy_lvgl_send_event(NULL,&e));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,esy_lvgl_send_event(s_ctx.handle,NULL));
  e.complete=NULL; bool yield=true;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG,esy_lvgl_send_event_from_isr(s_ctx.handle,&e,&yield));
  TEST_ASSERT_FALSE(yield);
}
