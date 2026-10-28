/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_timer.h"

#include <limits.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#if !defined(TIM2) || !defined(TIM5)
#error "STM32 logical timers require the dedicated 32-bit TIM2 and TIM5 resources"
#endif

#define MAX_TIMER_TASKS       8u
#define TIMER_DOMAIN_COUNT   2u
#define TIMER_COUNTER_HZ     1000000u
#define TIMER_US_PER_MS      1000u

typedef struct {
  esy_timer_callback_t callback;
  void *callback_arg;
  uint32_t interval_us;
  uint32_t deadline;
  esy_timer_mode_t mode;
  bool repeat;
  bool active;
} timer_slot_t;

typedef struct {
  TIM_HandleTypeDef htim;
  uint8_t irq_priority;
  bool dispatching;
} timer_domain_t;

typedef struct {
  timer_domain_t domains[TIMER_DOMAIN_COUNT];
  timer_slot_t slots[MAX_TIMER_TASKS];
  bool initialized;
} esy_hb_timer_ctx_t;

static esy_hb_timer_ctx_t s_ctx;
static const IRQn_Type k_timer_irqs[TIMER_DOMAIN_COUNT] = {TIM2_IRQn, TIM5_IRQn};

/* One owner core serializes task calls; the two timer ISRs share slots with it.
 * Local IRQ masking protects only slot publication and compare-register updates.
 * No OS lock can cover an ISR; SDK initialization and callbacks stay outside. */
static uint32_t timer_lock(void) {
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  return key;
}

static void timer_unlock(uint32_t key) {
  __set_PRIMASK(key);
}

static bool deadline_reached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

static esy_err_t timer_prescaler(uint32_t *prescaler) {
  RCC_ClkInitTypeDef clocks;
  uint32_t flash_latency;
  HAL_RCC_GetClockConfig(&clocks, &flash_latency);
  uint32_t timer_hz = HAL_RCC_GetPCLK1Freq();
  if (clocks.APB1CLKDivider != RCC_HCLK_DIV1) timer_hz *= 2u;
#ifdef RCC_DCKCFGR_TIMPRE
  if ((RCC->DCKCFGR & RCC_DCKCFGR_TIMPRE) != 0u) {
    if (clocks.APB1CLKDivider == RCC_HCLK_DIV1 ||
        clocks.APB1CLKDivider == RCC_HCLK_DIV2 ||
        clocks.APB1CLKDivider == RCC_HCLK_DIV4) {
      timer_hz = HAL_RCC_GetHCLKFreq();
    } else {
      timer_hz *= 2u;
    }
  }
#endif
  if (timer_hz < TIMER_COUNTER_HZ ||
      timer_hz % TIMER_COUNTER_HZ != 0u ||
      timer_hz / TIMER_COUNTER_HZ > UINT16_MAX + 1u) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  *prescaler = timer_hz / TIMER_COUNTER_HZ - 1u;
  return ESY_OK;
}

static void reset_timers(void) {
  __HAL_RCC_TIM2_FORCE_RESET();
  __HAL_RCC_TIM5_FORCE_RESET();
  __HAL_RCC_TIM2_RELEASE_RESET();
  __HAL_RCC_TIM5_RELEASE_RESET();
}

static void release_timers(void) {
  for (uint32_t i = 0u; i < TIMER_DOMAIN_COUNT; ++i) {
    HAL_NVIC_DisableIRQ(k_timer_irqs[i]);
    if (s_ctx.domains[i].htim.Instance != NULL) {
      s_ctx.domains[i].htim.Instance->DIER = 0u;
      (void)HAL_TIM_Base_Stop(&s_ctx.domains[i].htim);
      (void)HAL_TIM_Base_DeInit(&s_ctx.domains[i].htim);
    }
    HAL_NVIC_ClearPendingIRQ(k_timer_irqs[i]);
  }
  reset_timers();
  __HAL_RCC_TIM2_CLK_DISABLE();
  __HAL_RCC_TIM5_CLK_DISABLE();
  memset(&s_ctx, 0, sizeof(s_ctx));
}

/* Arm one compare for the earliest logical deadline; never generate a periodic
 * software tick. Recheck CNT after publication so a passed compare is not lost. */
