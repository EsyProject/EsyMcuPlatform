/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_adc.h"
#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_hb_stm32f4_dma_internal.h"

#include <string.h>

#define STM32F4_ADC_MAX           4u
#define STM32F4_ADC_SCAN_MAX      16u
#define STM32F4_ADC_SAMPLE_BYTES  2u
#define STM32F4_ADC_POLL_MS       10u

typedef struct {
  ADC_HandleTypeDef hadc;
  esy_adc_t *bound_handle;
  esy_adc_sample_time_t sample_time;
  uint32_t ext_trig_edge;
  uint32_t ext_trig_src;
  esy_dma_config_t dma;
  uint8_t phys_index;
  uint8_t channel;
  bool initialized;
  bool dma_owned;
  bool dma_active;
  bool stopping;
  bool error_pending;
} stm32f4_adc_slot_t;

typedef struct {
  stm32f4_adc_slot_t slots[STM32F4_ADC_MAX];
} esy_hb_adc_ctx_t;

static esy_hb_adc_ctx_t s_ctx;

/* One task/startup owner serializes lifecycle. Its ADC/DMA IRQs share callback
 * publication; only metadata/register transitions use local masking. Vendor
 * initialization, task-side DMA retirement and client callbacks stay outside. */
static uint32_t adc_lock(void) {
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  return key;
}

static void adc_unlock(uint32_t key) { __set_PRIMASK(key); }

static ADC_TypeDef *decode_adc(uint8_t index) {
  switch (index) {
    case 1u: return ADC1;
#ifdef ADC2
    case 2u: return ADC2;
#endif
#ifdef ADC3
    case 3u: return ADC3;
#endif
    default: return NULL;
  }
}

static stm32f4_adc_slot_t *adc_context(const esy_adc_t *adc) {
  if (adc == NULL || adc->ctx == NULL) return NULL;
  stm32f4_adc_slot_t *slot = adc->ctx;
  return slot->initialized && slot->bound_handle == adc ? slot : NULL;
}

static stm32f4_adc_slot_t *adc_for_native(const ADC_HandleTypeDef *hadc) {
  for (uint32_t i = 0u; i < STM32F4_ADC_MAX; ++i) {
    stm32f4_adc_slot_t *slot = &s_ctx.slots[i];
    if (slot->initialized && &slot->hadc == hadc) return slot;
  }
  return NULL;
}

static uint32_t sample_time_to_hal(esy_adc_sample_time_t time) {
  switch (time) {
    case ESY_ADC_SAMPLETIME_3CYCLES: return ADC_SAMPLETIME_3CYCLES;
    case ESY_ADC_SAMPLETIME_15CYCLES: return ADC_SAMPLETIME_15CYCLES;
    case ESY_ADC_SAMPLETIME_28CYCLES: return ADC_SAMPLETIME_28CYCLES;
    case ESY_ADC_SAMPLETIME_56CYCLES: return ADC_SAMPLETIME_56CYCLES;
    case ESY_ADC_SAMPLETIME_84CYCLES: return ADC_SAMPLETIME_84CYCLES;
    case ESY_ADC_SAMPLETIME_112CYCLES: return ADC_SAMPLETIME_112CYCLES;
    case ESY_ADC_SAMPLETIME_144CYCLES: return ADC_SAMPLETIME_144CYCLES;
    default: return ADC_SAMPLETIME_480CYCLES;
  }
}

