/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/os/esy_os_work.h"
#include <string.h>
#ifdef ESY_PLATFORM_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include "FreeRTOS.h"
#include "task.h"
#endif
#if INCLUDE_vTaskSuspend != 1 || INCLUDE_eTaskGetState != 1
#error "esy_os_work requires INCLUDE_vTaskSuspend=1 and INCLUDE_eTaskGetState=1"
#endif

/* All work/queue/delayed state is shared by tasks and ISR submitters. A short
 * OS critical section protects multi-field admission and completion transitions;
 * per-field atomics cannot make these transactions indivisible. No blocking
 * operation or user callback runs under this lock. Lifecycle owners serialize
 * create/destroy and delayed-work control calls outside this lock. */
enum { WF_PENDING = 1u, WF_RUNNING = 2u, WF_CANCELLED = 4u };

struct esy_os_work_s {
  esy_os_work_handler_t handler;
  void *ctx;
  uint32_t flags;
  uint32_t waiters;
  struct esy_os_work_queue_s *bound_wq;
};

struct esy_os_work_queue_s {
  esy_os_task_t *task;
  esy_os_queue_t *queue;
  uint32_t submitters;
  uint32_t delayed_bindings;
  bool stopped;
  bool exited;
};

struct esy_os_delayed_work_s {
  esy_os_work_t *base;
  esy_os_work_queue_t *wq;
  esy_os_timer_t *timer;
  esy_os_timer_static_storage_t *timer_storage;
  bool active;
  bool cancelling;
  esy_err_t result;
};

_Static_assert(_Alignof(esy_os_work_static_storage_t) >= _Alignof(esy_os_work_t),
               "Work alignment is insufficient");
_Static_assert(_Alignof(esy_os_work_queue_static_storage_t) >= _Alignof(esy_os_work_queue_t),
               "Queue alignment is insufficient");
_Static_assert(_Alignof(esy_os_delayed_work_static_storage_t) >= _Alignof(esy_os_delayed_work_t),
               "Delayed work alignment is insufficient");
_Static_assert(sizeof(esy_os_work_t) <= ESY_OS_WORK_STATIC_STORAGE_BYTES,
               "Work storage is too small");
_Static_assert(sizeof(esy_os_work_queue_t) <= ESY_OS_WORK_QUEUE_INTERNAL_STORAGE_BYTES,
               "Work queue storage is too small");
_Static_assert(sizeof(esy_os_delayed_work_t) <= ESY_OS_DELAYED_WORK_INTERNAL_STORAGE_BYTES,
               "Delayed work storage is too small");
_Static_assert(ESY_OS_TASK_STATIC_STORAGE_BYTES % ESY_OS_QUEUE_STATIC_STORAGE_ALIGN == 0,
               "Packed queue storage must remain aligned");
_Static_assert(ESY_OS_WORK_STATIC_STORAGE_BYTES % ESY_OS_TIMER_STATIC_STORAGE_ALIGN == 0,
               "Packed timer storage must remain aligned");

static void workqueue_task_entry(void *arg) {
  esy_os_work_queue_t *wq = arg;
  for (;;) {
    esy_os_work_t *work = NULL;
    if (esy_os_queue_receive(wq->queue, &work, UINT32_MAX) != ESY_OK) continue;
    uint32_t key = esy_os_task_enter_crit_auto();
    if (!work) {
      wq->exited = true;
      esy_os_task_exit_crit_auto(key);
      /* Park until the lifecycle owner deletes this task. */
      for (;;) vTaskSuspend(NULL);
    }
    work->flags = (work->flags & ~WF_PENDING) | WF_RUNNING;
    bool cancelled = (work->flags & WF_CANCELLED) != 0u;
    esy_os_task_exit_crit_auto(key);
    if (!cancelled) work->handler(work, work->ctx);
    key = esy_os_task_enter_crit_auto();
    work->flags = 0u;
    work->bound_wq = NULL;
    esy_os_task_exit_crit_auto(key);
  }
}