static void schedule_locked(esy_timer_mode_t mode) {
  timer_domain_t *domain = &s_ctx.domains[(uint32_t)mode];
  TIM_TypeDef *tim = domain->htim.Instance;
  const uint32_t now = tim->CNT;
  uint32_t minimum = UINT32_MAX;
  uint32_t next = 0u;

  tim->DIER &= ~TIM_DIER_CC1IE;
  __HAL_TIM_CLEAR_FLAG(&domain->htim, TIM_FLAG_CC1);
  for (uint32_t i = 0u; i < MAX_TIMER_TASKS; ++i) {
    const timer_slot_t *slot = &s_ctx.slots[i];
    if (!slot->active || slot->mode != mode) continue;
    uint32_t remaining = deadline_reached(now, slot->deadline)
                             ? 0u : slot->deadline - now;
    if (remaining < minimum) {
      minimum = remaining;
      next = slot->deadline;
    }
  }
  if (minimum == UINT32_MAX) {
    HAL_NVIC_ClearPendingIRQ(k_timer_irqs[(uint32_t)mode]);
    return;
  }
  tim->CCR1 = next;
  tim->DIER |= TIM_DIER_CC1IE;
  if (deadline_reached(tim->CNT, next)) {
    /* CC1G sets the normal compare flag as well as its pending interrupt. */
    tim->EGR = TIM_EGR_CC1G;
  }
}

esy_err_t esy_hb_timer_init(void) {
  if (s_ctx.initialized) return ESY_ERR_STATE;
  if (!IS_TIM_32B_COUNTER_INSTANCE(TIM2) || !IS_TIM_32B_COUNTER_INSTANCE(TIM5))
    return ESY_ERR_NOT_SUPPORTED;
  uint32_t prescaler;
  esy_err_t result = timer_prescaler(&prescaler);
  if (result != ESY_OK) return result;

  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.domains[ESY_TIMER_MODE_MS].htim.Instance = TIM2;
  s_ctx.domains[ESY_TIMER_MODE_US].htim.Instance = TIM5;
  __HAL_RCC_TIM2_CLK_ENABLE();
  __HAL_RCC_TIM5_CLK_ENABLE();
  reset_timers();
  for (uint32_t i = 0u; i < TIMER_DOMAIN_COUNT; ++i) {
    TIM_HandleTypeDef *htim = &s_ctx.domains[i].htim;
    HAL_NVIC_DisableIRQ(k_timer_irqs[i]);
    HAL_NVIC_ClearPendingIRQ(k_timer_irqs[i]);
    htim->Init.Prescaler = prescaler;
    htim->Init.CounterMode = TIM_COUNTERMODE_UP;
    htim->Init.Period = UINT32_MAX;
    htim->Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(htim) != HAL_OK) goto init_failed;
    TIM_OC_InitTypeDef compare = {.OCMode = TIM_OCMODE_TIMING};
    if (HAL_TIM_OC_ConfigChannel(htim, &compare, TIM_CHANNEL_1) != HAL_OK)
      goto init_failed;
    htim->Instance->DIER = 0u;
    htim->Instance->SR = 0u;
    if (HAL_TIM_Base_Start(htim) != HAL_OK) goto init_failed;
  }
  s_ctx.initialized = true;
  return ESY_OK;

init_failed:
  release_timers();
  return ESY_ERR_IO;
}

void esy_hb_timer_deinit(void) {
  if (!s_ctx.initialized) return;
  uint32_t key = timer_lock();
  s_ctx.initialized = false;
  for (uint32_t i = 0u; i < TIMER_DOMAIN_COUNT; ++i) {
    HAL_NVIC_DisableIRQ(k_timer_irqs[i]);
    s_ctx.domains[i].htim.Instance->DIER = 0u;
    HAL_NVIC_ClearPendingIRQ(k_timer_irqs[i]);
  }
  timer_unlock(key);
  release_timers();
}

int32_t esy_hb_timer_start(const esy_timer_task_config_t *task) {
  if (task == NULL || task->callback == NULL || task->interval <= 0 ||
      (task->mode != ESY_TIMER_MODE_MS && task->mode != ESY_TIMER_MODE_US)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (task->inner.source != ESY_TIMER_SOURCE_DEFAULT)
    return ESY_ERR_NOT_SUPPORTED;
  if (task->inner.irq_priority >= (1u << __NVIC_PRIO_BITS))
    return ESY_ERR_INVALID_ARG;
  const uint64_t interval = (uint64_t)(uint32_t)task->interval *
      (task->mode == ESY_TIMER_MODE_MS ? TIMER_US_PER_MS : 1u);
  if (interval > INT32_MAX) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.initialized) return ESY_ERR_STATE;

  uint32_t key = timer_lock();
  timer_domain_t *domain = &s_ctx.domains[(uint32_t)task->mode];
  int32_t free_slot = -1;
  /* Both timer IRQs share HAL callback slots. Equal priority prevents one timer
   * dispatcher from canceling/reusing a slot before the other enters HAL. */
  for (uint32_t i = 0u; i < TIMER_DOMAIN_COUNT; ++i) {
    if (s_ctx.domains[i].dispatching &&
        s_ctx.domains[i].irq_priority != task->inner.irq_priority) {
      timer_unlock(key);
      return ESY_ERR_BUSY;
    }
  }
  for (uint32_t i = 0u; i < MAX_TIMER_TASKS; ++i) {
    if (!s_ctx.slots[i].active) {
      if (free_slot < 0) free_slot = (int32_t)i;
    } else if (s_ctx.domains[(uint32_t)s_ctx.slots[i].mode].irq_priority !=
               task->inner.irq_priority) {
      timer_unlock(key);
      return ESY_ERR_BUSY;
    }
  }
  if (free_slot < 0) {
    timer_unlock(key);
    return ESY_ERR_NO_MEM;
  }
  domain->irq_priority = task->inner.irq_priority;
  timer_slot_t *slot = &s_ctx.slots[(uint32_t)free_slot];
  *slot = (timer_slot_t){
      .callback = task->callback,
      .callback_arg = task->callback_arg,
      .interval_us = (uint32_t)interval,
      .deadline = domain->htim.Instance->CNT + (uint32_t)interval,
      .mode = task->mode,
      .repeat = task->repeat,
      .active = true,
  };
  HAL_NVIC_SetPriority(k_timer_irqs[(uint32_t)task->mode],
                       domain->irq_priority, 0u);
  schedule_locked(task->mode);
  HAL_NVIC_EnableIRQ(k_timer_irqs[(uint32_t)task->mode]);
  timer_unlock(key);
  return free_slot;
}