static esy_err_t configure_sequence(stm32f4_adc_slot_t *slot,
                                    const uint8_t *channels, uint8_t count,
                                    bool continuous, bool circular_dma) {
  ADC_HandleTypeDef *hadc = &slot->hadc;
  if (hadc->State != HAL_ADC_STATE_RESET && HAL_ADC_Stop(hadc) != HAL_OK)
    return ESY_ERR_IO;
  hadc->Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc->Init.Resolution = ADC_RESOLUTION_12B;
  hadc->Init.ScanConvMode = count > 1u ? ENABLE : DISABLE;
  hadc->Init.ContinuousConvMode = continuous ? ENABLE : DISABLE;
  hadc->Init.DiscontinuousConvMode = DISABLE;
  hadc->Init.NbrOfDiscConversion = 0u;
  hadc->Init.ExternalTrigConvEdge = slot->ext_trig_edge;
  hadc->Init.ExternalTrigConv = slot->ext_trig_src;
  hadc->Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc->Init.NbrOfConversion = count;
  hadc->Init.DMAContinuousRequests = circular_dma ? ENABLE : DISABLE;
  hadc->Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(hadc) != HAL_OK) return ESY_ERR_IO;
  for (uint8_t i = 0u; i < count; ++i) {
    ADC_ChannelConfTypeDef channel = {
        .Channel = channels[i], .Rank = (uint32_t)i + 1u,
        .SamplingTime = sample_time_to_hal(slot->sample_time),
    };
    if (HAL_ADC_ConfigChannel(hadc, &channel) != HAL_OK) return ESY_ERR_IO;
  }
  return ESY_OK;
}

static void stop_requests(stm32f4_adc_slot_t *slot) {
  CLEAR_BIT(slot->hadc.Instance->CR2,
            ADC_CR2_EXTEN | ADC_CR2_DMA | ADC_CR2_DDS | ADC_CR2_ADON);
  __HAL_ADC_DISABLE_IT(&slot->hadc, ADC_IT_OVR);
  slot->hadc.State = HAL_ADC_STATE_READY;
}

static void clear_stream_flags(DMA_HandleTypeDef *handle) {
  __HAL_DMA_CLEAR_FLAG(handle, __HAL_DMA_GET_TC_FLAG_INDEX(handle));
  __HAL_DMA_CLEAR_FLAG(handle, __HAL_DMA_GET_HT_FLAG_INDEX(handle));
  __HAL_DMA_CLEAR_FLAG(handle, __HAL_DMA_GET_TE_FLAG_INDEX(handle));
  __HAL_DMA_CLEAR_FLAG(handle, __HAL_DMA_GET_DME_FLAG_INDEX(handle));
  __HAL_DMA_CLEAR_FLAG(handle, __HAL_DMA_GET_FE_FLAG_INDEX(handle));
}

/* ISR retirement never polls or invokes the task-only DMA abort. If hardware
 * still owns the stream, retain the buffer/lease and finish from task abort. */
static bool retire_from_isr(stm32f4_adc_slot_t *slot) {
  stop_requests(slot);
  DMA_HandleTypeDef *handle = slot->hadc.DMA_Handle;
  __HAL_DMA_DISABLE(handle);
  __HAL_DMA_DISABLE_IT(handle, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE | DMA_IT_DME);
  __HAL_DMA_DISABLE_IT(handle, DMA_IT_FE);
  if ((handle->Instance->CR & DMA_SxCR_EN) != 0u) return false;
  clear_stream_flags(handle);
  HAL_NVIC_ClearPendingIRQ((IRQn_Type)slot->dma.inner.irq_index);
  handle->XferCpltCallback = NULL;
  handle->XferHalfCpltCallback = NULL;
  handle->XferErrorCallback = NULL;
  handle->Parent = NULL;
  handle->State = HAL_DMA_STATE_READY;
  handle->Lock = HAL_UNLOCKED;
  slot->hadc.DMA_Handle = NULL;
  slot->dma_owned = false;
  slot->dma_active = false;
  return true;
}

esy_err_t esy_hb_adc_check_available(uint8_t phys_index) {
  if (phys_index >= STM32F4_ADC_MAX || decode_adc(phys_index) == NULL)
    return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  return s_ctx.slots[phys_index].initialized ? ESY_ERR_BUSY : ESY_OK;
}

