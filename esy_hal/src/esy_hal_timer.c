/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_timer.h"

#include <limits.h>
#include <string.h>

#include "esy_board/esy_board.h"
#include "esy_hal_backend/esy_hb_timer.h"
#include "esy_hal/esy_hal_system.h"

#define ESY_HAL_TIMER_MAX_TRACKED_TASKS 16u

enum { ESY_HAL_TIMER_DRIVER_ID_UNPUBLISHED = -1 };

typedef struct {
  esy_timer_callback_t callback;
  void *callback_arg;
  uint32_t interval_us;
  uint32_t deadline_us;
  int32_t driver_id;
  bool repeat;
  bool active;
  bool starting;
} esy_hal_timer_slot_t;

typedef struct {
  esy_hal_timer_slot_t slots[ESY_HAL_TIMER_MAX_TRACKED_TASKS];
  bool initialized;
} esy_hal_timer_ctx_t;

static esy_hal_timer_ctx_t s_ctx;

static void timer_callback(void *arg) {
  esy_hal_timer_slot_t *slot = arg;
  esy_timer_callback_t callback;
  void *callback_arg;

  if (slot == NULL) return;
  /* The owner core's timer ISR shares deadlines with task/ISR queries. Publish
   * the transition under local masking; user callbacks must run after restore. */
  uint32_t key = esy_hal_system_irq_save();
  if (!slot->active) {
    esy_hal_system_irq_restore(key);
    return;
  }
  callback = slot->callback;
  callback_arg = slot->callback_arg;
  if (slot->repeat) {
    uint32_t now = esy_hal_system_time_32_us();
    uint32_t periods = (int32_t)(now - slot->deadline_us) >= 0
                           ? (now - slot->deadline_us) / slot->interval_us + 1u
                           : 1u;
    slot->deadline_us += periods * slot->interval_us;
  } else {
    slot->active = false;
  }
  esy_hal_system_irq_restore(key);
  if (callback != NULL) callback(callback_arg);
}

esy_err_t esy_hal_timer_init(void) {
  esy_err_t ret;

  if (s_ctx.initialized) return ESY_ERR_STATE;
  memset(&s_ctx, 0, sizeof(s_ctx));
  ret = esy_hb_timer_init();
  if (ret == ESY_OK) s_ctx.initialized = true;
  return ret;
}

void esy_hal_timer_deinit(void) {
  esy_hb_timer_deinit();
  memset(&s_ctx, 0, sizeof(s_ctx));
}

void esy_hal_timer_default_task_cfg(esy_timer_task_config_t* config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->interval = 1000;
  config->callback = NULL;
  config->callback_arg = NULL;
  config->repeat = true;
  config->mode = ESY_TIMER_MODE_MS;
}

