/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/defs/coordination/esy_atomic_defs.h"
#include "esy_middleware/lvgl/esy_lvgl.h"
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include "lvgl.h"
#include "esy_middleware/lcd/esy_lcd.h"
#include "esy_hal/esy_hal_system.h"

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 6 || LVGL_VERSION_PATCH != 0
#error "esy_lvgl requires the verified LVGL 9.6.0 provider."
#endif
#if LV_USE_OS != LV_OS_NONE || LV_USE_STDLIB_MALLOC != LV_STDLIB_BUILTIN || LV_MEM_ADR != 0
#error "esy_lvgl requires single-owner LVGL with a fixed built-in memory pool."
#endif
#if LV_COLOR_DEPTH != 16 || LV_DRAW_BUF_STRIDE_ALIGN != 1 || LV_USE_DRAW_SW != 1 || \
    LV_DRAW_SW_DRAW_UNIT_CNT != 1
#error "esy_lvgl requires RGB565, packed stride and one software draw unit."
#endif

#define LVGL_MAGIC 0x4C56474Cu
#define LVGL_CLOSED (UINT_MAX / 2u + 1u)
#define LVGL_PUBLISHERS (LVGL_CLOSED - 1u)
#define LVGL_PIXEL_BYTES 2u

static const uint32_t k_wait_ms = 20u;
static const uint32_t k_event_budget = 8u;
static const uint32_t k_flush_timeout_ms = 1000u;

typedef struct {
  uint32_t magic;
  esy_lvgl_config_t config;
  esy_lvgl_pointer_input_t pointer_input;
  esy_lvgl_static_resources_t resources;
  esy_os_task_t *task;
  esy_os_queue_t *queue;
  lv_display_t *display;
  lv_indev_t *indev;
  /* MPSC task/ISR submitters retain a reference until after queue publication
   * and wakeup. CLOSED forbids new references; the GUI drains old publishers. */
  atomic_uint publishers;
  atomic_uint state;
  atomic_int error;
  atomic_uint task_published;
  /* Transport task publishes done as its LAST context access. Only the GUI
   * touches pixels, pending and LVGL. OS notification alone is not retirement. */
  atomic_uint flush_done;
  uint8_t *pixels;
  uint32_t pixel_bytes;
  uint32_t flush_started;
  bool pending;
  bool ui_entered;
} esy_lvgl_ctx_t;

typedef struct {
  esy_lvgl_ctx_t *active; /* Externally serialized lifecycle owner. */
  bool core_initialized; /* GUI-only; core retained across stopped instances. */
} esy_lvgl_module_ctx_t;
static esy_lvgl_module_ctx_t s_ctx;

_Static_assert(sizeof(esy_lvgl_ctx_t) <= ESY_LVGL_PRIV_BYTES, "Context capacity");
_Static_assert(_Alignof(esy_lvgl_t) >= _Alignof(esy_lvgl_ctx_t), "Context alignment");

static esy_lvgl_ctx_t *context(esy_lvgl_t *handle) {
  return handle ? (esy_lvgl_ctx_t *)(void *)handle->priv.bytes : NULL;
}

static bool stopping(esy_lvgl_ctx_t *ctx) {
  return (atomic_load_explicit(&ctx->publishers, memory_order_acquire) & LVGL_CLOSED) != 0u;
}

static void close_submission(esy_lvgl_ctx_t *ctx) {
  atomic_fetch_or_explicit(&ctx->publishers, LVGL_CLOSED, memory_order_acq_rel);
  unsigned state = atomic_load_explicit(&ctx->state, memory_order_acquire);
  while (state == ESY_LVGL_RUNNING || state == ESY_LVGL_STARTING) {
    if (atomic_compare_exchange_weak_explicit(&ctx->state, &state, ESY_LVGL_STOPPING,
                                              memory_order_release, memory_order_relaxed)) break;
  }
}

static void fail_runtime(esy_lvgl_ctx_t *ctx, esy_err_t error) {
  int expected = ESY_OK;
  (void)atomic_compare_exchange_strong_explicit(&ctx->error, &expected, error,
                                               memory_order_release, memory_order_relaxed);
  close_submission(ctx);
}

