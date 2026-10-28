/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_os_task.h"

#include <string.h>

struct esy_os_task_s {
  uint32_t slot;
};

typedef struct fake_esy_os_task_ctx_s {
  fake_esy_os_task_entry_t table[FAKE_ESY_OS_TASK_MAX_TASKS];
  esy_os_task_t handles[FAKE_ESY_OS_TASK_MAX_TASKS];
  esy_os_task_t *current_task;
  fake_esy_os_task_entry_t *last_created;
  fake_esy_os_task_entry_t *last_deleted;
  esy_err_t create_static_result;
  bool park_supported;
  void (*park_hook)(void);
  void (*critical_exit_hook)(void);
  uint32_t create_static_fail_call;
  esy_err_t create_static_failure_result;
  uint32_t create_static_count;
  uint32_t delete_count;
  uint32_t yield_core_count;
  uint32_t critical_nesting;
  uint32_t total_delay_ms;
  void (*notify_give_hook)(esy_os_task_t *task);
  void (*notify_take_hook)(void);
  void (*delay_hook)(uint32_t ms);
} fake_esy_os_task_ctx_t;

static fake_esy_os_task_ctx_t s_ctx;

static fake_esy_os_task_entry_t *fake_esy_os_task_alloc_slot(void) {
  for (uint32_t i = 0u; i < FAKE_ESY_OS_TASK_MAX_TASKS; i++) {
    if (!s_ctx.table[i].used) {
      memset(&s_ctx.table[i], 0, sizeof(s_ctx.table[i]));
      s_ctx.handles[i].slot = i;
      s_ctx.table[i].used = true;
      s_ctx.table[i].task_handle = &s_ctx.handles[i];
      return &s_ctx.table[i];
    }
  }

  return NULL;
}

static fake_esy_os_task_entry_t *fake_esy_os_task_entry_from_handle(
    esy_os_task_t *task) {
  if (task == NULL || task->slot >= FAKE_ESY_OS_TASK_MAX_TASKS) {
    return NULL;
  }

  fake_esy_os_task_entry_t *entry = &s_ctx.table[task->slot];
  if (entry->task_handle != task) {
    return NULL;
  }

  return entry;
}

fake_esy_os_task_entry_t *fake_esy_os_task_get_table(void) { return s_ctx.table; }

uint32_t fake_esy_os_task_get_table_size(void) {
  return FAKE_ESY_OS_TASK_MAX_TASKS;
}

fake_esy_os_task_entry_t *fake_esy_os_task_find(esy_os_task_t *task) {
  return fake_esy_os_task_entry_from_handle(task);
}

fake_esy_os_task_entry_t *fake_esy_os_task_get_last_created(void) {
  return s_ctx.last_created;
}

fake_esy_os_task_entry_t *fake_esy_os_task_get_last_deleted(void) {
  return s_ctx.last_deleted;
}

void fake_esy_os_task_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.create_static_result = ESY_OK;
  s_ctx.park_supported = true;
}

void fake_esy_os_task_set_create_result(esy_err_t ret) {
  s_ctx.create_static_result = ret;
}

void fake_esy_os_task_set_create_static_failure(uint32_t call, esy_err_t result) {
  s_ctx.create_static_fail_call = call;
  s_ctx.create_static_failure_result = result;
}

uint32_t fake_esy_os_task_get_create_static_count(void) {
  return s_ctx.create_static_count;
}

uint32_t fake_esy_os_task_get_delete_count(void) { return s_ctx.delete_count; }

void fake_esy_os_task_set_current(esy_os_task_t *task) {
  s_ctx.current_task = task;
}

esy_os_task_t *fake_esy_os_task_get_current(void) { return s_ctx.current_task; }

uint32_t fake_esy_os_task_get_critical_nesting(void) {
  return s_ctx.critical_nesting;
}

void fake_esy_os_task_set_stack_remaining(esy_os_task_t *task, int32_t words) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_entry_from_handle(task);
  if (entry != NULL) {
    entry->stack_remaining_words = words;
  }
}

uint32_t fake_esy_os_task_get_total_delay_ms(void) { return s_ctx.total_delay_ms; }

