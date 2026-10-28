/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_timer_capture.h"
#include "esy_common/defs/coordination/esy_atomic_defs.h"
#include <stdatomic.h>

#include <string.h>
#include "stm32f4xx_hal.h"

#define STM32F4_TC_MAX 8
#define STM32F4_TC_TICK_HZ 1000000u

typedef struct {
  uint8_t  phys_index;
  atomic_bool initialized;
  bool native_initialized;
  bool native_started;
  uint8_t gpio;
  esy_timer_capture_t *bound_handle;

  uint8_t  tim_id;
  uint32_t channel;
  bool     rising_edge;

  atomic_uint last_captured;
  atomic_uint last_interval;
  uint32_t previous_captured;
  bool has_capture;

  esy_timer_capture_callback_t callback;
  void *callback_arg;

  TIM_HandleTypeDef htim;
} stm32f4_tc_slot_t;

typedef struct {
  atomic_flag claim_lock;
  stm32f4_tc_slot_t slots[STM32F4_TC_MAX];
} stm32f4_tc_ctx_t;

static stm32f4_tc_ctx_t s_ctx = {.claim_lock = ATOMIC_FLAG_INIT};

static TIM_TypeDef *decode_tim(uint8_t tim_id) {
  switch (tim_id) {
    case 1: return TIM1;
    case 2: return TIM2;
    case 3: return TIM3;
    case 4: return TIM4;
    case 5: return TIM5;
#ifdef TIM8
    case 8: return TIM8;
#endif
#ifdef TIM9
    case 9: return TIM9;
#endif
    default: return NULL;
  }
}

static IRQn_Type get_tim_cc_irqn(uint8_t tim_id) {
  switch (tim_id) {
    case 1: return TIM1_CC_IRQn;
    case 2: return TIM2_IRQn;
    case 3: return TIM3_IRQn;
    case 4: return TIM4_IRQn;
    case 5: return TIM5_IRQn;
#ifdef TIM8
    case 8: return TIM8_CC_IRQn;
#endif
#ifdef TIM9
    case 9: return TIM1_BRK_TIM9_IRQn;
#endif
    default: return (IRQn_Type)-1;
  }
}

static uint32_t flag_by_ch(uint32_t ch) {
  switch (ch) {
    case TIM_CHANNEL_1: return TIM_FLAG_CC1;
    case TIM_CHANNEL_2: return TIM_FLAG_CC2;
    case TIM_CHANNEL_3: return TIM_FLAG_CC3;
    case TIM_CHANNEL_4: return TIM_FLAG_CC4;
    default: return 0;
  }
}

static uint32_t it_by_ch(uint32_t ch) {
  switch (ch) {
    case TIM_CHANNEL_1: return TIM_IT_CC1;
    case TIM_CHANNEL_2: return TIM_IT_CC2;
    case TIM_CHANNEL_3: return TIM_IT_CC3;
    case TIM_CHANNEL_4: return TIM_IT_CC4;
    default: return 0;
  }
}

static esy_err_t capture_prescaler(uint8_t tim_id, uint32_t *prescaler) {
  RCC_ClkInitTypeDef clocks;
  uint32_t flash_latency;
  bool apb2 = (tim_id == 1u || tim_id == 8u || tim_id == 9u);

  HAL_RCC_GetClockConfig(&clocks, &flash_latency);
  uint32_t divider = apb2 ? clocks.APB2CLKDivider : clocks.APB1CLKDivider;
  uint32_t timer_hz = apb2 ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
  if (divider != RCC_HCLK_DIV1) timer_hz *= 2u;
#ifdef RCC_DCKCFGR_TIMPRE
  if ((RCC->DCKCFGR & RCC_DCKCFGR_TIMPRE) != 0u) {
    if (divider == RCC_HCLK_DIV1 || divider == RCC_HCLK_DIV2 ||
        divider == RCC_HCLK_DIV4) {
      timer_hz = HAL_RCC_GetHCLKFreq();
    } else {
      timer_hz *= 2u;
    }
  }
#endif
  if (timer_hz < STM32F4_TC_TICK_HZ ||
      timer_hz % STM32F4_TC_TICK_HZ != 0u ||
      timer_hz / STM32F4_TC_TICK_HZ > UINT16_MAX + 1u) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  *prescaler = timer_hz / STM32F4_TC_TICK_HZ - 1u;
  return ESY_OK;
}

static void tc_cc_irq_dispatch(stm32f4_tc_slot_t *c) {
  uint32_t ch_flag = flag_by_ch(c->channel);
  uint32_t ch_it   = it_by_ch(c->channel);
  if (ch_flag && __HAL_TIM_GET_FLAG(&c->htim, ch_flag) &&
      __HAL_TIM_GET_IT_SOURCE(&c->htim, ch_it)) {

    __HAL_TIM_CLEAR_IT(&c->htim, ch_it);
    uint32_t val = HAL_TIM_ReadCapturedValue(&c->htim, c->channel);

    /* The ISR owns capture history; task queries read one published word. */
    uint32_t interval = c->has_capture
                            ? (val - c->previous_captured) & c->htim.Init.Period
                            : 0u;
    c->previous_captured = val;
    atomic_store_explicit(&c->last_interval, interval, memory_order_release);
    atomic_store_explicit(&c->last_captured, val, memory_order_release);
    c->has_capture = true;

    if (c->callback) {
      c->callback(val, c->callback_arg);
    }
  }
}