static void convert_pixels(uint8_t *pixels, uint32_t bytes, bool to_wire,
                           esy_lvgl_rgb565_byte_order_t order) {
  const uint32_t high = order == ESY_LVGL_RGB565_MSB_FIRST ? 0u : 1u;
  const uint32_t low = high ^ 1u;
  for (uint32_t i = 0u; i < bytes; i += LVGL_PIXEL_BYTES) {
    uint16_t pixel;
    if (to_wire) {
      memcpy(&pixel, pixels + i, sizeof(pixel));
      pixels[i + high] = (uint8_t)(pixel >> 8);
      pixels[i + low] = (uint8_t)pixel;
    } else {
      pixel = (uint16_t)((uint16_t)pixels[i + high] << 8) | pixels[i + low];
      memcpy(pixels + i, &pixel, sizeof(pixel));
    }
  }
}

static void transport_complete(void *arg) {
  esy_lvgl_ctx_t *ctx = arg;
  (void)esy_os_task_notify_give(ctx->task);
  /* Wake before publishing retirement: no callback accesses after this store.
   * A bounded wait also covers preemption between notification and publication. */
  atomic_store_explicit(&ctx->flush_done, 1u, memory_order_release);
}

static void finish_flush(esy_lvgl_ctx_t *ctx) {
  if (ctx->pending && atomic_load_explicit(&ctx->flush_done, memory_order_acquire)) {
    convert_pixels(ctx->pixels, ctx->pixel_bytes, false, ctx->config.rgb565_byte_order);
    ctx->pending = false;
    lv_display_flush_ready(ctx->display);
  } else if (ctx->pending &&
      (uint32_t)(esy_hal_system_get_tick() - ctx->flush_started) >= ctx->config.flush_timeout_ms) {
    fail_runtime(ctx, ESY_ERR_TIMEOUT);
  }
}

static void wait_flush(esy_lvgl_ctx_t *ctx) {
  while (ctx->pending) {
    finish_flush(ctx);
    if (!ctx->pending) break;
    /* Timeout closes admission but cannot revoke the transport's buffer loan. */
    (void)esy_os_task_notify_take(true, k_wait_ms);
    finish_flush(ctx);
    /* UI producers can continuously notify this task. Give a lower-priority
     * transport worker CPU time even when notifications never become empty. */
    if (ctx->pending) esy_os_task_delay_ms(1u);
  }
}

static void flush_wait(lv_display_t *display) {
  wait_flush(lv_display_get_driver_data(display));
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
  esy_lvgl_ctx_t *ctx = lv_display_get_driver_data(display);
  if (stopping(ctx)) {
    lv_display_flush_ready(display);
    return;
  }
  if (!area || !pixels || area->x1 < 0 || area->y1 < 0 ||
      area->x2 < area->x1 || area->y2 < area->y1 ||
      area->x2 >= ctx->config.width || area->y2 >= ctx->config.height) {
    fail_runtime(ctx, ESY_ERR_INVALID_ARG);
    lv_display_flush_ready(display);
    return;
  }
  uint32_t count = (uint32_t)(area->x2 - area->x1 + 1) *
                   (uint32_t)(area->y2 - area->y1 + 1);
  if (count > ctx->resources.draw_buffer_size / LVGL_PIXEL_BYTES) {
    fail_runtime(ctx, ESY_ERR_INVALID_ARG);
    lv_display_flush_ready(display);
    return;
  }
  wait_flush(ctx);
  if (stopping(ctx)) {
    lv_display_flush_ready(display);
    return;
  }
  ctx->pixels = pixels;
  ctx->pixel_bytes = count * LVGL_PIXEL_BYTES;
  ctx->flush_started = esy_hal_system_get_tick();
  ctx->pending = true;
  atomic_store_explicit(&ctx->flush_done, 0u, memory_order_relaxed);
  convert_pixels(pixels, ctx->pixel_bytes, true, ctx->config.rgb565_byte_order);
  esy_err_t result = esy_lcd_flush(ctx->config.lcd, (uint16_t)area->x1,
      (uint16_t)area->y1, (uint16_t)area->x2, (uint16_t)area->y2,
      pixels, transport_complete, ctx);
  if (result != ESY_OK) {
    /* LCD rejection guarantees no callback or retained buffer. */
    atomic_store_explicit(&ctx->flush_done, 1u, memory_order_release);
    fail_runtime(ctx, result);
  }
  finish_flush(ctx);
}

static void read_input(lv_indev_t *indev, lv_indev_data_t *data) {
  esy_lvgl_ctx_t *ctx = lv_indev_get_driver_data(indev);
  uint16_t x = 0u, y = 0u;
  bool pressed = false;
  data->state = LV_INDEV_STATE_RELEASED;
  if (!stopping(ctx) && ctx->pointer_input.read(&x, &y, &pressed, ctx->pointer_input.arg) == ESY_OK && pressed) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
}