esy_err_t esy_os_task_create_static(const char *name, esy_os_task_func_t func,
                                    void *arg, uint32_t stack_size_bytes,
                                    uint32_t priority, void *stack_buffer,
                                    esy_os_task_static_storage_t *storage,
                                    esy_os_task_t **out_task) {
  if (out_task) *out_task = NULL;
  s_ctx.create_static_count++;
  if (s_ctx.create_static_count == s_ctx.create_static_fail_call) {
    return s_ctx.create_static_failure_result;
  }

  if (s_ctx.create_static_result != ESY_OK) {
    return s_ctx.create_static_result;
  }
  if (func == NULL || stack_buffer == NULL || storage == NULL ||
      out_task == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  fake_esy_os_task_entry_t *entry = fake_esy_os_task_alloc_slot();
  if (entry == NULL) {
    return ESY_ERR_NO_MEM;
  }

  entry->created_static = true;
  entry->name = name;
  entry->entry = func;
  entry->arg = arg;
  entry->priority = priority;
  entry->stack_size = stack_size_bytes;
  entry->stack_size_bytes = stack_size_bytes;
  entry->stack_buffer = stack_buffer;
  entry->static_storage = storage;
  entry->stack_remaining_words = (int32_t)stack_size_bytes;

  s_ctx.last_created = entry;
  *out_task = entry->task_handle;
  return ESY_OK;
}

void esy_os_task_delete(esy_os_task_t *task) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_entry_from_handle(task);
  if (entry == NULL) {
    return;
  }

  entry->deleted = true;
  entry->used = false;
  s_ctx.last_deleted = entry;
  s_ctx.delete_count++;
  if (s_ctx.current_task == task) {
    s_ctx.current_task = NULL;
  }
}

void fake_esy_os_task_set_park_supported(bool supported) { s_ctx.park_supported = supported; }
void fake_esy_os_task_set_park_hook(void (*hook)(void)) { s_ctx.park_hook = hook; }
void fake_esy_os_task_set_critical_exit_hook(void (*hook)(void)) {
  s_ctx.critical_exit_hook = hook;
}
bool esy_os_task_park_supported(void) { return s_ctx.park_supported; }
esy_err_t esy_os_task_park(void) {
  if (!s_ctx.park_supported) return ESY_ERR_NOT_SUPPORTED;
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(s_ctx.current_task);
  if (entry == NULL) return ESY_ERR_STATE;
  entry->parked = true;
  if (s_ctx.park_hook) s_ctx.park_hook();
  return ESY_OK;
}
esy_err_t esy_os_task_delete_parked(esy_os_task_t *task) {
  if (task == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.park_supported) return ESY_ERR_NOT_SUPPORTED;
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_find(task);
  if (entry == NULL || entry->deleted) return ESY_OK;
  if (task == s_ctx.current_task) return ESY_ERR_STATE;
  if (!entry->parked) return ESY_ERR_BUSY;
  esy_os_task_delete(task);
  return ESY_OK;
}

void fake_esy_os_task_set_notify_give_hook(void (*hook)(esy_os_task_t *task)) {
  s_ctx.notify_give_hook = hook;
}

void fake_esy_os_task_set_notify_take_hook(void (*hook)(void)) {
  s_ctx.notify_take_hook = hook;
}

void fake_esy_os_task_set_delay_hook(void (*hook)(uint32_t ms)) {
  s_ctx.delay_hook = hook;
}

void esy_os_task_delay_ms(uint32_t ms) {
  s_ctx.total_delay_ms += ms;
  if (s_ctx.delay_hook) s_ctx.delay_hook(ms);
}

esy_os_tick_t esy_os_task_get_tick(void) { return s_ctx.total_delay_ms; }

bool esy_os_task_is_current(const esy_os_task_t *task) {
  return task && task == s_ctx.current_task;
}