int32_t esy_hal_timer_start(esy_timer_task_config_t* task_config) {
  esy_timer_task_config_t driver_config;
  esy_hal_timer_slot_t *slot = NULL;
  uint32_t interval_us;
  int32_t driver_id;

  if (!task_config || task_config->interval <= 0 || !task_config->callback) {
    return ESY_ERR_INVALID_ARG;
  }

  if (!esy_board_get_timer_task_inner_config(task_config->mode,
                                             &task_config->inner)) {
    return ESY_ERR_NOT_SUPPORTED;
  }

  if (!s_ctx.initialized) return ESY_ERR_STATE;

  if ((task_config->mode == ESY_TIMER_MODE_MS) &&
      ((uint32_t)task_config->interval > (UINT32_MAX / 1000u))) {
    return ESY_ERR_INVALID_ARG;
  }
  interval_us = (task_config->mode == ESY_TIMER_MODE_MS)
                    ? (uint32_t)task_config->interval * 1000u
                    : (uint32_t)task_config->interval;
  if (interval_us > (uint32_t)INT32_MAX) return ESY_ERR_INVALID_ARG;
  /* One owner core; its timer ISR shares these slots. Reserve and publish
   * metadata atomically, but keep HAL backend startup and user callbacks outside. */
  uint32_t key = esy_hal_system_irq_save();
  for (uint32_t i = 0u; i < ESY_HAL_TIMER_MAX_TRACKED_TASKS; i++) {
    if (!s_ctx.slots[i].active && !s_ctx.slots[i].starting) {
      slot = &s_ctx.slots[i];
      break;
    }
  }
  if (slot == NULL) {
    esy_hal_system_irq_restore(key);
    return ESY_ERR_NO_MEM;
  }

  driver_config = *task_config;
  slot->callback = task_config->callback;
  slot->callback_arg = task_config->callback_arg;
  slot->interval_us = interval_us;
  slot->deadline_us = esy_hal_system_time_32_us() + interval_us;
  slot->repeat = task_config->repeat;
  /* A backend may invoke this periodic callback before returning its ID.
   * Such a callback can cancel peers, but must never match this reservation. */
  slot->driver_id = ESY_HAL_TIMER_DRIVER_ID_UNPUBLISHED;
  slot->starting = true;
  slot->active = true;
  driver_config.callback = timer_callback;
  driver_config.callback_arg = slot;

  esy_hal_system_irq_restore(key);
  driver_id = esy_hb_timer_start(&driver_config);
  key = esy_hal_system_irq_save();
  if (driver_id < 0) {
    memset(slot, 0, sizeof(*slot));
    esy_hal_system_irq_restore(key);
    return driver_id;
  }
  slot->driver_id = driver_id;
  slot->starting = false;
  esy_hal_system_irq_restore(key);
  return driver_id;
}

void esy_hal_timer_cancel(int32_t timer_id) {
  if (timer_id < 0 || (uint32_t)timer_id >= esy_hb_timer_get_max_timer_task())
    return;

  if (!esy_hb_timer_is_active(timer_id)) return;

  esy_hb_timer_cancel(timer_id);
  uint32_t key = esy_hal_system_irq_save();
  for (uint32_t i = 0u; i < ESY_HAL_TIMER_MAX_TRACKED_TASKS; i++) {
    if (s_ctx.slots[i].active && !s_ctx.slots[i].starting &&
        (s_ctx.slots[i].driver_id == timer_id)) {
      memset(&s_ctx.slots[i], 0, sizeof(s_ctx.slots[i]));
      break;
    }
  }
  esy_hal_system_irq_restore(key);
}

bool esy_hal_timer_is_active(int32_t timer_id) {
  if (timer_id < 0 || (uint32_t)timer_id >= esy_hb_timer_get_max_timer_task())
    return false;

  return esy_hb_timer_is_active(timer_id);
}

esy_err_t esy_hal_timer_get_next_wake_interval_us(
    uint32_t *out_interval_us) {
  uint32_t now_us;
  uint32_t minimum = UINT32_MAX;
  bool found = false;

  if (out_interval_us == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.initialized) return ESY_ERR_STATE;
  uint32_t key = esy_hal_system_irq_save();
  now_us = esy_hal_system_time_32_us();
  for (uint32_t i = 0u; i < ESY_HAL_TIMER_MAX_TRACKED_TASKS; i++) {
    const esy_hal_timer_slot_t *slot = &s_ctx.slots[i];
    uint32_t remaining;

    if (!slot->active) continue;
    remaining = ((int32_t)(slot->deadline_us - now_us) <= 0)
                    ? 0u
                    : slot->deadline_us - now_us;
    if (remaining < minimum) minimum = remaining;
    found = true;
  }
  esy_hal_system_irq_restore(key);
  if (!found) return ESY_ERR_STATE;
  *out_interval_us = minimum;
  return ESY_OK;
}

void esy_hal_timer_handle_ms_irq(void) {
  esy_hb_timer_handle_ms_irq();
}

void esy_hal_timer_handle_us_irq(void) {
  esy_hb_timer_handle_us_irq();
}