esy_err_t esy_os_work_queue_create_static(
    const char *name, uint32_t prio, uint32_t queue_depth,
    const esy_os_work_queue_resources_t *resources, esy_os_work_queue_t **out_wq) {
  if (out_wq) *out_wq = NULL;
  if (!out_wq || !resources || !resources->storage ||
      !resources->task_stack_buffer || !resources->queue_buffer ||
      resources->task_stack_buffer_size > UINT32_MAX || queue_depth == 0u ||
      queue_depth > resources->queue_buffer_size / sizeof(esy_os_work_t *) ||
      (uintptr_t)resources->storage % ESY_OS_WORK_QUEUE_STATIC_STORAGE_ALIGN != 0u)
    return ESY_ERR_INVALID_ARG;
  uint8_t *p = resources->storage->bytes;
  esy_os_work_queue_t *wq = (void *)p;
  p += ESY_OS_WORK_QUEUE_INTERNAL_STORAGE_BYTES;
  esy_os_task_static_storage_t *task_control_storage = (void *)p;
  p += ESY_OS_TASK_STATIC_STORAGE_BYTES;
  esy_os_queue_static_storage_t *queue_storage = (void *)p;
  memset(wq, 0, sizeof(*wq));
  esy_err_t rc = esy_os_queue_create_static(sizeof(esy_os_work_t *), queue_depth,
      resources->queue_buffer, queue_storage, &wq->queue);
  if (rc != ESY_OK) return rc;
  rc = esy_os_task_create_static(name, workqueue_task_entry, wq,
      (uint32_t)resources->task_stack_buffer_size, prio,
      resources->task_stack_buffer, task_control_storage, &wq->task);
  if (rc != ESY_OK) {
    esy_os_queue_delete(wq->queue);
    wq->queue = NULL;
    return rc;
  }
  *out_wq = wq;
  return ESY_OK;
}

esy_err_t esy_os_work_queue_destroy(esy_os_work_queue_t *wq) {
  if (!wq) return ESY_ERR_INVALID_ARG;
  if (esy_os_task_is_current(wq->task)) return ESY_ERR_STATE;
  uint32_t key = esy_os_task_enter_crit_auto();
  if (wq->delayed_bindings != 0u) {
    esy_os_task_exit_crit_auto(key);
    return ESY_ERR_BUSY;
  }
  wq->stopped = true;
  bool deleted = wq->queue == NULL;
  esy_os_task_exit_crit_auto(key);
  if (deleted) return ESY_OK;
  for (;;) {
    key = esy_os_task_enter_crit_auto();
    bool drained = wq->submitters == 0u;
    esy_os_task_exit_crit_auto(key);
    if (drained) break;
    esy_os_task_delay_ms(1u);
  }
  esy_os_work_t *exit_marker = NULL;
  while (esy_os_queue_send(wq->queue, &exit_marker, 0u) != ESY_OK)
    esy_os_task_delay_ms(1u);
  for (;;) {
    key = esy_os_task_enter_crit_auto();
    bool exited = wq->exited;
    esy_os_task_exit_crit_auto(key);
    if (exited && eTaskGetState(esy_os_task_get_handle(wq->task)) == eSuspended) break;
    esy_os_task_delay_ms(1u);
  }
  esy_os_task_delete(wq->task);
  esy_os_queue_delete(wq->queue);
  key = esy_os_task_enter_crit_auto();
  wq->task = NULL;
  wq->queue = NULL;
  esy_os_task_exit_crit_auto(key);
  return ESY_OK;
}

esy_err_t esy_os_work_queue_set_core_affinity(esy_os_work_queue_t *wq,
                                             uint8_t core_affinity) {
  if (!wq || !wq->task) return ESY_ERR_INVALID_ARG;
  esy_os_task_set_core_affinity(wq->task, core_affinity);
  return ESY_OK;
}