esy_err_t esy_os_task_delay_until(esy_os_tick_t *prev_tick, uint32_t period_ms) {
  /* The shared host fake models a 32-bit, 1 kHz native tick counter. */
  if (!prev_tick || !period_ms || *prev_tick > UINT32_MAX)
    return ESY_ERR_INVALID_ARG;
  uint32_t period = period_ms == UINT32_MAX ? UINT32_MAX - 1u : period_ms;
  uint32_t previous = (uint32_t)*prev_tick;
  uint32_t target = previous + period;
  uint32_t now = s_ctx.total_delay_ms;
  bool wait = now < previous ? target < previous && target > now
                            : target < previous || target > now;
  if (wait) s_ctx.total_delay_ms = target;
  *prev_tick = target;
  return ESY_OK;
}

int32_t esy_os_task_get_stack_remaining(esy_os_task_t *task) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_entry_from_handle(task);
  if (entry == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  return entry->stack_remaining_words;
}

void *esy_os_task_get_handle(esy_os_task_t *task) { return task; }

void esy_os_task_set_core_affinity(esy_os_task_t *task, uint8_t core_affinity) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_entry_from_handle(task);
  if (entry != NULL) {
    entry->core_affinity = core_affinity;
  }
}

esy_err_t esy_os_task_notify(esy_os_task_t *task, uint32_t value,
                             esy_os_notify_action_t action) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_entry_from_handle(task);
  if (entry == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  switch (action) {
  case ESY_OS_NOTIFY_NO_ACTION: break;
  case ESY_OS_NOTIFY_SET_BITS: entry->notify_value |= value; break;
  case ESY_OS_NOTIFY_INCREMENT: ++entry->notify_value; break;
  case ESY_OS_NOTIFY_SET_WITHOUT_OVERWRITE:
    if (entry->notify_pending) return ESY_ERR_FAIL;
    entry->notify_value = value;
    break;
  case ESY_OS_NOTIFY_SET_WITH_OVERWRITE: entry->notify_value = value; break;
  default: return ESY_ERR_INVALID_ARG;
  }
  entry->notify_pending = true;
  return ESY_OK;
}

esy_err_t esy_os_task_notify_wait(uint32_t entry_clear_bits,
                                  uint32_t exit_clear_bits, uint32_t *value,
                                  uint32_t timeout_ms) {
  (void)timeout_ms;

  fake_esy_os_task_entry_t *entry =
      fake_esy_os_task_entry_from_handle(s_ctx.current_task);
  if (entry == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!entry->notify_pending) {
    entry->notify_value &= ~entry_clear_bits;
  }
  if (s_ctx.notify_take_hook) s_ctx.notify_take_hook();
  if (!entry->notify_pending) {
    return ESY_ERR_FAIL;
  }

  if (value != NULL) {
    *value = entry->notify_value;
  }
  entry->notify_value &= ~exit_clear_bits;
  entry->notify_pending = false;
  return ESY_OK;
}

void esy_os_task_notify_clear(void) {
  fake_esy_os_task_entry_t *entry =
      fake_esy_os_task_entry_from_handle(s_ctx.current_task);
  if (entry != NULL) {
    entry->notify_pending = false;
  }
}

