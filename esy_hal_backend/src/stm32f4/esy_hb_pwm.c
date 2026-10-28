/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_pwm.h"
#include "esy_common/defs/peripheral/esy_pwm_defs.h"

#include <stdint.h>
#include <stdatomic.h>

#include "stm32f4xx_hal.h"

#include "esy_hal_backend/esy_hb_dma.h"

#define STM32F4_PWM_MAX   8

enum {
  PWM_DMA_DONE = 1u << 0,
  PWM_DMA_HALF = 1u << 1,
  PWM_DMA_ERROR = 1u << 2,
};

typedef struct {
  TIM_HandleTypeDef htim;
  uint8_t tim_id;
  uint32_t channel;
  uint8_t  phys_index;

  void (*handler)(void *);
  void *handler_arg;
  uint8_t irq_priority;

  uint32_t saved_ccr;
  bool enabled;
  bool initialized;

  bool dma_loop;
  bool dma_owned;
  uint32_t dma_dreq;
  uint8_t dma_channel;
  uint8_t dma_resource;
  uint32_t dma_request;
  uint32_t saved_ccds;
  atomic_uint dma_events;
  esy_dma_callback_t dma_callback;
  void *dma_callback_arg;
  esy_dma_callback_t dma_half_callback;
  void *dma_half_callback_arg;
  esy_dma_callback_t dma_error_callback;
  void *dma_error_callback_arg;

  esy_pwm_t *bound_pwm;
} stm32f4_pwm_ctx_t;

static stm32f4_pwm_ctx_t s_ctx[STM32F4_PWM_MAX] = {0};

static bool pwm_irqn(uint8_t tim_id, IRQn_Type *irqn) {
  switch (tim_id) {
    case 1: *irqn = TIM1_UP_TIM10_IRQn; return true;
    case 2: *irqn = TIM2_IRQn; return true;
    case 3: *irqn = TIM3_IRQn; return true;
    case 4: *irqn = TIM4_IRQn; return true;
    case 5: *irqn = TIM5_IRQn; return true;
#ifdef TIM8
    case 8: *irqn = TIM8_UP_TIM13_IRQn; return true;
#endif
#ifdef TIM9
    case 9: *irqn = TIM1_BRK_TIM9_IRQn; return true;
#endif
    default: return false;
  }
}

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

static uint32_t get_tim_clk_hz(uint8_t tim_id) {
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
  return timer_hz;
}

static esy_err_t compute_psc_arr(uint8_t tim_id, uint32_t target_hz,
                               uint32_t *out_psc, uint32_t *out_arr) {
  if (target_hz == 0u) return ESY_ERR_INVALID_ARG;
  const uint32_t timer_hz = get_tim_clk_hz(tim_id);
  if (timer_hz == 0u) return ESY_ERR_NOT_SUPPORTED;
  if (target_hz > timer_hz) return ESY_ERR_INVALID_ARG;

  bool wide = (tim_id == 2 || tim_id == 5); /* 32-bit timers */
  const uint64_t max_period = (uint64_t)(wide ? UINT32_MAX : UINT16_MAX) + 1u;
  const uint64_t ticks = (uint64_t)timer_hz / target_hz;
  const uint64_t prescaler_div = (ticks + max_period - 1u) / max_period;
  if (prescaler_div > (uint64_t)UINT16_MAX + 1u)
    return ESY_ERR_NOT_SUPPORTED;
  const uint64_t period = (uint64_t)timer_hz /
      ((uint64_t)target_hz * prescaler_div);
  if (period == 0u || period > max_period) return ESY_ERR_NOT_SUPPORTED;
  *out_psc = (uint32_t)(prescaler_div - 1u);
  *out_arr = (uint32_t)(period - 1u);
  return ESY_OK;
}

static esy_err_t compute_compare(const TIM_HandleTypeDef *htim, uint32_t arr,
                                 double duty, uint32_t *compare) {
  if (!(duty >= 0.0 && duty <= 1.0)) return ESY_ERR_INVALID_ARG;
  /* ARR+1 may be 2^32; keep it wide through duty scaling and range validation. */
  const double pulse = ((double)arr + 1.0) * duty;
  const uint64_t pulse_ticks = (uint64_t)pulse;
  const uint32_t limit = IS_TIM_32B_COUNTER_INSTANCE(htim->Instance)
                             ? UINT32_MAX : UINT16_MAX;
  if (pulse_ticks > limit) return ESY_ERR_NOT_SUPPORTED;
  *compare = (uint32_t)pulse_ticks;
  return ESY_OK;
}