static esy_err_t submit_common(esy_os_work_queue_t *wq, esy_os_work_t *work,
                               bool from_isr, bool *need_yield) {
  if (need_yield) *need_yield = false;
  if (!wq || !work) return ESY_ERR_INVALID_ARG;
  uint32_t key = esy_os_task_enter_crit_auto();
  if (wq->stopped || !wq->queue || work->flags || work->waiters) {
    esy_os_task_exit_crit_auto(key);
    return ESY_ERR_STATE;
  }
  work->flags = WF_PENDING;
  work->bound_wq = wq;
  ++wq->submitters;
  esy_os_task_exit_crit_auto(key);
  esy_err_t rc = from_isr
      ? esy_os_queue_send_from_isr(wq->queue, &work, need_yield)
      : esy_os_queue_send(wq->queue, &work, 0u);
  key = esy_os_task_enter_crit_auto();
  if (rc != ESY_OK) {
    work->flags = 0u;
    work->bound_wq = NULL;
  }
  --wq->submitters;
  esy_os_task_exit_crit_auto(key);
  return rc == ESY_OK ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_os_work_submit(esy_os_work_queue_t *wq, esy_os_work_t *work) {
  return submit_common(wq, work, false, NULL);
}

esy_err_t esy_os_work_submit_from_isr(esy_os_work_queue_t *wq,
                                    esy_os_work_t *work, bool *need_yield) {
  return submit_common(wq, work, true, need_yield);
}

esy_err_t esy_os_work_create_static(esy_os_work_handler_t handler, void *ctx,
    esy_os_work_static_storage_t *storage, esy_os_work_t **out_work) {
  if (out_work) *out_work = NULL;
  if (!handler || !storage || !out_work ||
      (uintptr_t)storage % ESY_OS_WORK_STATIC_STORAGE_ALIGN != 0u)
    return ESY_ERR_INVALID_ARG;
  esy_os_work_t *work = (void *)storage->bytes;
  memset(work, 0, sizeof(*work));
  work->handler = handler;
  work->ctx = ctx;
  *out_work = work;
  return ESY_OK;
}

void esy_os_work_cancel(esy_os_work_t *work) {
  if (!work) return;
  uint32_t key = esy_os_task_enter_crit_auto();
  if (work->flags & WF_PENDING) work->flags |= WF_CANCELLED;
  esy_os_task_exit_crit_auto(key);
}

static esy_err_t wait_for_work(esy_os_work_t *work, bool cancel) {
  if (!work) return ESY_ERR_INVALID_ARG;
  uint32_t key = esy_os_task_enter_crit_auto();
  if (work->bound_wq && esy_os_task_is_current(work->bound_wq->task)) {
    esy_os_task_exit_crit_auto(key);
    return ESY_ERR_STATE;
  }
  ++work->waiters; /* Prevent a new submission from replacing the observed cycle. */
  if (cancel && (work->flags & WF_PENDING)) work->flags |= WF_CANCELLED;
  for (;;) {
    bool idle = work->flags == 0u;
    if (idle) --work->waiters;
    esy_os_task_exit_crit_auto(key);
    if (idle) return ESY_OK;
    esy_os_task_delay_ms(1u);
    key = esy_os_task_enter_crit_auto();
  }
}

esy_err_t esy_os_work_cancel_sync(esy_os_work_t *work) {
  return wait_for_work(work, true);
}

esy_err_t esy_os_work_flush(esy_os_work_t *work) {
  return wait_for_work(work, false);
}

static bool work_has_flag(const esy_os_work_t *work, uint32_t flag) {
  uint32_t key = esy_os_task_enter_crit_auto();
  bool present = work && (work->flags & flag) != 0u;
  esy_os_task_exit_crit_auto(key);
  return present;
}

bool esy_os_work_is_pending(const esy_os_work_t *work) {
  return work_has_flag(work, WF_PENDING);
}

bool esy_os_work_is_running(const esy_os_work_t *work) {
  return work_has_flag(work, WF_RUNNING);
}

static void delayed_timer_cb(void *arg) {
  esy_os_delayed_work_t *dwork = arg;
  uint32_t key = esy_os_task_enter_crit_auto();
  bool submit = !dwork->cancelling;
  esy_os_work_queue_t *wq = dwork->wq;
  esy_os_task_exit_crit_auto(key);
  esy_err_t rc = submit ? esy_os_work_submit(wq, dwork->base) : ESY_ERR_STATE;
  key = esy_os_task_enter_crit_auto();
  dwork->active = false;
  dwork->result = rc;
  esy_os_task_exit_crit_auto(key);
}

esy_err_t esy_os_delayed_work_create_static(esy_os_work_handler_t handler,
    void *ctx, esy_os_delayed_work_static_storage_t *storage,
    esy_os_delayed_work_t **out_dwork) {
  if (out_dwork) *out_dwork = NULL;
  if (!storage || !out_dwork ||
      (uintptr_t)storage % ESY_OS_DELAYED_WORK_STATIC_STORAGE_ALIGN != 0u)
    return ESY_ERR_INVALID_ARG;
  uint8_t *p = storage->bytes;
  esy_os_delayed_work_t *dwork = (void *)p;
  p += ESY_OS_DELAYED_WORK_INTERNAL_STORAGE_BYTES;
  esy_os_work_static_storage_t *work_store = (void *)p;
  p += ESY_OS_WORK_STATIC_STORAGE_BYTES;
  memset(dwork, 0, sizeof(*dwork));
  esy_err_t rc = esy_os_work_create_static(handler, ctx, work_store, &dwork->base);
  if (rc != ESY_OK) return rc;
  dwork->timer_storage = (void *)p;
  dwork->result = ESY_ERR_STATE;
  *out_dwork = dwork;
  return ESY_OK;
}

static esy_err_t retire_delayed_timer(esy_os_delayed_work_t *dwork) {
  if (!dwork->timer) return ESY_OK;
  esy_err_t rc = esy_os_timer_delete(dwork->timer);
  if (rc == ESY_OK) dwork->timer = NULL;
  return rc;
}

esy_err_t esy_os_delayed_work_submit(esy_os_work_queue_t *wq,
    esy_os_delayed_work_t *dwork, uint32_t delay_ms) {
  if (!wq || !dwork) return ESY_ERR_INVALID_ARG;
  uint32_t key = esy_os_task_enter_crit_auto();
  dwork->cancelling = true;
  esy_os_task_exit_crit_auto(key);
  esy_err_t rc = retire_delayed_timer(dwork);
  if (rc != ESY_OK) return rc;
  key = esy_os_task_enter_crit_auto();
  dwork->active = false;
  if (dwork->base->flags || dwork->base->waiters || wq->stopped || !wq->queue) {
    esy_os_task_exit_crit_auto(key);
    return ESY_ERR_STATE;
  }
  if (dwork->wq != wq) {
    if (dwork->wq) --dwork->wq->delayed_bindings;
    ++wq->delayed_bindings;
    dwork->wq = wq;
  }
  dwork->cancelling = false;
  dwork->result = ESY_ERR_BUSY;
  esy_os_task_exit_crit_auto(key);
  if (delay_ms == 0u) {
    rc = esy_os_work_submit(wq, dwork->base);
  } else {
    rc = esy_os_timer_create_static("dwork", delay_ms, false, dwork,
        delayed_timer_cb, dwork->timer_storage, &dwork->timer);
    if (rc == ESY_OK) {
      key = esy_os_task_enter_crit_auto();
      dwork->active = true;
      esy_os_task_exit_crit_auto(key);
      rc = esy_os_timer_start(dwork->timer);
    }
  }
  key = esy_os_task_enter_crit_auto();
  if (rc != ESY_OK || delay_ms == 0u) {
    dwork->active = false;
    dwork->result = rc;
  }
  esy_os_task_exit_crit_auto(key);
  return rc;
}

esy_err_t esy_os_delayed_work_reschedule(esy_os_delayed_work_t *dwork,
                                         uint32_t delay_ms) {
  if (!dwork || !dwork->wq) return ESY_ERR_STATE;
  return esy_os_delayed_work_submit(dwork->wq, dwork, delay_ms);
}

esy_err_t esy_os_delayed_work_cancel(esy_os_delayed_work_t *dwork) {
  if (!dwork) return ESY_ERR_INVALID_ARG;
  uint32_t key = esy_os_task_enter_crit_auto();
  dwork->cancelling = true;
  esy_os_task_exit_crit_auto(key);
  esy_err_t rc = retire_delayed_timer(dwork);
  esy_os_work_cancel(dwork->base);
  key = esy_os_task_enter_crit_auto();
  if (rc == ESY_OK) dwork->active = false;
  if (rc == ESY_OK && (dwork->base->flags || dwork->base->waiters)) rc = ESY_ERR_BUSY;
  if (rc == ESY_OK && dwork->wq) {
    --dwork->wq->delayed_bindings;
    dwork->wq = NULL;
  }
  esy_os_task_exit_crit_auto(key);
  return rc;
}

esy_err_t esy_os_delayed_work_cancel_sync(esy_os_delayed_work_t *dwork) {
  if (!dwork) return ESY_ERR_INVALID_ARG;
  if (dwork->wq && esy_os_task_is_current(dwork->wq->task)) return ESY_ERR_STATE;
  for (;;) {
    esy_err_t rc = esy_os_delayed_work_cancel(dwork);
    if (rc != ESY_ERR_BUSY) return rc;
    esy_os_task_delay_ms(1u);
  }
}

bool esy_os_delayed_work_is_active(const esy_os_delayed_work_t *dwork) {
  uint32_t key = esy_os_task_enter_crit_auto();
  bool active = dwork && dwork->active;
  esy_os_task_exit_crit_auto(key);
  return active;
}

esy_err_t esy_os_delayed_work_last_result(const esy_os_delayed_work_t *dwork) {
  if (!dwork) return ESY_ERR_INVALID_ARG;
  uint32_t key = esy_os_task_enter_crit_auto();
  esy_err_t result = dwork->result;
  esy_os_task_exit_crit_auto(key);
  return result;
}