void esy_hb_timer_cancel(int32_t timer_id) {
  if (timer_id < 0 || (uint32_t)timer_id >= MAX_TIMER_TASKS ||
      !s_ctx.initialized) return;
  uint32_t key = timer_lock();
  timer_slot_t *slot = &s_ctx.slots[(uint32_t)timer_id];
  if (slot->active) {
    esy_timer_mode_t mode = slot->mode;
    slot->active = false;
    schedule_locked(mode);
  }
  timer_unlock(key);
}

void esy_hb_timer_cancel_all(void) {
  if (!s_ctx.initialized) return;
  uint32_t key = timer_lock();
  for (uint32_t i = 0u; i < MAX_TIMER_TASKS; ++i)
    s_ctx.slots[i].active = false;
  schedule_locked(ESY_TIMER_MODE_MS);
  schedule_locked(ESY_TIMER_MODE_US);
  timer_unlock(key);
}

static void dispatch_timer_interrupt(esy_timer_mode_t mode) {
  if (!s_ctx.initialized) return;
  timer_domain_t *domain = &s_ctx.domains[(uint32_t)mode];
  uint32_t key = timer_lock();
  if (domain->dispatching ||
      (domain->htim.Instance->SR & TIM_SR_CC1IF) == 0u ||
      (domain->htim.Instance->DIER & TIM_DIER_CC1IE) == 0u) {
    timer_unlock(key);
    return;
  }
  domain->htim.Instance->DIER &= ~TIM_DIER_CC1IE;
  __HAL_TIM_CLEAR_FLAG(&domain->htim, TIM_FLAG_CC1);
  domain->dispatching = true;
  timer_unlock(key);

  /* At most one delivery per slot per IRQ. State is advanced before callback;
   * never touch that slot afterward, since the callback may cancel and reuse it. */
  uint32_t dispatched = 0u;
  for (uint32_t count = 0u; count < MAX_TIMER_TASKS; ++count) {
    esy_timer_callback_t callback = NULL;
    void *callback_arg = NULL;
    key = timer_lock();
    const uint32_t now = domain->htim.Instance->CNT;
    for (uint32_t i = 0u; i < MAX_TIMER_TASKS; ++i) {
      timer_slot_t *slot = &s_ctx.slots[i];
      if ((dispatched & (1u << i)) != 0u || !slot->active ||
          slot->mode != mode || !deadline_reached(now, slot->deadline)) continue;
      dispatched |= 1u << i;
      callback = slot->callback;
      callback_arg = slot->callback_arg;
      if (slot->repeat) {
        uint32_t periods = (now - slot->deadline) / slot->interval_us + 1u;
        slot->deadline += periods * slot->interval_us;
      } else {
        slot->active = false;
      }
      break;
    }
    timer_unlock(key);
    if (callback == NULL) break;
    callback(callback_arg);
  }
  key = timer_lock();
  schedule_locked(mode);
  domain->dispatching = false;
  timer_unlock(key);
}

void esy_hb_timer_handle_ms_irq(void) {
  dispatch_timer_interrupt(ESY_TIMER_MODE_MS);
}

void esy_hb_timer_handle_us_irq(void) {
  dispatch_timer_interrupt(ESY_TIMER_MODE_US);
}

uint8_t esy_hb_timer_get_max_timer_task(void) {
  return MAX_TIMER_TASKS;
}

bool esy_hb_timer_is_active(int32_t timer_id) {
  if (timer_id < 0 || (uint32_t)timer_id >= MAX_TIMER_TASKS ||
      !s_ctx.initialized) return false;
  uint32_t key = timer_lock();
  bool active = s_ctx.slots[(uint32_t)timer_id].active;
  timer_unlock(key);
  return active;
}