/* DMA IRQs only touch timer registers and publish events. The serialized
 * owner retires generic DMA callback storage before releasing the borrow. */
static void pwm_dma_stop_request(stm32f4_pwm_ctx_t *c) {
  __HAL_TIM_DISABLE_DMA(&c->htim, c->dma_request);
  (void)HAL_TIM_PWM_Stop(&c->htim, c->channel);
}

static void pwm_dma_callback_wrapper(void *arg) {
  stm32f4_pwm_ctx_t *c = arg;
  if (!c->dma_loop) __HAL_TIM_DISABLE_DMA(&c->htim, c->dma_request);
  atomic_fetch_or_explicit(&c->dma_events, PWM_DMA_DONE, memory_order_release);
}

static void pwm_dma_half_wrapper(void *arg) {
  stm32f4_pwm_ctx_t *c = arg;
  atomic_fetch_or_explicit(&c->dma_events, PWM_DMA_HALF, memory_order_release);
}

static void pwm_dma_error_wrapper(void *arg) {
  stm32f4_pwm_ctx_t *c = arg;
  pwm_dma_stop_request(c);
  atomic_fetch_or_explicit(&c->dma_events, PWM_DMA_ERROR, memory_order_release);
}

static esy_err_t pwm_dma_retire(stm32f4_pwm_ctx_t *c, bool stop_output) {
  if (!c->dma_owned) return ESY_OK;
  __HAL_TIM_DISABLE_DMA(&c->htim, c->dma_request);
  if (stop_output) {
    pwm_dma_stop_request(c);
    c->enabled = false;
  }
  esy_err_t result = esy_hb_dma_abort(c, c->dma_dreq, c->dma_channel);
  if (result != ESY_OK) return result;
  MODIFY_REG(c->htim.Instance->CR2, TIM_CR2_CCDS, c->saved_ccds);
  c->dma_owned = false;
  return ESY_OK;
}

static void pwm_dma_clear(stm32f4_pwm_ctx_t *c) {
  c->dma_callback = NULL;
  c->dma_callback_arg = NULL;
  c->dma_half_callback = NULL;
  c->dma_half_callback_arg = NULL;
  c->dma_error_callback = NULL;
  c->dma_error_callback_arg = NULL;
  c->dma_loop = false;
  atomic_store_explicit(&c->dma_events, 0u, memory_order_relaxed);
}

esy_err_t esy_hb_pwm_check_available(uint8_t phys_index) {
  if (phys_index >= STM32F4_PWM_MAX) return ESY_ERR_INVALID_ARG;
  return s_ctx[phys_index].bound_pwm == NULL ? ESY_OK : ESY_ERR_BUSY;
}

