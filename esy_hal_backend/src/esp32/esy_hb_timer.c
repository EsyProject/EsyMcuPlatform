/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_timer.h"

#include <string.h>

#include "esp_timer.h"

#define MAX_TIMER_TASKS 8u

typedef struct {
  esy_timer_task_config_t task;
  esp_timer_handle_t timer_handle;
  bool active;
} esp32_timer_slot_t;

typedef struct {
  esp32_timer_slot_t slot[MAX_TIMER_TASKS];
  bool initialized;
} esy_hb_timer_ctx_t;

static esy_hb_timer_ctx_t s_ctx;

/* The same-core lifecycle owner serializes calls with SDK timer dispatch.
 * SDK calls and user callbacks remain outside locks.
 * A retained native handle reserves its slot even after notifications stop. */
static bool retire_timer(esp32_timer_slot_t *slot) {
  slot->active = false;
  slot->task.callback = NULL;
  slot->task.callback_arg = NULL;
  if (slot->timer_handle == NULL) return true;

  esp_err_t result = esp_timer_stop(slot->timer_handle);
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;
  result = esp_timer_delete(slot->timer_handle);
  if (result != ESP_OK) return false;
  slot->timer_handle = NULL;
  return true;
}

esy_err_t esy_hb_timer_init(void) {
  if (s_ctx.initialized) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < MAX_TIMER_TASKS; ++i)
    if (s_ctx.slot[i].timer_handle != NULL) return ESY_ERR_BUSY;
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.initialized = true;
  return ESY_OK;
}

void esy_hb_timer_deinit(void) {
  esy_hb_timer_cancel_all();
  s_ctx.initialized = false;
}

static void timer_callback_wrapper(void *arg) {
  esp32_timer_slot_t *slot = (esp32_timer_slot_t *)arg;
  if (!slot || !slot->active || !slot->task.callback) {
    return;
  }

  esy_timer_callback_t callback = slot->task.callback;
  void *callback_arg = slot->task.callback_arg;
  /* The SDK has already disarmed a one-shot. Retire it before user code can
   * reuse the slot; cleanup failure suppresses further delivery and keeps the
   * native handle available to cancel/deinit for a later retry. */
  if (!slot->task.repeat) (void)retire_timer(slot);
  callback(callback_arg);
  /* A callback may cancel this timer and reuse its slot. Do not access it here. */
}

int32_t esy_hb_timer_start(const esy_timer_task_config_t *task_config) {
  if (!task_config || !task_config->callback || task_config->interval <= 0 ||
      (task_config->mode != ESY_TIMER_MODE_MS &&
       task_config->mode != ESY_TIMER_MODE_US)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!s_ctx.initialized) return ESY_ERR_STATE;

  for (uint32_t i = 0u; i < MAX_TIMER_TASKS; ++i) {
    if (!s_ctx.slot[i].active && s_ctx.slot[i].timer_handle == NULL) {
      s_ctx.slot[i].task = *task_config;
      s_ctx.slot[i].active = true;

      esp_timer_create_args_t timer_args = {
          .callback = timer_callback_wrapper,
          .arg = &s_ctx.slot[i],
          .name = "esy_timer",
      };

      esp_err_t ret = esp_timer_create(&timer_args, &s_ctx.slot[i].timer_handle);
      if (ret != ESP_OK) {
        s_ctx.slot[i].active = false;
        return ESY_ERR_FAIL;
      }

      uint64_t timeout_us;
      if (task_config->mode == ESY_TIMER_MODE_MS) {
        timeout_us = (uint64_t)(uint32_t)task_config->interval * 1000u;
      } else {
        timeout_us = (uint32_t)task_config->interval;
      }

      if (task_config->repeat) {
        ret = esp_timer_start_periodic(s_ctx.slot[i].timer_handle, timeout_us);
      } else {
        ret = esp_timer_start_once(s_ctx.slot[i].timer_handle, timeout_us);
      }

      if (ret != ESP_OK) {
        if (!retire_timer(&s_ctx.slot[i])) return ESY_ERR_IO;
        return ESY_ERR_FAIL;
      }

      return (int32_t)i;
    }
  }
  return ESY_ERR_NO_MEM;
}

void esy_hb_timer_cancel(int32_t timer_id) {
  if (timer_id < 0 || (uint32_t)timer_id >= MAX_TIMER_TASKS) {
    return;
  }
  (void)retire_timer(&s_ctx.slot[timer_id]);
}

void esy_hb_timer_cancel_all(void) {
  for (uint32_t i = 0u; i < MAX_TIMER_TASKS; ++i)
    (void)retire_timer(&s_ctx.slot[i]);
}

uint8_t esy_hb_timer_get_max_timer_task(void) {
  return MAX_TIMER_TASKS;
}

bool esy_hb_timer_is_active(int32_t timer_id) {
  if (timer_id < 0 || (uint32_t)timer_id >= MAX_TIMER_TASKS) {
    return false;
  }
  return s_ctx.slot[timer_id].active || s_ctx.slot[timer_id].timer_handle != NULL;
}

void esy_hb_timer_handle_ms_irq(void) {
}

void esy_hb_timer_handle_us_irq(void) {
}