static stm32f4_tc_slot_t *capture_context(const esy_timer_capture_t *cap) {
  if (cap == NULL || cap->ctx == NULL) return NULL;
  for (uint32_t i = 0u; i < STM32F4_TC_MAX; ++i) {
    stm32f4_tc_slot_t *c = &s_ctx.slots[i];
    if (cap->ctx == c && c->bound_handle == cap) return c;
  }
  return NULL;
}

esy_err_t esy_hb_timer_capture_claim(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  if (cap == NULL || cfg == NULL || cfg->phys_index >= STM32F4_TC_MAX ||
      cfg->inner.gpio == ESY_PIN_UNUSED || cfg->inner.channel < 1u ||
      cfg->inner.channel > 4u) return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  TIM_TypeDef *timx = decode_tim(cfg->inner.tim_id);
  const uint32_t channel = cfg->inner.channel == 1u ? TIM_CHANNEL_1 :
                           cfg->inner.channel == 2u ? TIM_CHANNEL_2 :
                           cfg->inner.channel == 3u ? TIM_CHANNEL_3 : TIM_CHANNEL_4;
  if (timx == NULL || get_tim_cc_irqn(cfg->inner.tim_id) < 0 ||
      !IS_TIM_CCX_INSTANCE(timx, channel)) return ESY_ERR_INVALID_ARG;
  if (cap->ctx != NULL) {
    const stm32f4_tc_slot_t *c = capture_context(cap);
    return c != NULL && c->phys_index == cfg->phys_index &&
                   c->tim_id == cfg->inner.tim_id && c->channel == channel &&
                   c->gpio == cfg->inner.gpio && !c->initialized &&
                   !c->native_initialized && !c->native_started
               ? ESY_OK : ESY_ERR_STATE;
  }
  uint32_t prescaler;
  esy_err_t result = capture_prescaler(cfg->inner.tim_id, &prescaler);
  if (result != ESY_OK) return result;
  if (atomic_flag_test_and_set_explicit(&s_ctx.claim_lock, memory_order_acquire))
    return ESY_ERR_BUSY;
  for (uint32_t i = 0u; i < STM32F4_TC_MAX; ++i) {
    const stm32f4_tc_slot_t *other = &s_ctx.slots[i];
    if (other->bound_handle != NULL &&
        (i == cfg->phys_index || other->tim_id == cfg->inner.tim_id ||
         other->gpio == cfg->inner.gpio)) {
      atomic_flag_clear_explicit(&s_ctx.claim_lock, memory_order_release);
      return ESY_ERR_BUSY;
    }
  }
  stm32f4_tc_slot_t *c = &s_ctx.slots[cfg->phys_index];
  memset(c, 0, sizeof(*c));
  atomic_init(&c->initialized, false);
  atomic_init(&c->last_captured, 0u);
  atomic_init(&c->last_interval, 0u);
  c->phys_index = cfg->phys_index;
  c->tim_id = cfg->inner.tim_id;
  c->channel = channel;
  c->gpio = cfg->inner.gpio;
  c->bound_handle = cap;
  c->htim.Instance = timx;
  c->htim.Init.Prescaler = prescaler;
  c->htim.Init.CounterMode = TIM_COUNTERMODE_UP;
  c->htim.Init.Period = IS_TIM_32B_COUNTER_INSTANCE(timx)
                            ? UINT32_MAX : UINT16_MAX;
  c->htim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  c->htim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  cap->ctx = c;
  atomic_flag_clear_explicit(&s_ctx.claim_lock, memory_order_release);
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_init(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  esy_err_t result = esy_hb_timer_capture_claim(cap, cfg);
  if (result != ESY_OK) return result;
  stm32f4_tc_slot_t *c = capture_context(cap);
  const IRQn_Type irqn = get_tim_cc_irqn(c->tim_id);
  result = capture_prescaler(c->tim_id, &c->htim.Init.Prescaler);
  if (result != ESY_OK) return result;
  c->rising_edge = cfg->rising_edge;
  c->callback = cfg->callback;
  c->callback_arg = cfg->callback_arg;
  c->has_capture = false;
  c->previous_captured = 0u;
  atomic_store_explicit(&c->last_captured, 0u, memory_order_relaxed);
  atomic_store_explicit(&c->last_interval, 0u, memory_order_relaxed);
  /* Even failed native init may have called the board MSP hook. Keep its
   * reservation and native state until a successful cleanup retry. */
  c->native_initialized = true;
  if (HAL_TIM_IC_Init(&c->htim) != HAL_OK) return ESY_ERR_FAIL;
  TIM_IC_InitTypeDef ic = {
      .ICPolarity = c->rising_edge ? TIM_INPUTCHANNELPOLARITY_RISING
                                  : TIM_INPUTCHANNELPOLARITY_FALLING,
      .ICSelection = TIM_ICSELECTION_DIRECTTI,
      .ICPrescaler = TIM_ICPSC_DIV1,
      .ICFilter = 0,
  };
  if (HAL_TIM_IC_ConfigChannel(&c->htim, &ic, c->channel) != HAL_OK)
    return ESY_ERR_FAIL;
  __HAL_TIM_SET_COUNTER(&c->htim, 0u);
  __HAL_TIM_CLEAR_FLAG(&c->htim, flag_by_ch(c->channel));
  atomic_store_explicit(&c->initialized, true, memory_order_release);
  c->native_started = true;
  if (HAL_TIM_IC_Start_IT(&c->htim, c->channel) != HAL_OK) {
    atomic_store_explicit(&c->initialized, false, memory_order_release);
    return ESY_ERR_FAIL;
  }
  HAL_NVIC_SetPriority(irqn, cfg->inner.irq_priority, 0);
  HAL_NVIC_EnableIRQ(irqn);
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_suspend(esy_timer_capture_t *cap) {
  if (cap == NULL) return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  stm32f4_tc_slot_t *c = capture_context(cap);
  if (c == NULL) return ESY_ERR_STATE;
  if (!c->native_initialized && !c->native_started && !c->initialized)
    return ESY_OK;
  IRQn_Type irqn = get_tim_cc_irqn(c->tim_id);
  HAL_NVIC_DisableIRQ(irqn);
  atomic_store_explicit(&c->initialized, false, memory_order_release);
  if (c->native_started) {
    if (HAL_TIM_IC_Stop_IT(&c->htim, c->channel) != HAL_OK) return ESY_ERR_FAIL;
    c->native_started = false;
  }
  if (c->native_initialized) {
    if (HAL_TIM_IC_DeInit(&c->htim) != HAL_OK) return ESY_ERR_FAIL;
    c->native_initialized = false;
  }
  HAL_NVIC_ClearPendingIRQ(irqn);
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_deinit(esy_timer_capture_t *cap) {
  if (cap == NULL) return ESY_ERR_INVALID_ARG;
  if (cap->ctx == NULL) return ESY_OK;
  stm32f4_tc_slot_t *c = capture_context(cap);
  if (c == NULL) return ESY_ERR_STATE;
  esy_err_t result = esy_hb_timer_capture_suspend(cap);
  if (result != ESY_OK) return result;
  if (atomic_flag_test_and_set_explicit(&s_ctx.claim_lock, memory_order_acquire))
    return ESY_ERR_BUSY;
  memset(c, 0, sizeof(*c));
  cap->ctx = NULL;
  atomic_flag_clear_explicit(&s_ctx.claim_lock, memory_order_release);
  return ESY_OK;
}

extern esy_err_t esy_hb_timer_capture_get_value(const esy_timer_capture_t *cap,
                                                    uint32_t *value_us) {
  if (cap == NULL || value_us == NULL) return ESY_ERR_INVALID_ARG;
  const stm32f4_tc_slot_t *c = capture_context(cap);
  if (c == NULL || !c->initialized) return ESY_ERR_STATE;
  *value_us = atomic_load_explicit(&c->last_captured, memory_order_acquire);
  return ESY_OK;
}

extern esy_err_t esy_hb_timer_capture_get_interval(const esy_timer_capture_t *cap,
                                                       uint32_t *interval_us) {
  if (cap == NULL || interval_us == NULL) return ESY_ERR_INVALID_ARG;
  const stm32f4_tc_slot_t *c = capture_context(cap);
  if (c == NULL || !c->initialized) return ESY_ERR_STATE;
  *interval_us = atomic_load_explicit(&c->last_interval, memory_order_acquire);
  return ESY_OK;
}

extern esy_err_t esy_hb_timer_capture_get_phys_index(const esy_timer_capture_t *cap,
                                                         uint8_t *out_index) {
  if (cap == NULL || out_index == NULL) return ESY_ERR_INVALID_ARG;
  const stm32f4_tc_slot_t *c = capture_context(cap);
  if (c == NULL) return ESY_ERR_STATE;
  *out_index = c->phys_index;
  return ESY_OK;
}

extern esy_err_t esy_hb_timer_capture_get_caps(const esy_timer_capture_t *cap,
                                                   uint32_t *out_caps) {
  if (cap == NULL || out_caps == NULL) return ESY_ERR_INVALID_ARG;
  if (capture_context(cap) == NULL) return ESY_ERR_STATE;
  *out_caps = ESY_TIMER_CAPTURE_CAP_EDGE_CALLBACK |
              ESY_TIMER_CAPTURE_CAP_INTERVAL |
              ESY_TIMER_CAPTURE_CAP_HARDWARE_LATCH;
  return ESY_OK;
}

extern void esy_hb_timer_capture_handle_irq(uint32_t index) {
  if(index >= STM32F4_TC_MAX) {
    return;
  }

  stm32f4_tc_slot_t *c = &s_ctx.slots[index];
  if (!c->initialized) return;

  tc_cc_irq_dispatch(c);
}