extern esy_err_t esy_hb_pwm_init(esy_pwm_t *pwm,
                                     const esy_pwm_config_t *config) {
  if (!pwm || !config) return ESY_ERR_INVALID_ARG;
  if (config->phys_index >= STM32F4_PWM_MAX) return ESY_ERR_INVALID_ARG;

  TIM_TypeDef *tim = decode_tim(config->inner.tim_id);
  if (!tim) return ESY_ERR_INVALID_ARG;

  uint32_t psc, arr;
  esy_err_t result = compute_psc_arr(config->inner.tim_id, config->freq_hz,
                                    &psc, &arr);
  if (result != ESY_OK) return result;

  if (!IS_TIM_CCX_INSTANCE(tim, config->inner.channel))
    return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = &s_ctx[config->phys_index];
  if (c->bound_pwm != NULL) return ESY_ERR_BUSY;
  c->tim_id      = config->inner.tim_id;
  c->channel     = config->inner.channel;
  c->phys_index  = config->phys_index;
  c->handler     = NULL;
  c->handler_arg = NULL;
  c->enabled     = false;
  c->initialized = false;
  c->dma_owned = false;
  atomic_init(&c->dma_events, 0u);
  pwm_dma_clear(c);

  c->htim.Instance = tim;

  c->htim.Init.Prescaler         = psc;
  c->htim.Init.Period            = arr;
  c->htim.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  c->htim.Init.CounterMode       = TIM_COUNTERMODE_UP;
  c->htim.Init.RepetitionCounter = 0;

  c->bound_pwm = pwm;
  pwm->ctx = c;
  if (HAL_TIM_PWM_Init(&c->htim) != HAL_OK) return ESY_ERR_FAIL;

  esy_err_t r = esy_hb_pwm_set_duty(pwm, config->duty_cycle / 100.0f);
  if (r != ESY_OK) return r;

  r = esy_hb_pwm_set_polarity(pwm, config->start_low);
  if (r != ESY_OK) return r;

  c->initialized = true;
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_deinit(esy_pwm_t *pwm) {
  if (pwm == NULL) return ESY_ERR_INVALID_ARG;
  if (pwm->ctx == NULL) return ESY_OK;
  stm32f4_pwm_ctx_t *c = pwm->ctx;
  esy_err_t result = esy_hb_pwm_abort_dma(pwm);
  if (result != ESY_OK) return result;
  result = esy_hb_pwm_set_wrap_handler(pwm, NULL, NULL, 0u);
  if (result != ESY_OK) return result;
  if (HAL_TIM_PWM_Stop(&c->htim, c->channel) != HAL_OK ||
      HAL_TIM_PWM_DeInit(&c->htim) != HAL_OK) return ESY_ERR_FAIL;
  c->initialized = false;
  c->handler = NULL;
  c->handler_arg = NULL;
  c->bound_pwm = NULL;
  pwm->ctx = NULL;
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_set_duty(esy_pwm_t *pwm, float duty_cycle) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = (stm32f4_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&c->htim);
  uint32_t cmp;
  esy_err_t result = compute_compare(&c->htim, arr, (double)duty_cycle, &cmp);
  if (result != ESY_OK) return result;
  __HAL_TIM_SET_COMPARE(&c->htim, c->channel, cmp);
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz) {
  if (!pwm || !pwm->ctx || !freq_hz) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = (stm32f4_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  uint32_t old_arr = __HAL_TIM_GET_AUTORELOAD(&c->htim);
  double duty = (double)__HAL_TIM_GET_COMPARE(&c->htim, c->channel) /
      ((double)old_arr + 1.0);

  uint32_t psc, arr, cmp;
  esy_err_t result = compute_psc_arr(c->tim_id, freq_hz, &psc, &arr);
  if (result != ESY_OK) return result;
  result = compute_compare(&c->htim, arr, duty, &cmp);
  if (result != ESY_OK) return result;

  bool was_enabled = c->enabled;
  if (was_enabled) HAL_TIM_PWM_Stop(&c->htim, c->channel);

  __HAL_TIM_SET_PRESCALER(&c->htim, psc);
  __HAL_TIM_SET_AUTORELOAD(&c->htim, arr);
  __HAL_TIM_SET_COUNTER(&c->htim, 0);

  __HAL_TIM_SET_COMPARE(&c->htim, c->channel, cmp);

  /* Force update event to reload PSC/ARR immediately */
  c->htim.Instance->EGR = TIM_EGR_UG;

  if (was_enabled) {
    if (HAL_TIM_PWM_Start(&c->htim, c->channel) == HAL_OK) {
      c->enabled = true;
    } else {
      c->enabled = false;
      return ESY_ERR_FAIL;
    }
  }
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_enable(esy_pwm_t *pwm, bool enable) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = (stm32f4_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  if (enable) {
    if (HAL_TIM_PWM_Start(&c->htim, c->channel) != HAL_OK) return ESY_ERR_FAIL;
    c->enabled = true;
  } else {
    HAL_TIM_PWM_Stop(&c->htim, c->channel);
    c->enabled = false;
  }
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_set_polarity(esy_pwm_t *pwm, bool start_low) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = (stm32f4_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  bool was_enabled = c->enabled;
  if (was_enabled) HAL_TIM_PWM_Stop(&c->htim, c->channel);

  TIM_OC_InitTypeDef oc = {0};
  oc.OCMode       = TIM_OCMODE_PWM1;
  oc.Pulse        = __HAL_TIM_GET_COMPARE(&c->htim, c->channel);
  oc.OCPolarity   = start_low ? TIM_OCPOLARITY_LOW : TIM_OCPOLARITY_HIGH;
  oc.OCFastMode   = TIM_OCFAST_DISABLE;

  if (HAL_TIM_PWM_ConfigChannel(&c->htim, &oc, c->channel) != HAL_OK) {
    return ESY_ERR_FAIL;
  }

  if (was_enabled) {
    if (HAL_TIM_PWM_Start(&c->htim, c->channel) != HAL_OK) return ESY_ERR_FAIL;
    c->enabled = true;
  }
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_enable_trigger_output(esy_pwm_t *pwm, bool enabled) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = (stm32f4_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  TIM_MasterConfigTypeDef cfg = {0};
  cfg.MasterOutputTrigger = enabled ? TIM_TRGO_UPDATE : TIM_TRGO_RESET;
  cfg.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;

  return (HAL_TIMEx_MasterConfigSynchronization(&c->htim, &cfg) == HAL_OK)
           ? ESY_OK : ESY_ERR_FAIL;
}

extern esy_err_t esy_hb_pwm_dma_output(esy_pwm_t *pwm,
                                       esy_dma_config_t *dma_config,
                                       bool loop) {
  if (!pwm || !pwm->ctx || !dma_config || !dma_config->src ||
      dma_config->length == 0u) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;
  if (!IS_TIM_CCDMA_INSTANCE(c->htim.Instance)) return ESY_ERR_NOT_SUPPORTED;
  if (dma_config->transfer_size != ESY_DMA_TRANSFER_SIZE_16 &&
      dma_config->transfer_size != ESY_DMA_TRANSFER_SIZE_32)
    return ESY_ERR_INVALID_ARG;
  if (dma_config->transfer_size == ESY_DMA_TRANSFER_SIZE_32 &&
      !IS_TIM_32B_COUNTER_INSTANCE(c->htim.Instance))
    return ESY_ERR_NOT_SUPPORTED;
  const size_t bytes = (size_t)1u << dma_config->transfer_size;
  if (dma_config->length % bytes != 0u ||
      dma_config->length / bytes > UINT16_MAX) return ESY_ERR_INVALID_ARG;

  void *dst;
  uint32_t request;
  switch (c->channel) {
    case TIM_CHANNEL_1:
      dst = (void *)&c->htim.Instance->CCR1; request = TIM_DMA_CC1; break;
    case TIM_CHANNEL_2:
      dst = (void *)&c->htim.Instance->CCR2; request = TIM_DMA_CC2; break;
    case TIM_CHANNEL_3:
      dst = (void *)&c->htim.Instance->CCR3; request = TIM_DMA_CC3; break;
    case TIM_CHANNEL_4:
      dst = (void *)&c->htim.Instance->CCR4; request = TIM_DMA_CC4; break;
    default: return ESY_ERR_INVALID_ARG;
  }
  uint8_t resource;
  esy_err_t result = esy_hb_dma_get_resource_id(dma_config->inner.dreq,
                                               dma_config->inner.channel,
                                               &resource);
  if (result != ESY_OK) return result;
  for (size_t i = 0; i < STM32F4_PWM_MAX; ++i) {
    if (s_ctx[i].dma_owned && s_ctx[i].dma_resource == resource)
      return ESY_ERR_BUSY;
  }
  if (esy_hb_dma_is_busy(dma_config->inner.dreq, dma_config->inner.channel))
    return ESY_ERR_BUSY;
  if (HAL_TIM_PWM_Stop(&c->htim, c->channel) != HAL_OK) return ESY_ERR_FAIL;
  c->enabled = false;
  dma_config->dst = dst;
  dma_config->inner.direction = ESY_DMA_MEM_TO_PERIPH;
  dma_config->circular = loop || dma_config->circular;
  c->dma_loop = dma_config->circular;
  c->dma_callback = dma_config->callback;
  c->dma_callback_arg = dma_config->callback_arg;
  c->dma_half_callback = dma_config->half_callback;
  c->dma_half_callback_arg = dma_config->half_callback_arg;
  c->dma_error_callback = dma_config->error_callback;
  c->dma_error_callback_arg = dma_config->error_callback_arg;
  dma_config->callback = pwm_dma_callback_wrapper;
  dma_config->callback_arg = c;
  dma_config->half_callback = pwm_dma_half_wrapper;
  dma_config->half_callback_arg = c;
  dma_config->error_callback = pwm_dma_error_wrapper;
  dma_config->error_callback_arg = c;
  atomic_store_explicit(&c->dma_events, 0u, memory_order_relaxed);
  result = esy_hb_dma_prepare(c, dma_config);
  if (result != ESY_OK) {
    pwm_dma_clear(c);
    return result;
  }
  c->dma_dreq = dma_config->inner.dreq;
  c->dma_channel = dma_config->inner.channel;
  c->dma_resource = resource;
  c->dma_request = request;
  c->saved_ccds = c->htim.Instance->CR2 & TIM_CR2_CCDS;
  c->dma_owned = true;
  __HAL_TIM_DISABLE_DMA(&c->htim, request);
  /* Update events guarantee progress even when the compare is 0 or ARR+1.
   * CCDS belongs to the whole timer, which has one configuration owner. */
  MODIFY_REG(c->htim.Instance->CR2, TIM_CR2_CCDS, TIM_CCDMAREQUEST_UPDATE);
  result = esy_hb_dma_start(c, 1u << resource);
  if (result == ESY_OK &&
      HAL_TIM_PWM_Start(&c->htim, c->channel) != HAL_OK) result = ESY_ERR_FAIL;
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_hb_pwm_abort_dma(pwm);
    return cleanup == ESY_OK ? result : cleanup;
  }
  c->enabled = true;
  /* Publish the request last: an error IRQ must not be followed by a late
   * HAL_TIM_PWM_Start that would restart the failed output. */
  __HAL_TIM_ENABLE_DMA(&c->htim, request);
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_abort_dma(esy_pwm_t *pwm) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = pwm->ctx;
  esy_err_t result = pwm_dma_retire(c, true);
  if (result != ESY_OK) return result;
  pwm_dma_clear(c);
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_poll(esy_pwm_t *pwm) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = pwm->ctx;
  if (!c->dma_owned) return ESY_OK;
  unsigned events = atomic_load_explicit(&c->dma_events, memory_order_acquire);
  if ((events & PWM_DMA_ERROR) || (!c->dma_loop && (events & PWM_DMA_DONE))) {
    esy_err_t result = pwm_dma_retire(c, (events & PWM_DMA_ERROR) != 0u);
    if (result != ESY_OK) return result;
    events = atomic_load_explicit(&c->dma_events, memory_order_acquire);
    if (events & PWM_DMA_ERROR) {
      pwm_dma_stop_request(c);
      c->enabled = false;
    }
    esy_dma_callback_t callback = (events & PWM_DMA_ERROR)
        ? c->dma_error_callback : c->dma_callback;
    void *arg = (events & PWM_DMA_ERROR)
        ? c->dma_error_callback_arg : c->dma_callback_arg;
    pwm_dma_clear(c);
    if (callback != NULL) callback(arg);
    return ESY_OK;
  }
  /* A finite TC arriving after the first load must remain pending for the
   * retirement path on the next poll; never deliver it as a cycle event. */
  unsigned notifications = PWM_DMA_HALF | (c->dma_loop ? PWM_DMA_DONE : 0u);
  events = atomic_fetch_and_explicit(&c->dma_events,
      ~notifications, memory_order_acq_rel);
  if (events & PWM_DMA_ERROR) return ESY_OK;
  if ((events & PWM_DMA_HALF) && c->dma_half_callback != NULL)
    c->dma_half_callback(c->dma_half_callback_arg);
  if (c->dma_loop && (events & PWM_DMA_DONE) && c->dma_callback != NULL)
    c->dma_callback(c->dma_callback_arg);
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_set_wrap_handler(esy_pwm_t *pwm,
                                                 void (*handler)(void *),
                                                 void *arg,
                                                 uint8_t irq_priority) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = (stm32f4_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;
  IRQn_Type irqn;
  if (!pwm_irqn(c->tim_id, &irqn) ||
      (handler != NULL && irq_priority >= (1u << __NVIC_PRIO_BITS)))
    return ESY_ERR_INVALID_ARG;

  bool timer_peer = false;
  bool irq_peer = false;
  for (size_t i = 0u; i < STM32F4_PWM_MAX; ++i) {
    stm32f4_pwm_ctx_t *peer = &s_ctx[i];
    IRQn_Type peer_irqn;
    if (peer == c || !peer->initialized || peer->handler == NULL ||
        !pwm_irqn(peer->tim_id, &peer_irqn) || peer_irqn != irqn) continue;
    if (handler != NULL && peer->irq_priority != irq_priority)
      return ESY_ERR_BUSY;
    irq_peer = true;
    if (peer->htim.Instance == c->htim.Instance) timer_peer = true;
  }

  /* One task owns callback registration; the timer IRQ only snapshots it.
   * Exclude publication and the finite source/NVIC updates, never user code. */
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  if (!timer_peer) {
    __HAL_TIM_DISABLE_IT(&c->htim, TIM_IT_UPDATE);
    __HAL_TIM_CLEAR_IT(&c->htim, TIM_IT_UPDATE);
  }
  c->handler = handler;
  c->handler_arg = handler != NULL ? arg : NULL;
  c->irq_priority = irq_priority;
  if (handler != NULL) {
    HAL_NVIC_SetPriority(irqn, irq_priority, 0);
    if (!irq_peer) HAL_NVIC_ClearPendingIRQ(irqn);
    HAL_NVIC_EnableIRQ(irqn);
    __HAL_TIM_ENABLE_IT(&c->htim, TIM_IT_UPDATE);
  } else if (!irq_peer) {
    HAL_NVIC_DisableIRQ(irqn);
    HAL_NVIC_ClearPendingIRQ(irqn);
  }
  __set_PRIMASK(key);
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_suspend(esy_pwm_t *pwm) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = (stm32f4_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  c->enabled   = (HAL_TIM_PWM_Stop(&c->htim, c->channel), c->enabled);
  c->saved_ccr = __HAL_TIM_GET_COMPARE(&c->htim, c->channel);
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_resume(esy_pwm_t *pwm) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  stm32f4_pwm_ctx_t *c = (stm32f4_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  __HAL_TIM_SET_COMPARE(&c->htim, c->channel, c->saved_ccr);
  if (c->enabled) {
    if (HAL_TIM_PWM_Start(&c->htim, c->channel) != HAL_OK) return ESY_ERR_FAIL;
  }
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_set_low_power_mode(esy_pwm_t *pwm,
                                                   bool enable_lprun) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  (void)enable_lprun;
  return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_pwm_set_clock_source(esy_pwm_t *pwm,
                                                 uint8_t clock_source) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  (void)clock_source;
  return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_pwm_set_dead_time(esy_pwm_t *pwm,
                                              uint32_t dead_time_ns) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  (void)dead_time_ns;
  return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_pwm_enable_complementary(esy_pwm_t *pwm,
                                                     bool enable) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  (void)enable;
  return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_pwm_get_caps(const esy_pwm_t *pwm, uint32_t *out_caps) {
  if (!pwm || !pwm->ctx || !out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_PWM_CAP_DMA_OUTPUT | ESY_PWM_CAP_WRAP_IRQ |
              ESY_PWM_CAP_POLARITY;
  return ESY_OK;
}

extern esy_err_t esy_hb_pwm_get_phys_index(const esy_pwm_t *pwm, uint8_t *out_index) {
  if (!pwm || !pwm->ctx || !out_index) return ESY_ERR_INVALID_ARG;
  const stm32f4_pwm_ctx_t *c = (const stm32f4_pwm_ctx_t *)pwm->ctx;
  *out_index = c->phys_index;
  return ESY_OK;
}

extern void esy_hb_pwm_handle_irq(uint32_t index) {
  if(index >= STM32F4_PWM_MAX) {
    return;
  }

  stm32f4_pwm_ctx_t *c = &s_ctx[index];
  if (!c->initialized) return;
  if (__HAL_TIM_GET_FLAG(&c->htim, TIM_FLAG_UPDATE) == RESET ||
      __HAL_TIM_GET_IT_SOURCE(&c->htim, TIM_IT_UPDATE) == RESET) return;
  /* A timer has one UPDATE source shared by its PWM channels. Acknowledge it
   * once, then notify every registered channel before another board forwarding
   * call can observe the cleared flag. Callbacks cannot reenter lifecycle. */
  __HAL_TIM_CLEAR_IT(&c->htim, TIM_IT_UPDATE);
  for (size_t i = 0u; i < STM32F4_PWM_MAX; ++i) {
    stm32f4_pwm_ctx_t *peer = &s_ctx[i];
    if (!peer->initialized || peer->htim.Instance != c->htim.Instance)
      continue;
    void (*handler)(void *) = peer->handler;
    void *arg = peer->handler_arg;
    if (handler != NULL) handler(arg);
  }
}