static esy_err_t setup_gui(esy_lvgl_ctx_t *ctx) {
  if (!s_ctx.core_initialized) {
    lv_init();
    lv_tick_set_cb(esy_hal_system_get_tick);
    s_ctx.core_initialized = true;
  }
  ctx->display = lv_display_create(ctx->config.width, ctx->config.height);
  if (!ctx->display) return ESY_ERR_NO_MEM;
  lv_display_set_color_format(ctx->display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_default(ctx->display);
  lv_display_set_buffers(ctx->display, ctx->resources.draw_buffer1,
      ctx->resources.draw_buffer2, ctx->resources.draw_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_driver_data(ctx->display, ctx);
  lv_display_set_flush_cb(ctx->display, flush);
  lv_display_set_flush_wait_cb(ctx->display, flush_wait);
  if (ctx->config.pointer_input) {
    ctx->indev = lv_indev_create();
    if (!ctx->indev) return ESY_ERR_NO_MEM;
    lv_indev_set_type(ctx->indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_driver_data(ctx->indev, ctx);
    lv_indev_set_read_cb(ctx->indev, read_input);
  }
  if (ctx->config.on_start) {
    ctx->ui_entered = true;
    return ctx->config.on_start(ctx->config.ui_arg);
  }
  return ESY_OK;
}

/* Returns true only after observing an empty queue. Stop waits for publishers
 * before using that observation as permission to reclaim accepted requests. */
static bool dispatch(esy_lvgl_ctx_t *ctx) {
  esy_lvgl_event_t event;
  for (uint32_t i = 0u; i < ctx->config.event_budget; ++i) {
    if (esy_os_queue_receive(ctx->queue, &event, 0u) != ESY_OK) return true;
    esy_lvgl_event_result_t result = ESY_LVGL_EVENT_CANCELLED;
    if (!stopping(ctx)) {
      event.callback(event.arg);
      result = ESY_LVGL_EVENT_EXECUTED;
    }
    event.complete(result, event.arg);
  }
  return false;
}

static void gui_task(void *arg) {
  esy_lvgl_ctx_t *ctx = arg;
  /* Static task creation can schedule this entry before publishing its handle. */
  while (!atomic_load_explicit(&ctx->task_published, memory_order_acquire))
    esy_os_task_delay_ms(1u);
  if (!stopping(ctx)) {
    esy_err_t result = setup_gui(ctx);
    if (result != ESY_OK) fail_runtime(ctx, result);
    else {
      unsigned expected = ESY_LVGL_STARTING;
      (void)atomic_compare_exchange_strong_explicit(&ctx->state, &expected, ESY_LVGL_RUNNING,
                                                   memory_order_release, memory_order_relaxed);
    }
  }
  while (!stopping(ctx)) {
    finish_flush(ctx);
    bool empty = dispatch(ctx);
    if (stopping(ctx)) break;
    /* Compute the deadline after UI requests have changed LVGL timers. */
    uint32_t delay = lv_timer_handler();
    if (!empty) {
      /* Bound CPU ownership as well as rendering starvation under event floods. */
      esy_os_task_delay_ms(1u);
    } else {
      if (delay == 0u) delay = 1u;
      if (delay > k_wait_ms) delay = k_wait_ms;
      (void)esy_os_task_notify_take(true, delay);
    }
  }
  /* In-progress submitters may still commit an accepted request. Wait without
   * starving a preempted producer; admission has already been closed. */
  while (atomic_load_explicit(&ctx->publishers, memory_order_acquire) & LVGL_PUBLISHERS)
    (void)esy_os_task_notify_take(true, 1u);
  while (!dispatch(ctx)) (void)esy_os_task_notify_take(true, 1u);
  wait_flush(ctx);
  if (ctx->ui_entered) ctx->config.on_stop(ctx->config.ui_arg);
  /* on_stop may request a final synchronous refresh; the closed gate rejects it. */
  wait_flush(ctx);
  if (ctx->indev) lv_indev_delete(ctx->indev);
  if (ctx->display) lv_display_delete(ctx->display);
  ctx->indev = NULL;
  ctx->display = NULL;
  unsigned terminal = atomic_load_explicit(&ctx->error, memory_order_acquire) == ESY_OK
      ? ESY_LVGL_STOPPED : ESY_LVGL_FAILED;
  atomic_store_explicit(&ctx->state, terminal, memory_order_release);
  /* State publication precedes actual parking; destroy also checks the OS. */
  for (;;) (void)esy_os_task_park();
}

static esy_err_t validate(const esy_lvgl_config_t *config) {
  if (!config || !config->static_resources || !config->lcd ||
      !config->width || !config->height || !config->event_budget ||
      !config->flush_timeout_ms || config->flush_timeout_ms > INT32_MAX ||
      (!!config->on_start != !!config->on_stop)) return ESY_ERR_INVALID_ARG;
  if (config->rgb565_byte_order != ESY_LVGL_RGB565_MSB_FIRST &&
      config->rgb565_byte_order != ESY_LVGL_RGB565_LSB_FIRST)
    return ESY_ERR_INVALID_ARG;
  const esy_lvgl_static_resources_t *r = config->static_resources;
  if (!r->context || !r->task_stack_buffer || !r->task_stack_buffer_size ||
      !r->task_control_storage || !r->event_queue_buffer || !r->event_queue_depth ||
      !r->event_queue_control_storage || !r->draw_buffer1 ||
      r->draw_buffer_size < (uint32_t)config->width * LVGL_PIXEL_BYTES ||
      (uintptr_t)r->task_stack_buffer % ESY_OS_TASK_STACK_ALIGN ||
      (uintptr_t)r->draw_buffer1 % LV_DRAW_BUF_ALIGN ||
      (r->draw_buffer2 && (uintptr_t)r->draw_buffer2 % LV_DRAW_BUF_ALIGN))
    return ESY_ERR_INVALID_ARG;
  if (r->draw_buffer2) {
    uintptr_t a = (uintptr_t)r->draw_buffer1, b = (uintptr_t)r->draw_buffer2;
    if ((a > b ? a - b : b - a) < r->draw_buffer_size) return ESY_ERR_INVALID_ARG;
  }
  if (config->pointer_input && !config->pointer_input->read) return ESY_ERR_INVALID_ARG;
  if (!esy_lcd_is_attached(config->lcd)) return ESY_ERR_STATE;
  if (!esy_os_task_park_supported()) return ESY_ERR_NOT_SUPPORTED;
  return ESY_OK;
}

void esy_lvgl_default_config(esy_lvgl_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->task_core_affinity = ESY_CORE_ALL;
  config->event_budget = k_event_budget;
  config->flush_timeout_ms = k_flush_timeout_ms;
}

esy_err_t esy_lvgl_create(const esy_lvgl_config_t *config, esy_lvgl_t **out_lvgl) {
  if (!out_lvgl) return ESY_ERR_INVALID_ARG;
  esy_err_t result = validate(config);
  if (result != ESY_OK) { *out_lvgl = NULL; return result; }
  esy_lvgl_ctx_t *ctx = context(config->static_resources->context);
  /* The public first-use {0} requirement makes this state read defined. */
  if (s_ctx.active || ctx->magic) return ESY_ERR_STATE;
  *out_lvgl = NULL;
  memset(ctx, 0, sizeof(*ctx));
  atomic_init(&ctx->publishers, 0u);
  atomic_init(&ctx->state, ESY_LVGL_INITIALIZED);
  atomic_init(&ctx->error, ESY_OK);
  atomic_init(&ctx->task_published, 0u);
  atomic_init(&ctx->flush_done, 0u);
  ctx->resources = *config->static_resources;
  ctx->config = *config;
  if (config->pointer_input) {
    ctx->pointer_input = *config->pointer_input;
    ctx->config.pointer_input = &ctx->pointer_input;
  }
  ctx->config.static_resources = &ctx->resources;
  result = esy_os_queue_create_static(sizeof(esy_lvgl_event_t), ctx->resources.event_queue_depth,
      ctx->resources.event_queue_buffer, ctx->resources.event_queue_control_storage, &ctx->queue);
  if (result != ESY_OK) { memset(ctx, 0, sizeof(*ctx)); return result; }
  ctx->magic = LVGL_MAGIC;
  s_ctx.active = ctx;
  *out_lvgl = ctx->resources.context;
  return ESY_OK;
}

esy_err_t esy_lvgl_start(esy_lvgl_t *lvgl) {
  esy_lvgl_ctx_t *ctx = context(lvgl);
  if (!ctx || ctx->magic != LVGL_MAGIC) return ESY_ERR_INVALID_ARG;
  if (atomic_load_explicit(&ctx->state, memory_order_acquire) != ESY_LVGL_INITIALIZED)
    return ESY_ERR_STATE;
  atomic_store_explicit(&ctx->state, ESY_LVGL_STARTING, memory_order_release);
  esy_err_t result = esy_os_task_create_static("lvgl", gui_task, ctx,
      ctx->resources.task_stack_buffer_size, ctx->config.task_priority,
      ctx->resources.task_stack_buffer, ctx->resources.task_control_storage, &ctx->task);
  if (result != ESY_OK) {
    atomic_store_explicit(&ctx->state, ESY_LVGL_INITIALIZED, memory_order_release);
    return result;
  }
  esy_os_task_set_core_affinity(ctx->task, ctx->config.task_core_affinity);
  atomic_store_explicit(&ctx->task_published, 1u, memory_order_release);
  return ESY_OK;
}

esy_err_t esy_lvgl_request_stop(esy_lvgl_t *lvgl) {
  esy_lvgl_ctx_t *ctx = context(lvgl);
  if (!ctx || ctx->magic != LVGL_MAGIC) return ESY_ERR_INVALID_ARG;
  close_submission(ctx);
  if (!ctx->task) atomic_store_explicit(&ctx->state, ESY_LVGL_STOPPED, memory_order_release);
  else (void)esy_os_task_notify_give(ctx->task);
  return ESY_OK;
}

esy_err_t esy_lvgl_get_status(esy_lvgl_t *lvgl, esy_lvgl_status_t *status) {
  esy_lvgl_ctx_t *ctx = context(lvgl);
  if (!ctx || ctx->magic != LVGL_MAGIC || !status) return ESY_ERR_INVALID_ARG;
  status->state = (esy_lvgl_state_t)atomic_load_explicit(&ctx->state, memory_order_acquire);
  status->last_error = atomic_load_explicit(&ctx->error, memory_order_acquire);
  return ESY_OK;
}

esy_err_t esy_lvgl_destroy(esy_lvgl_t *lvgl) {
  esy_lvgl_ctx_t *ctx = context(lvgl);
  if (!ctx || !ctx->magic) return ESY_OK;
  if (ctx->magic != LVGL_MAGIC) return ESY_ERR_INVALID_ARG;
  if (ctx->task) {
    if (esy_os_task_is_current(ctx->task)) return ESY_ERR_STATE;
    unsigned state = atomic_load_explicit(&ctx->state, memory_order_acquire);
    if (state != ESY_LVGL_STOPPED && state != ESY_LVGL_FAILED) return ESY_ERR_BUSY;
    esy_err_t result = esy_os_task_delete_parked(ctx->task);
    if (result != ESY_OK) return result;
  }
  esy_os_queue_delete(ctx->queue);
  s_ctx.active = NULL;
  memset(ctx, 0, sizeof(*ctx));
  return ESY_OK;
}

static esy_err_t submit(esy_lvgl_t *lvgl, const esy_lvgl_event_t *event,
                        bool from_isr, bool *need_yield) {
  if (need_yield) *need_yield = false;
  esy_lvgl_ctx_t *ctx = context(lvgl);
  if (!ctx || ctx->magic != LVGL_MAGIC || !event || !event->callback || !event->complete)
    return ESY_ERR_INVALID_ARG;
  unsigned refs = atomic_load_explicit(&ctx->publishers, memory_order_acquire);
  do {
    if (refs & LVGL_CLOSED) return ESY_ERR_STATE;
    if (refs == LVGL_PUBLISHERS) return ESY_ERR_BUSY;
  } while (!atomic_compare_exchange_weak_explicit(&ctx->publishers, &refs, refs + 1u,
                                                 memory_order_acquire, memory_order_relaxed));
  esy_err_t result = ESY_ERR_STATE;
  if (atomic_load_explicit(&ctx->state, memory_order_acquire) == ESY_LVGL_RUNNING) {
    bool queue_yield = false, task_yield = false;
    result = from_isr ? esy_os_queue_send_from_isr(ctx->queue, event, &queue_yield)
                      : esy_os_queue_send(ctx->queue, event, 0u);
    if (result == ESY_OK) {
      if (from_isr) (void)esy_os_task_notify_give_from_isr(ctx->task, &task_yield);
      else (void)esy_os_task_notify_give(ctx->task);
    }
    if (need_yield) *need_yield = queue_yield || task_yield;
  }
  atomic_fetch_sub_explicit(&ctx->publishers, 1u, memory_order_release);
  return result;
}

esy_err_t esy_lvgl_send_event(esy_lvgl_t *lvgl, const esy_lvgl_event_t *event) {
  return submit(lvgl, event, false, NULL);
}

esy_err_t esy_lvgl_send_event_from_isr(esy_lvgl_t *lvgl, const esy_lvgl_event_t *event,
                                     bool *need_yield) {
  return submit(lvgl, event, true, need_yield);
}
