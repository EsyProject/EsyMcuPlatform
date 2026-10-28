/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_timer.h"

#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esy_os_freertos_internal.h"
#ifdef ESY_PLATFORM_ESP32
#include "freertos/timers.h"
#else
#include "timers.h"
#endif

#if INCLUDE_xTimerPendFunctionCall != 1
#error "esy_os_timer requires INCLUDE_xTimerPendFunctionCall=1 for cleanup fences"
#endif

enum { TIMER_IDLE, TIMER_STOPPING, TIMER_DELETING };

/* One external control owner; the daemon only publishes fence completion. */
struct esy_os_timer_s {
  TimerHandle_t handle;

  esy_os_timer_callback_t callback;  /* User callback */
  void *arg;                         /* User argument */
  unsigned operation;
  bool fence_queued;
  atomic_bool fence_complete;
};

/* Internal layout placed inside opaque storage bytes */
struct esy_os_timer_static_impl_s {
  StaticTimer_t native;              /* FreeRTOS static timer control block */
  struct esy_os_timer_s wrapper;     /* Public opaque wrapper */
};

_Static_assert(_Alignof(esy_os_timer_static_storage_t) >=
                   _Alignof(struct esy_os_timer_static_impl_s),
               "Native control block alignment exceeds public storage");

/* Compile-time guard: public storage must be large enough */
_Static_assert(ESY_OS_TIMER_STATIC_STORAGE_BYTES >= sizeof(struct esy_os_timer_static_impl_s),
               "ESY_OS_TIMER_STATIC_STORAGE_BYTES too small");

/* FreeRTOS callback trampoline -> dispatch to user callback */
static void esy_os_timer_callback_trampoline_(TimerHandle_t xTimer) {
  esy_os_timer_t *t = (esy_os_timer_t *)pvTimerGetTimerID(xTimer);
  if (t && t->callback) {
    t->callback(t->arg);
  }
}

esy_err_t esy_os_timer_create_static(const char *name,
                                     uint32_t period_ms,
                                     bool auto_reload,
                                     void *arg,
                                     esy_os_timer_callback_t callback,
                                     esy_os_timer_static_storage_t *storage,
                                     esy_os_timer_t **out_timer) {
  if (out_timer) *out_timer = NULL;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
  if (!callback || !storage || !out_timer || period_ms == 0u) return ESY_ERR_INVALID_ARG;

  /* Alignment guard for opaque storage */
  if (((uintptr_t)storage->bytes % ESY_OS_TIMER_STATIC_STORAGE_ALIGN) != 0) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Reinterpret opaque bytes as internal static object */
  struct esy_os_timer_static_impl_s *impl =
      (struct esy_os_timer_static_impl_s *)(void *)storage->bytes;

  memset(impl, 0, sizeof(*impl));

  atomic_init(&impl->wrapper.fence_complete, false);
  impl->wrapper.callback  = callback;
  impl->wrapper.arg       = arg;

  TickType_t ticks = esy_os_finite_ticks(period_ms);
  if (ticks == 0) ticks = 1;  /* Ensure at least 1 tick */

  /* Pass wrapper pointer as TimerID so trampoline can dispatch */
  TimerHandle_t h = xTimerCreateStatic(name ? name : "esy_timer",
                                       ticks,
                                       auto_reload ? pdTRUE : pdFALSE,
                                       (void *)&impl->wrapper,
                                       esy_os_timer_callback_trampoline_,
                                       &impl->native);
  if (!h) return ESY_ERR_FAIL;

  impl->wrapper.handle = h;
  *out_timer = &impl->wrapper;
  return ESY_OK;
#else
  (void)name; (void)period_ms; (void)auto_reload; (void)arg;
  (void)callback; (void)storage; (void)out_timer;
  return ESY_ERR_NOT_SUPPORTED;
#endif
}

/* This is the last access by the daemon: acquire by the polling owner makes
 * all preceding timer commands/callbacks complete before storage can be reused. */
static void timer_cleanup_fence(void *arg, uint32_t unused) {
  (void)unused;
  esy_os_timer_t *timer = arg;
  atomic_store_explicit(&timer->fence_complete, true, memory_order_release);
}

static esy_err_t timer_cleanup(esy_os_timer_t *timer, unsigned operation) {
  if (!timer) return ESY_ERR_INVALID_ARG;
  if (!timer->handle) return ESY_OK;
  if (timer->operation != TIMER_IDLE && timer->operation != operation)
    return ESY_ERR_STATE;
  if (timer->operation == TIMER_IDLE) {
    atomic_store_explicit(&timer->fence_complete, false, memory_order_relaxed);
    BaseType_t sent = operation == TIMER_DELETING
                         ? xTimerDelete(timer->handle, 0)
                         : xTimerStop(timer->handle, 0);
    if (sent != pdPASS) return ESY_ERR_BUSY;
    timer->operation = operation;
    timer->fence_queued = false;
  }
  if (!timer->fence_queued) {
    if (xTimerPendFunctionCall(timer_cleanup_fence, timer, 0u, 0) != pdPASS)
      return ESY_ERR_BUSY;
    timer->fence_queued = true;
  }
  if (!atomic_load_explicit(&timer->fence_complete, memory_order_acquire))
    return ESY_ERR_BUSY;
  if (operation == TIMER_DELETING) {
    timer->handle = NULL;
    timer->callback = NULL;
    timer->arg = NULL;
  }
  timer->operation = TIMER_IDLE;
  return ESY_OK;
}

esy_err_t esy_os_timer_delete(esy_os_timer_t *timer) {
  return timer_cleanup(timer, TIMER_DELETING);
}

esy_err_t esy_os_timer_start(esy_os_timer_t *timer) {
  if (!timer || !timer->handle) return ESY_ERR_INVALID_ARG;
  if (timer->operation != TIMER_IDLE) return ESY_ERR_BUSY;
  return xTimerStart(timer->handle, 0) == pdPASS ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_os_timer_stop(esy_os_timer_t *timer) {
  return timer_cleanup(timer, TIMER_STOPPING);
}