esy_err_t esy_hb_adc_init(esy_adc_t *adc, const esy_adc_config_t *config) {
  if (adc == NULL || config == NULL ||
      config->phys_index >= STM32F4_ADC_MAX ||
      !IS_ADC_CHANNEL(config->inner.channel) ||
      (uint32_t)config->sample_time > ESY_ADC_SAMPLETIME_480CYCLES)
    return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u || adc->ctx != NULL) return ESY_ERR_STATE;
  esy_err_t available = esy_hb_adc_check_available(config->phys_index);
  if (available != ESY_OK) return available;
  ADC_TypeDef *instance = decode_adc(config->phys_index);
  stm32f4_adc_slot_t *slot = &s_ctx.slots[config->phys_index];
  memset(slot, 0, sizeof(*slot));
  slot->hadc.Instance = instance;
  slot->phys_index = config->phys_index;
  slot->channel = config->inner.channel;
  slot->sample_time = config->sample_time;
  slot->ext_trig_src = ADC_SOFTWARE_START;
  slot->ext_trig_edge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  slot->bound_handle = adc;
  slot->initialized = true;
  adc->ctx = slot;
  return configure_sequence(slot, &slot->channel, 1u, false, false);
}

esy_err_t esy_hb_adc_abort_dma(esy_adc_t *adc) {
  if (adc == NULL) return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  stm32f4_adc_slot_t *slot = adc_context(adc);
  if (slot == NULL) return adc->ctx == NULL ? ESY_OK : ESY_ERR_STATE;
  uint32_t key = adc_lock();
  if (!slot->dma_owned) {
    adc_unlock(key);
    return ESY_OK;
  }
  slot->stopping = true;
  stop_requests(slot);
  adc_unlock(key);
  esy_err_t result = esy_hb_dma_abort(slot, slot->dma.inner.dreq,
                                     slot->dma.inner.channel);
  key = adc_lock();
  if (result != ESY_OK) {
    slot->stopping = false;
    adc_unlock(key);
    return result;
  }
  esy_dma_callback_t error = slot->error_pending ? slot->dma.error_callback : NULL;
  void *error_arg = slot->dma.error_callback_arg;
  HAL_NVIC_ClearPendingIRQ((IRQn_Type)slot->dma.inner.irq_index);
  slot->hadc.DMA_Handle = NULL;
  slot->dma_owned = false;
  slot->dma_active = false;
  slot->stopping = false;
  slot->error_pending = false;
  memset(&slot->dma, 0, sizeof(slot->dma));
  adc_unlock(key);
  if (error != NULL) error(error_arg);
  return ESY_OK;
}