esy_err_t esy_os_task_notify_give(esy_os_task_t *task) {
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_entry_from_handle(task);
  if (entry == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  ++entry->notify_value;
  entry->notify_pending = true;
  ++entry->notify_give_count;
  if (s_ctx.notify_give_hook) s_ctx.notify_give_hook(task);
  return ESY_OK;
}

esy_err_t esy_os_task_notify_take(bool clear_count, uint32_t timeout_ms) {
  fake_esy_os_task_entry_t *entry =
      fake_esy_os_task_entry_from_handle(s_ctx.current_task);
  if (entry == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  entry->notify_take_clear_count = clear_count;
  entry->notify_take_timeout_ms = timeout_ms;
  if (s_ctx.notify_take_hook) s_ctx.notify_take_hook();
  entry->notify_pending = false;
  if (entry->notify_value == 0u) {
    return ESY_ERR_FAIL;
  }

  if (clear_count) {
    entry->notify_value = 0u;
  } else {
    --entry->notify_value;
  }
  return ESY_OK;
}

esy_err_t esy_os_task_notify_give_from_isr(esy_os_task_t *task,
                                           bool *need_yield) {
  if (need_yield != NULL) {
    *need_yield = false;
  }
  fake_esy_os_task_entry_t *entry = fake_esy_os_task_entry_from_handle(task);
  if (entry != NULL) ++entry->notify_give_from_isr_count;
  return esy_os_task_notify_give(task);
}

esy_err_t esy_os_task_notify_from_isr(esy_os_task_t *task, uint32_t value,
                                      esy_os_notify_action_t action, bool *need_yield) {
  if (need_yield != NULL) {
    *need_yield = false;
  }
  return esy_os_task_notify(task, value, action);
}

void esy_os_task_enter_critical(void) { s_ctx.critical_nesting++; }

void esy_os_task_exit_critical(void) {
  if (s_ctx.critical_nesting > 0u) s_ctx.critical_nesting--;
  if (s_ctx.critical_nesting == 0u && s_ctx.critical_exit_hook != NULL) {
    void (*hook)(void) = s_ctx.critical_exit_hook;
    s_ctx.critical_exit_hook = NULL;
    hook();
  }
}

uint32_t esy_os_task_enter_critical_from_isr(void) {
  esy_os_task_enter_critical();
  return s_ctx.critical_nesting;
}

void esy_os_task_exit_critical_from_isr(uint32_t saved_status) {
  (void)saved_status;
  esy_os_task_exit_critical();
}

uint32_t esy_os_task_enter_crit_auto(void) {
  esy_os_task_enter_critical();
  return s_ctx.critical_nesting;
}

void esy_os_task_exit_crit_auto(uint32_t key) {
  (void)key;
  esy_os_task_exit_critical();
}

esy_err_t esy_os_task_get_system_state(esy_os_task_status_t *items,
                                       uint32_t items_capacity,
                                       uint32_t *out_count,
                                       uint32_t *out_total_run_time,
                                       esy_os_task_snapshot_storage_t *storage) {
  if (out_count) *out_count = 0u;
  if (out_total_run_time) *out_total_run_time = 0u;
  if (!items || items_capacity == 0u ||
      items_capacity > ESY_OS_TASK_SYSTEM_STATE_MAX || !storage ||
      (uintptr_t)items % _Alignof(esy_os_task_status_t) != 0u ||
      (uintptr_t)storage % _Alignof(esy_os_task_snapshot_storage_t) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  uint32_t needed = 0u;
  for (uint32_t i = 0u; i < FAKE_ESY_OS_TASK_MAX_TASKS; ++i) {
    if (s_ctx.table[i].used && !s_ctx.table[i].deleted) ++needed;
  }
  if (needed == 0u || needed > items_capacity) return ESY_ERR_NO_MEM;
  uint32_t count = 0u;
  for (uint32_t i = 0u;
       i < FAKE_ESY_OS_TASK_MAX_TASKS && count < items_capacity; i++) {
    if (s_ctx.table[i].used && !s_ctx.table[i].deleted) {
      size_t length = 0u;
      const char *name = s_ctx.table[i].name;
      while (name && length < sizeof(items[count].name) - 1u && name[length]) {
        items[count].name[length] = name[length];
        ++length;
      }
      items[count].name[length] = '\0';
      items[count].handle = s_ctx.table[i].task_handle;
      items[count].state = ESY_OS_TASK_STATE_READY;
      items[count].current_priority = s_ctx.table[i].priority;
      items[count].base_priority = s_ctx.table[i].priority;
      items[count].stack_high_water_mark =
          (uint32_t)s_ctx.table[i].stack_remaining_words;
      items[count].task_number = i;
      items[count].runtime_counter = 0u;
      count++;
    }
  }

  if (out_count != NULL) {
    *out_count = count;
  }
  if (out_total_run_time != NULL) {
    *out_total_run_time = 0u;
  }
  return ESY_OK;
}

__attribute__((constructor)) static void fake_esy_os_task_ctor(void) {
  fake_esy_os_task_reset();
}

void esy_os_task_yield_core(uint8_t core_id) {
  (void)core_id;
  s_ctx.yield_core_count++;
}

uint32_t fake_esy_os_task_get_yield_core_count(void) {
  return s_ctx.yield_core_count;
}
