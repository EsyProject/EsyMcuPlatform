/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_timer.h"

#include "pico/time.h"
#include "hardware/sync.h"

#define MAX_TIMER_TASKS 8

typedef struct {
  esy_timer_task_config_t task;
  repeating_timer_t timer;
  bool active;
  bool starting;
  bool dispatching;
} pico_timer_slot_t;

typedef struct {
  pico_timer_slot_t slots[MAX_TIMER_TASKS];
} esy_hb_timer_ctx_t;

static esy_hb_timer_ctx_t s_ctx;

static bool is_valid_timer_id(int32_t timer_id) {
  return (timer_id >= 0) && (timer_id < MAX_TIMER_TASKS);
}

esy_err_t esy_hb_timer_init(void) {
  esy_hb_timer_cancel_all();

  return ESY_OK;
}

void esy_hb_timer_deinit(void) {
  esy_hb_timer_cancel_all();
}

static bool timer_callback_wrapper(repeating_timer_t *rt) {
  if (!rt || !rt->user_data) return false;

  pico_timer_slot_t *slot = (pico_timer_slot_t *)rt->user_data;
  if (!slot->active || !slot->task.callback) return false;

  const bool repeat = slot->task.repeat;
  slot->dispatching = true;
  slot->task.callback(slot->task.callback_arg);
  if (!repeat) slot->active = false;
  slot->dispatching = false;
  return repeat && slot->active;
}

static int32_t alloc_and_start_timer(const esy_timer_task_config_t *task, bool is_ms) {
  /* Same-core task/ISR slot reservation; SDK calls remain outside masking. */
  uint32_t key = save_and_disable_interrupts();
  for (int i = 0; i < MAX_TIMER_TASKS; ++i) {
    pico_timer_slot_t *slot = &s_ctx.slots[i];
    if (!slot->active && !slot->starting && !slot->dispatching) {
      slot->task = *task;
      slot->active = true;
      slot->starting = true;
      restore_interrupts(key);

      bool ok =
          is_ms ? add_repeating_timer_ms(-task->interval, timer_callback_wrapper,
                                         slot, &slot->timer)
                : add_repeating_timer_us(-task->interval, timer_callback_wrapper,
                                         slot, &slot->timer);

      key = save_and_disable_interrupts();
      slot->starting = false;
      if (!ok) {
        slot->active = false;
        restore_interrupts(key);
        return ESY_ERR_FAIL;
      }

      restore_interrupts(key);
      return i;
    }
  }
  restore_interrupts(key);
  return ESY_ERR_FAIL;
}

int32_t esy_hb_timer_start(const esy_timer_task_config_t *task_config) {
  if (!task_config || !task_config->callback || task_config->interval == 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  if ((task_config->mode != ESY_TIMER_MODE_MS) &&
      (task_config->mode != ESY_TIMER_MODE_US)) {
    return ESY_ERR_INVALID_ARG;
  }

  return (task_config->mode == ESY_TIMER_MODE_MS)
             ? alloc_and_start_timer(task_config, true)
             : alloc_and_start_timer(task_config, false);
}

void esy_hb_timer_cancel(int32_t timer_id) {
  if (!is_valid_timer_id(timer_id)) return;

  pico_timer_slot_t *slot = &s_ctx.slots[timer_id];
  if (slot->active) {
    cancel_repeating_timer(&slot->timer);
    slot->active = false;
  }
}

void esy_hb_timer_cancel_all(void) {
  for (int i = 0; i < MAX_TIMER_TASKS; ++i) {
    pico_timer_slot_t *slot = &s_ctx.slots[i];
    if (slot->active) {
      cancel_repeating_timer(&slot->timer);
      slot->active = false;
    }
  }
}

uint8_t esy_hb_timer_get_max_timer_task(void) {
  return MAX_TIMER_TASKS;
}

bool esy_hb_timer_is_active(int32_t timer_id) {
  if (!is_valid_timer_id(timer_id)) return false;

  return s_ctx.slots[timer_id].active;
}

void esy_hb_timer_handle_ms_irq(void) {
}

void esy_hb_timer_handle_us_irq(void) {
}