esy_err_t esy_hb_adc_deinit(esy_adc_t *adc) {
  if (adc == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_adc_slot_t *slot = adc_context(adc);
  if (slot == NULL) return adc->ctx == NULL ? ESY_OK : ESY_ERR_STATE;
  esy_err_t result = esy_hb_adc_abort_dma(adc);
  if (result != ESY_OK) return result;
  if (HAL_ADC_Stop(&slot->hadc) != HAL_OK ||
      HAL_ADC_DeInit(&slot->hadc) != HAL_OK) return ESY_ERR_IO;
  memset(slot, 0, sizeof(*slot));
  adc->ctx = NULL;
  return ESY_OK;
}

bool esy_hb_adc_is_dma_busy(const esy_adc_t *adc) {
  stm32f4_adc_slot_t *slot = adc_context(adc);
  return slot != NULL && (slot->dma_owned || slot->dma_active);
}

int32_t esy_hb_adc_sample_once(esy_adc_t *adc) {
  stm32f4_adc_slot_t *slot = adc_context(adc);
  if (slot == NULL) return ESY_ERR_STATE;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  if (slot->dma_owned) return ESY_ERR_BUSY;
  esy_err_t result = configure_sequence(slot, &slot->channel, 1u, false, false);
  if (result != ESY_OK) return result;
  HAL_StatusTypeDef status = HAL_ADC_Start(&slot->hadc);
  if (status == HAL_OK)
    status = HAL_ADC_PollForConversion(&slot->hadc, STM32F4_ADC_POLL_MS);
  int32_t sample = status == HAL_OK ? (int32_t)HAL_ADC_GetValue(&slot->hadc)
                                   : (status == HAL_TIMEOUT ? ESY_ERR_TIMEOUT
                                                            : ESY_ERR_IO);
  if (HAL_ADC_Stop(&slot->hadc) != HAL_OK) return ESY_ERR_IO;
  return sample;
}

static esy_err_t start_dma(esy_adc_t *adc, const esy_dma_config_t *config,
                            const uint8_t *channels, uint8_t count,
                            bool circular) {
  if (adc == NULL || config == NULL || config->dst == NULL ||
      channels == NULL || count == 0u || count > STM32F4_ADC_SCAN_MAX ||
      config->length == 0u || config->length % STM32F4_ADC_SAMPLE_BYTES != 0u ||
      config->length / STM32F4_ADC_SAMPLE_BYTES > UINT16_MAX ||
      config->length / STM32F4_ADC_SAMPLE_BYTES % count != 0u ||
      (uintptr_t)config->dst % STM32F4_ADC_SAMPLE_BYTES != 0u ||
      (!circular && config->circular) ||
      config->inner.irq_priority >= (1u << __NVIC_PRIO_BITS))
    return ESY_ERR_INVALID_ARG;
  for (uint8_t i = 0u; i < count; ++i)
    if (!IS_ADC_CHANNEL(channels[i])) return ESY_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u) return ESY_ERR_STATE;
  stm32f4_adc_slot_t *slot = adc_context(adc);
  if (slot == NULL) return ESY_ERR_STATE;
  if (slot->dma_owned ||
      esy_hb_dma_is_busy(config->inner.dreq, config->inner.channel))
    return ESY_ERR_BUSY;
  for (uint32_t i = 0u; i < STM32F4_ADC_MAX; ++i) {
    if (s_ctx.slots[i].dma_active &&
        s_ctx.slots[i].dma.inner.irq_priority != config->inner.irq_priority)
      return ESY_ERR_BUSY;
  }
  const bool repeated = slot->ext_trig_edge == ADC_EXTERNALTRIGCONVEDGE_NONE &&
      (circular || config->length / STM32F4_ADC_SAMPLE_BYTES > count);
  esy_err_t result = configure_sequence(slot, channels, count, repeated, circular);
  if (result != ESY_OK) return result;
  esy_dma_config_t transfer = *config;
  transfer.src = (const void *)&slot->hadc.Instance->DR;
  transfer.transfer_size = ESY_DMA_TRANSFER_SIZE_16;
  transfer.inner.direction = ESY_DMA_PERIPH_TO_MEM;
  transfer.circular = circular;
  result = esy_hb_dma_prepare(slot, &transfer);
  if (result != ESY_OK) return result;
  DMA_HandleTypeDef *handle = esy_hb_stm32f4_dma_get_handle(
      slot, transfer.inner.dreq, transfer.inner.channel);
  if (handle == NULL) {
    (void)esy_hb_dma_abort(slot, transfer.inner.dreq, transfer.inner.channel);
    return ESY_ERR_STATE;
  }
  uint32_t key = adc_lock();
  slot->dma = transfer;
  slot->dma_owned = true;
  slot->dma_active = true;
  slot->error_pending = false;
  slot->stopping = false;
  __HAL_LINKDMA(&slot->hadc, DMA_Handle, *handle);
  HAL_NVIC_SetPriority(ADC_IRQn, transfer.inner.irq_priority, 0u);
  HAL_NVIC_EnableIRQ(ADC_IRQn);
  adc_unlock(key);
  /* The native ADC provider starts this exact generic-owned stream once. */
  HAL_StatusTypeDef status = HAL_ADC_Start_DMA(&slot->hadc,
      (uint32_t *)transfer.dst,
      (uint32_t)(transfer.length / STM32F4_ADC_SAMPLE_BYTES));
  if (status != HAL_OK) {
    result = esy_hb_adc_abort_dma(adc);
    return result != ESY_OK ? result : ESY_ERR_IO;
  }
  if (transfer.half_callback == NULL) __HAL_DMA_DISABLE_IT(handle, DMA_IT_HT);
  return ESY_OK;
}

