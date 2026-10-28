/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_os_timer.h"

#include <string.h>

struct esy_os_timer_s {
  uint32_t slot;
};

typedef struct {
  fake_esy_os_timer_entry_t table[FAKE_ESY_OS_TIMER_MAX_TIMERS];
  esy_os_timer_t handles[FAKE_ESY_OS_TIMER_MAX_TIMERS];
  fake_esy_os_timer_entry_t *last_created;
  fake_esy_os_timer_entry_t *last_deleted;
  esy_err_t create_static_result;
  esy_err_t start_result;
  uint32_t create_static_count;
  uint32_t delete_count;

  esy_err_t cleanup_result;
} fake_esy_os_timer_ctx_t;
static fake_esy_os_timer_ctx_t s_ctx;

static fake_esy_os_timer_entry_t *fake_esy_os_timer_alloc_slot(void) {
  for (uint32_t i = 0u; i < FAKE_ESY_OS_TIMER_MAX_TIMERS; i++) {
    if (!s_ctx.table[i].used) {
      memset(&s_ctx.table[i], 0, sizeof(s_ctx.table[i]));
      s_ctx.handles[i].slot = i;
      s_ctx.table[i].used = true;
      s_ctx.table[i].timer_handle = &s_ctx.handles[i];
      return &s_ctx.table[i];
    }
  }

  return NULL;
}

static fake_esy_os_timer_entry_t *fake_esy_os_timer_entry_from_handle(
    esy_os_timer_t *timer) {
  if (timer == NULL || timer->slot >= FAKE_ESY_OS_TIMER_MAX_TIMERS) {
    return NULL;
  }

  fake_esy_os_timer_entry_t *entry = &s_ctx.table[timer->slot];
  if (entry->timer_handle != timer) {
    return NULL;
  }

  return entry;
}

fake_esy_os_timer_entry_t *fake_esy_os_timer_get_table(void) {
  return s_ctx.table;
}

uint32_t fake_esy_os_timer_get_table_size(void) {
  return FAKE_ESY_OS_TIMER_MAX_TIMERS;
}

fake_esy_os_timer_entry_t *fake_esy_os_timer_find(esy_os_timer_t *timer) {
  return fake_esy_os_timer_entry_from_handle(timer);
}

fake_esy_os_timer_entry_t *fake_esy_os_timer_get_last_created(void) {
  return s_ctx.last_created;
}

fake_esy_os_timer_entry_t *fake_esy_os_timer_get_last_deleted(void) {
  return s_ctx.last_deleted;
}

void fake_esy_os_timer_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  memset(s_ctx.handles, 0, sizeof(s_ctx.handles));
  s_ctx.last_created = NULL;
  s_ctx.last_deleted = NULL;
  s_ctx.create_static_result = ESY_OK;
  s_ctx.start_result = ESY_OK;
  s_ctx.create_static_count = 0u;
  s_ctx.delete_count = 0u;
}

void fake_esy_os_timer_set_create_result(esy_err_t ret) {
  s_ctx.create_static_result = ret;
}

void fake_esy_os_timer_set_start_result(esy_err_t ret) {
  s_ctx.start_result = ret;
}

uint32_t fake_esy_os_timer_get_create_static_count(void) {
  return s_ctx.create_static_count;
}

uint32_t fake_esy_os_timer_get_delete_count(void) {
  return s_ctx.delete_count;
}

void fake_esy_os_timer_fire(esy_os_timer_t *timer) {
  fake_esy_os_timer_entry_t *entry = fake_esy_os_timer_entry_from_handle(timer);
  if (entry != NULL && entry->used && entry->started && !entry->deleted &&
      !entry->dispatching && entry->callback != NULL) {
    if (!entry->auto_reload) entry->started = false;
    entry->dispatching = true;
    entry->callback(entry->arg);
    entry->dispatching = false;
  }
}