esy_err_t esy_hb_adc_transfer_dma(esy_adc_t *adc, esy_dma_config_t *config) {
  stm32f4_adc_slot_t *slot = adc_context(adc);
  if (slot == NULL) return ESY_ERR_STATE;
  return start_dma(adc, config, &slot->channel, 1u, false);
}

esy_err_t esy_hb_adc_transfer_dma_multi_channel(esy_adc_t *adc,
    esy_dma_config_t *config, const uint8_t *channels, uint8_t count) {
  return start_dma(adc, config, channels, count, false);
}

esy_err_t esy_hb_adc_start_continuous_dma(esy_adc_t *adc,
                                         esy_dma_config_t *config) {
  stm32f4_adc_slot_t *slot = adc_context(adc);
  if (slot == NULL) return ESY_ERR_STATE;
  return start_dma(adc, config, &slot->channel, 1u, true);
}

esy_err_t esy_hb_adc_set_trigger_source(esy_adc_t *adc,
                                         esy_adc_trigger_edge_t edge,
                                         esy_adc_trigger_source_t source) {
  stm32f4_adc_slot_t *slot = adc_context(adc);
  if (slot == NULL) return ESY_ERR_STATE;
  if (slot->dma_owned) return ESY_ERR_BUSY;
  uint32_t native_edge;
  switch (edge) {
    case ESY_ADC_TRIGGER_EDGE_NONE: native_edge = ADC_EXTERNALTRIGCONVEDGE_NONE; break;
    case ESY_ADC_TRIGGER_EDGE_RISING: native_edge = ADC_EXTERNALTRIGCONVEDGE_RISING; break;
    case ESY_ADC_TRIGGER_EDGE_FALLING: native_edge = ADC_EXTERNALTRIGCONVEDGE_FALLING; break;
    case ESY_ADC_TRIGGER_EDGE_BOTH: native_edge = ADC_EXTERNALTRIGCONVEDGE_RISINGFALLING; break;
    default: return ESY_ERR_INVALID_ARG;
  }
  uint32_t native_source;
  switch (source) {
    case ESY_ADC_TRIGGER_SRC_SOFTWARE: native_source = ADC_SOFTWARE_START; break;
    case ESY_ADC_TRIGGER_SRC_TIM2_TRGO: native_source = ADC_EXTERNALTRIGCONV_T2_TRGO; break;
    case ESY_ADC_TRIGGER_SRC_TIM3_TRGO: native_source = ADC_EXTERNALTRIGCONV_T3_TRGO; break;
#ifdef ADC_EXTERNALTRIGCONV_T4_TRGO
    case ESY_ADC_TRIGGER_SRC_TIM4_TRGO: native_source = ADC_EXTERNALTRIGCONV_T4_TRGO; break;
#endif
#ifdef ADC_EXTERNALTRIGCONV_T8_TRGO
    case ESY_ADC_TRIGGER_SRC_TIM8_TRGO: native_source = ADC_EXTERNALTRIGCONV_T8_TRGO; break;
#endif
#ifdef ADC_EXTERNALTRIGCONV_EXT_IT11
    case ESY_ADC_TRIGGER_SRC_EXTI11: native_source = ADC_EXTERNALTRIGCONV_EXT_IT11; break;
#endif
    default: return ESY_ERR_NOT_SUPPORTED;
  }
  if ((source == ESY_ADC_TRIGGER_SRC_SOFTWARE) !=
      (edge == ESY_ADC_TRIGGER_EDGE_NONE)) return ESY_ERR_INVALID_ARG;
  slot->ext_trig_edge = native_edge;
  slot->ext_trig_src = native_source;
  return ESY_OK;
}

esy_err_t esy_hb_adc_get_caps(const esy_adc_t *adc, uint32_t *caps) {
  if (adc_context(adc) == NULL || caps == NULL) return ESY_ERR_INVALID_ARG;
  *caps = ESY_ADC_CAP_DMA | ESY_ADC_CAP_SCAN | ESY_ADC_CAP_EXTERNAL_TRIGGER |
          ESY_ADC_CAP_CIRCULAR_DMA;
  return ESY_OK;
}

esy_err_t esy_hb_adc_get_phys_index(const esy_adc_t *adc, uint8_t *index) {
  stm32f4_adc_slot_t *slot = adc_context(adc);
  if (slot == NULL || index == NULL) return ESY_ERR_INVALID_ARG;
  *index = slot->phys_index;
  return ESY_OK;
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc) {
  stm32f4_adc_slot_t *slot = adc_for_native(hadc);
  if (slot != NULL && slot->dma_active && !slot->stopping &&
      !slot->error_pending &&
      slot->hadc.DMA_Handle->ErrorCode == HAL_DMA_ERROR_NONE &&
      slot->dma.half_callback != NULL)
    slot->dma.half_callback(slot->dma.half_callback_arg);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc) {
  stm32f4_adc_slot_t *slot = adc_for_native(hadc);
  if (slot == NULL || !slot->dma_active || slot->stopping) return;
  uint32_t key = adc_lock();
  slot->error_pending = true;
  bool retired = retire_from_isr(slot);
  const uint32_t dreq = slot->dma.inner.dreq;
  const uint8_t channel = slot->dma.inner.channel;
  esy_dma_callback_t callback = retired ? slot->dma.error_callback : NULL;
  void *arg = slot->dma.error_callback_arg;
  if (retired) {
    slot->error_pending = false;
    memset(&slot->dma, 0, sizeof(slot->dma));
  }
  adc_unlock(key);
  if (callback != NULL) callback(arg);
  if (retired) (void)esy_hb_dma_release(slot, dreq, channel);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
  stm32f4_adc_slot_t *slot = adc_for_native(hadc);
  if (slot == NULL || !slot->dma_active || slot->stopping || slot->error_pending)
    return;
  if (hadc->ErrorCode != HAL_ADC_ERROR_NONE ||
      slot->hadc.DMA_Handle->ErrorCode != HAL_DMA_ERROR_NONE ||
      __HAL_ADC_GET_FLAG(hadc, ADC_FLAG_OVR) != RESET) {
    HAL_ADC_ErrorCallback(hadc);
    return;
  }
  uint32_t key = adc_lock();
  esy_dma_callback_t callback = slot->dma.callback;
  void *arg = slot->dma.callback_arg;
  const uint32_t dreq = slot->dma.inner.dreq;
  const uint8_t channel = slot->dma.inner.channel;
  const bool finite = !slot->dma.circular;
  bool retired = false;
  if (!slot->dma.circular) {
    if (!retire_from_isr(slot)) {
      slot->error_pending = true;
      callback = NULL;
    } else {
      retired = true;
      memset(&slot->dma, 0, sizeof(slot->dma));
    }
  }
  adc_unlock(key);
  if (callback != NULL) callback(arg);
  if (finite && retired) (void)esy_hb_dma_release(slot, dreq, channel);
}

void esy_hb_adc_handle_irq(void) {
  for (uint32_t i = 0u; i < STM32F4_ADC_MAX; ++i)
    if (s_ctx.slots[i].initialized && s_ctx.slots[i].dma_active)
      HAL_ADC_IRQHandler(&s_ctx.slots[i].hadc);
}