void fake_esy_os_timer_advance_daemon(void) {
  /* One daemon owns every timer: a callback must return before queued commands
   * and their fences can progress, including commands for another timer. */
  for (uint32_t i = 0u; i < FAKE_ESY_OS_TIMER_MAX_TIMERS; ++i)
    if (s_ctx.table[i].dispatching) return;
  for (uint32_t i = 0u; i < FAKE_ESY_OS_TIMER_MAX_TIMERS; ++i) {
    fake_esy_os_timer_entry_t *entry = &s_ctx.table[i];
    if (entry->used && entry->cleanup_pending) {
      entry->started = false;
      entry->cleanup_complete = true;
    }
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
  s_ctx.create_static_count++;

  if (s_ctx.create_static_result != ESY_OK) {
    return s_ctx.create_static_result;
  }
  if (callback == NULL || storage == NULL || out_timer == NULL || period_ms == 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0u; i < FAKE_ESY_OS_TIMER_MAX_TIMERS; ++i)
    if (s_ctx.table[i].used && s_ctx.table[i].static_storage == storage)
      return ESY_ERR_STATE;

  fake_esy_os_timer_entry_t *entry = fake_esy_os_timer_alloc_slot();
  if (entry == NULL) {
    return ESY_ERR_NO_MEM;
  }

  entry->created_static = true;
  entry->name = name;
  entry->period_ms = period_ms;
  entry->auto_reload = auto_reload;
  entry->arg = arg;
  entry->callback = callback;
  entry->static_storage = storage;

  s_ctx.last_created = entry;
  *out_timer = entry->timer_handle;
  return ESY_OK;
}

static esy_err_t fake_timer_cleanup(esy_os_timer_t *timer, bool deleting) {
  fake_esy_os_timer_entry_t *entry = fake_esy_os_timer_entry_from_handle(timer);
  if (entry == NULL) {
    return timer ? ESY_OK : ESY_ERR_INVALID_ARG;
  }

  if (entry->deleted) return ESY_OK;
  if (entry->cleanup_pending && entry->cleanup_deleting != deleting)
    return ESY_ERR_STATE;
  if (s_ctx.cleanup_result != ESY_OK) return s_ctx.cleanup_result;
  if (!entry->cleanup_pending) {
    entry->cleanup_pending = true;
    entry->cleanup_deleting = deleting;
    entry->cleanup_complete = false;
  }
  if (entry->dispatching || !entry->cleanup_complete) return ESY_ERR_BUSY;
  entry->cleanup_pending = false;
  if (deleting) {
    entry->deleted = true;
    entry->used = false;
    entry->callback = NULL;
    entry->arg = NULL;
    s_ctx.last_deleted = entry;
    s_ctx.delete_count++;
  } else {
    entry->stopped = true;
    entry->stop_count++;
  }
  return ESY_OK;
}

esy_err_t esy_os_timer_delete(esy_os_timer_t *timer) {
  return fake_timer_cleanup(timer, true);
}

esy_err_t esy_os_timer_start(esy_os_timer_t *timer) {
  fake_esy_os_timer_entry_t *entry = fake_esy_os_timer_entry_from_handle(timer);
  if (entry == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (entry->deleted) return ESY_ERR_INVALID_ARG;
  if (entry->cleanup_pending) return ESY_ERR_BUSY;
  if (s_ctx.start_result != ESY_OK) {
    return s_ctx.start_result;
  }

  entry->started = true;
  entry->stopped = false;
  entry->start_count++;
  return ESY_OK;
}

esy_err_t esy_os_timer_stop(esy_os_timer_t *timer) {
  return fake_timer_cleanup(timer, false);
}

__attribute__((constructor)) static void fake_esy_os_timer_ctor(void) {
  fake_esy_os_timer_reset();
}

void fake_esy_os_timer_set_cleanup_result(esy_err_t result) {
  s_ctx.cleanup_result = result;
}
