/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_dma.h"

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#include "stm32f4xx_hal.h"
#include "esy_hb_stm32f4_dma_internal.h"

#define MAX_DMA_DREQ 16u
#define DMA_RESOURCE_MASK ((1u << MAX_DMA_DREQ) - 1u)

typedef struct {
  DMA_TypeDef *controller;
  DMA_Stream_TypeDef *stream;
  IRQn_Type irq;
} dma_stream_info_t;

typedef struct {
  _Atomic(const void *) owners[MAX_DMA_DREQ];
  const dma_stream_info_t *stream_table;
  DMA_HandleTypeDef handles[MAX_DMA_DREQ];
  uint32_t dummy[MAX_DMA_DREQ];
  bool configured[MAX_DMA_DREQ];
  size_t unit_bytes[MAX_DMA_DREQ];
  esy_dma_callback_t callbacks[MAX_DMA_DREQ];
  void *callback_args[MAX_DMA_DREQ];
  esy_dma_callback_t half_callbacks[MAX_DMA_DREQ];
  void *half_callback_args[MAX_DMA_DREQ];
  esy_dma_callback_t error_callbacks[MAX_DMA_DREQ];
  void *error_callback_args[MAX_DMA_DREQ];
} esy_hb_dma_ctx_t;

static const dma_stream_info_t k_dma_stream_table[] = {
    {DMA1, DMA1_Stream0, DMA1_Stream0_IRQn},
    {DMA1, DMA1_Stream1, DMA1_Stream1_IRQn},
    {DMA1, DMA1_Stream2, DMA1_Stream2_IRQn},
    {DMA1, DMA1_Stream3, DMA1_Stream3_IRQn},
    {DMA1, DMA1_Stream4, DMA1_Stream4_IRQn},
    {DMA1, DMA1_Stream5, DMA1_Stream5_IRQn},
    {DMA1, DMA1_Stream6, DMA1_Stream6_IRQn},
    {DMA1, DMA1_Stream7, DMA1_Stream7_IRQn},
    {DMA2, DMA2_Stream0, DMA2_Stream0_IRQn},
    {DMA2, DMA2_Stream1, DMA2_Stream1_IRQn},
    {DMA2, DMA2_Stream2, DMA2_Stream2_IRQn},
    {DMA2, DMA2_Stream3, DMA2_Stream3_IRQn},
    {DMA2, DMA2_Stream4, DMA2_Stream4_IRQn},
    {DMA2, DMA2_Stream5, DMA2_Stream5_IRQn},
    {DMA2, DMA2_Stream6, DMA2_Stream6_IRQn},
    {DMA2, DMA2_Stream7, DMA2_Stream7_IRQn},
};

static esy_hb_dma_ctx_t s_ctx = {
    .stream_table = k_dma_stream_table,
};

static size_t dma_handle_index(const DMA_HandleTypeDef *hdma) {
  return (size_t)(hdma - s_ctx.handles);
}

static void dma_complete_callback(DMA_HandleTypeDef *hdma) {
  size_t index = dma_handle_index(hdma);
  if ((index < MAX_DMA_DREQ) && s_ctx.callbacks[index]) {
    s_ctx.callbacks[index](s_ctx.callback_args[index]);
  }
}

static void dma_half_complete_callback(DMA_HandleTypeDef *hdma) {
  size_t index = dma_handle_index(hdma);
  if ((index < MAX_DMA_DREQ) && s_ctx.half_callbacks[index]) {
    s_ctx.half_callbacks[index](s_ctx.half_callback_args[index]);
  }
}

static void dma_error_callback(DMA_HandleTypeDef *hdma) {
  size_t index = dma_handle_index(hdma);
  if ((index < MAX_DMA_DREQ) && s_ctx.error_callbacks[index]) {
    s_ctx.error_callbacks[index](s_ctx.error_callback_args[index]);
  }
}

static void dma_clear_all_flags(DMA_HandleTypeDef *hdma) {
  __HAL_DMA_CLEAR_FLAG(hdma, __HAL_DMA_GET_TC_FLAG_INDEX(hdma));
  __HAL_DMA_CLEAR_FLAG(hdma, __HAL_DMA_GET_HT_FLAG_INDEX(hdma));
  __HAL_DMA_CLEAR_FLAG(hdma, __HAL_DMA_GET_TE_FLAG_INDEX(hdma));
  __HAL_DMA_CLEAR_FLAG(hdma, __HAL_DMA_GET_DME_FLAG_INDEX(hdma));
  __HAL_DMA_CLEAR_FLAG(hdma, __HAL_DMA_GET_FE_FLAG_INDEX(hdma));
}

static void dma_disable_all_interrupts(DMA_HandleTypeDef *hdma) {
  __HAL_DMA_DISABLE_IT(hdma, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE | DMA_IT_DME);
  __HAL_DMA_DISABLE_IT(hdma, DMA_IT_FE);
}

static void dma_enable_error_interrupts(DMA_HandleTypeDef *hdma) {
  __HAL_DMA_ENABLE_IT(hdma, DMA_IT_TE | DMA_IT_DME);
  __HAL_DMA_ENABLE_IT(hdma, DMA_IT_FE);
}

static bool get_hal_direction(esy_dma_direction_t dir, uint32_t *direction) {
  if (!direction) return false;

  switch (dir) {
    case ESY_DMA_MEM_TO_MEM:
      *direction = DMA_MEMORY_TO_MEMORY;
      return true;
    case ESY_DMA_MEM_TO_PERIPH:
      *direction = DMA_MEMORY_TO_PERIPH;
      return true;
    case ESY_DMA_PERIPH_TO_MEM:
      *direction = DMA_PERIPH_TO_MEMORY;
      return true;
    default:
      return false;
  }
}

static bool stm32f4_dma_channel_to_hal(uint8_t channel,
                                       uint32_t *hal_channel) {
  if (!hal_channel) return false;

  switch (channel) {
    case 0u:
      *hal_channel = DMA_CHANNEL_0;
      return true;
    case 1u:
      *hal_channel = DMA_CHANNEL_1;
      return true;
    case 2u:
      *hal_channel = DMA_CHANNEL_2;
      return true;
    case 3u:
      *hal_channel = DMA_CHANNEL_3;
      return true;
    case 4u:
      *hal_channel = DMA_CHANNEL_4;
      return true;
    case 5u:
      *hal_channel = DMA_CHANNEL_5;
      return true;
    case 6u:
      *hal_channel = DMA_CHANNEL_6;
      return true;
    case 7u:
      *hal_channel = DMA_CHANNEL_7;
      return true;
    default:
      return false;
  }
}

static bool get_hal_data_size(esy_dma_transfer_size_t size,
                             uint32_t *periph_alignment, uint32_t *memory_alignment,
                             size_t *unit_bytes) {
  switch (size) {
    case ESY_DMA_TRANSFER_SIZE_8:
      *periph_alignment = DMA_PDATAALIGN_BYTE;
      *memory_alignment = DMA_MDATAALIGN_BYTE;
      *unit_bytes = sizeof(uint8_t);
      return true;
    case ESY_DMA_TRANSFER_SIZE_16:
      *periph_alignment = DMA_PDATAALIGN_HALFWORD;
      *memory_alignment = DMA_MDATAALIGN_HALFWORD;
      *unit_bytes = sizeof(uint16_t);
      return true;
    case ESY_DMA_TRANSFER_SIZE_32:
      *periph_alignment = DMA_PDATAALIGN_WORD;
      *memory_alignment = DMA_MDATAALIGN_WORD;
      *unit_bytes = sizeof(uint32_t);
      return true;
    default:
      return false;
  }
}

esy_err_t esy_hb_dma_get_resource_id(uint32_t dreq, uint8_t channel,
                                  uint8_t *out_id) {
  if (out_id == NULL || dreq >= MAX_DMA_DREQ || channel > 7u)
    return ESY_ERR_INVALID_ARG;
  *out_id = (uint8_t)dreq;
  return ESY_OK;
}

esy_err_t esy_hb_dma_claim(const void *owner, uint32_t dreq, uint8_t channel) {
  uint8_t resource;
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_dma_get_resource_id(dreq, channel, &resource);
  if (result != ESY_OK) return result;
  const void *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(&s_ctx.owners[resource],
      &expected, owner, memory_order_acq_rel, memory_order_acquire))
    return ESY_ERR_BUSY;
  if ((s_ctx.stream_table[resource].stream->CR & DMA_SxCR_EN) != 0u) {
    atomic_store_explicit(&s_ctx.owners[resource], NULL, memory_order_release);
    return ESY_ERR_BUSY;
  }
  return ESY_OK;
}

esy_err_t esy_hb_dma_release(const void *owner, uint32_t dreq, uint8_t channel) {
  uint8_t resource;
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_dma_get_resource_id(dreq, channel, &resource);
  if (result != ESY_OK) return result;
  const void *current = atomic_load_explicit(&s_ctx.owners[resource], memory_order_acquire);
  if (current != NULL && current != owner) return ESY_ERR_BUSY;
  if ((s_ctx.stream_table[resource].stream->CR & DMA_SxCR_EN) != 0u)
    return ESY_ERR_BUSY;
  if (current == NULL) return ESY_OK;
  /* Release is also used after bounded native ISR retirement. Its owner has
   * detached native callbacks; publication is the last resource-state write. */
  s_ctx.configured[resource] = false;
  s_ctx.callbacks[resource] = NULL;
  s_ctx.callback_args[resource] = NULL;
  s_ctx.half_callbacks[resource] = NULL;
  s_ctx.half_callback_args[resource] = NULL;
  s_ctx.error_callbacks[resource] = NULL;
  s_ctx.error_callback_args[resource] = NULL;
  atomic_store_explicit(&s_ctx.owners[resource], NULL, memory_order_release);
  return ESY_OK;
}

esy_err_t esy_hb_dma_prepare(const void *owner, const esy_dma_config_t *config) {
  if (!config || config->inner.dreq >= MAX_DMA_DREQ || config->length == 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  uint32_t periph_alignment, memory_alignment;
  size_t unit_bytes;
  if (!get_hal_data_size(config->transfer_size, &periph_alignment,
                         &memory_alignment, &unit_bytes) ||
      config->length % unit_bytes != 0u ||
      config->length / unit_bytes > UINT16_MAX ||
      (uintptr_t)config->src % unit_bytes != 0u ||
      (uintptr_t)config->dst % unit_bytes != 0u)
    return ESY_ERR_INVALID_ARG;

  if (config->inner.direction == ESY_DMA_MEM_TO_MEM &&
      (!config->src || !config->dst)) {
    return ESY_ERR_INVALID_ARG;
  }

  if (config->inner.direction == ESY_DMA_MEM_TO_PERIPH && !config->dst) {
    return ESY_ERR_INVALID_ARG;
  }

  if (config->inner.direction == ESY_DMA_PERIPH_TO_MEM && !config->src) {
    return ESY_ERR_INVALID_ARG;
  }

  uint32_t hal_direction = 0u;
  if (!get_hal_direction(config->inner.direction, &hal_direction)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (config->inner.direction == ESY_DMA_MEM_TO_MEM && config->circular)
    return ESY_ERR_NOT_SUPPORTED;
  uint32_t hal_channel = 0u;
  if (!stm32f4_dma_channel_to_hal(config->inner.channel, &hal_channel)) {
    return ESY_ERR_INVALID_ARG;
  }

  const dma_stream_info_t *map = &s_ctx.stream_table[config->inner.dreq];
  /* Only DMA2 has the memory-port path needed by memory-to-memory mode.
   * Reject before claiming a stream or changing its clocks, IRQs or registers. */
  if (config->inner.direction == ESY_DMA_MEM_TO_MEM && map->controller != DMA2)
    return ESY_ERR_NOT_SUPPORTED;
  if (config->inner.irq_index != (uint8_t)map->irq ||
      config->inner.irq_priority >= (1u << __NVIC_PRIO_BITS))
    return ESY_ERR_INVALID_ARG;
  DMA_HandleTypeDef *hdma = &s_ctx.handles[config->inner.dreq];
  esy_err_t claim = esy_hb_dma_claim(owner, config->inner.dreq,
                                    config->inner.channel);
  if (claim != ESY_OK) return claim;
  s_ctx.configured[config->inner.dreq] = false;
  HAL_NVIC_DisableIRQ(map->irq);
  memset(hdma, 0, sizeof(*hdma));
  hdma->Instance = map->stream;

  if (map->controller == DMA1) {
    __HAL_RCC_DMA1_CLK_ENABLE();
  } else if (map->controller == DMA2) {
    __HAL_RCC_DMA2_CLK_ENABLE();
  }

  bool use_dummy = false;
  if ((config->inner.direction == ESY_DMA_MEM_TO_PERIPH &&
       config->src == NULL) ||
      (config->inner.direction == ESY_DMA_PERIPH_TO_MEM &&
       config->dst == NULL)) {
    use_dummy = true;
  }

  hdma->Init.Channel = hal_channel;
  hdma->Init.Direction = hal_direction;
  hdma->Init.PeriphInc = config->inner.direction == ESY_DMA_MEM_TO_MEM
                            ? DMA_PINC_ENABLE : DMA_PINC_DISABLE;
  /* A missing memory endpoint uses one aligned word, never an incrementing
   * array. This does not change a finite transfer into a circular transfer. */
  hdma->Init.MemInc = use_dummy ? DMA_MINC_DISABLE : DMA_MINC_ENABLE;
  hdma->Init.PeriphDataAlignment = periph_alignment;
  hdma->Init.MemDataAlignment = memory_alignment;
  hdma->Init.Mode = config->circular ? DMA_CIRCULAR : DMA_NORMAL;
  hdma->Init.Priority = DMA_PRIORITY_LOW;
  hdma->Init.FIFOMode = config->inner.direction == ESY_DMA_MEM_TO_MEM
                           ? DMA_FIFOMODE_ENABLE : DMA_FIFOMODE_DISABLE;
  hdma->Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
  hdma->Init.MemBurst = DMA_MBURST_SINGLE;
  hdma->Init.PeriphBurst = DMA_PBURST_SINGLE;

  if (HAL_DMA_Init(hdma) != HAL_OK) {
    /* Claim observed EN clear and no operation above can enable this stream.
     * Native init only clears EN/configures registers, so retirement cannot
     * retain hardware here under the exclusive-owner contract. */
    (void)esy_hb_dma_release(owner, config->inner.dreq, config->inner.channel);
    return ESY_ERR_FAIL;
  }

  __HAL_DMA_DISABLE(hdma);
  dma_disable_all_interrupts(hdma);
  dma_clear_all_flags(hdma);
  HAL_NVIC_ClearPendingIRQ(map->irq);
  if (config->callback) {
    __HAL_DMA_ENABLE_IT(hdma, DMA_IT_TC);
  }
  if (config->half_callback) {
    __HAL_DMA_ENABLE_IT(hdma, DMA_IT_HT);
  }
  dma_enable_error_interrupts(hdma);
  if (config->inner.direction == ESY_DMA_MEM_TO_PERIPH) {
    hdma->Instance->PAR = (uint32_t)config->dst;
    hdma->Instance->M0AR =
        (uint32_t)(config->src ? config->src
                               : &s_ctx.dummy[config->inner.dreq]);
  } else {
    hdma->Instance->PAR = (uint32_t)config->src;
    hdma->Instance->M0AR =
        (uint32_t)(config->dst ? config->dst
                               : &s_ctx.dummy[config->inner.dreq]);
  }
  hdma->Instance->NDTR = config->length / unit_bytes;

  s_ctx.callbacks[config->inner.dreq] = config->callback;
  s_ctx.callback_args[config->inner.dreq] = config->callback_arg;
  s_ctx.half_callbacks[config->inner.dreq] = config->half_callback;
  s_ctx.half_callback_args[config->inner.dreq] = config->half_callback_arg;
  s_ctx.error_callbacks[config->inner.dreq] = config->error_callback;
  s_ctx.error_callback_args[config->inner.dreq] =
      config->error_callback_arg;
  hdma->XferCpltCallback = dma_complete_callback;
  hdma->XferHalfCpltCallback = dma_half_complete_callback;
  hdma->XferErrorCallback = dma_error_callback;

  s_ctx.unit_bytes[config->inner.dreq] = unit_bytes;
  s_ctx.configured[config->inner.dreq] = true;
  HAL_NVIC_SetPriority(config->inner.irq_index, config->inner.irq_priority, 0);
  HAL_NVIC_EnableIRQ(config->inner.irq_index);

  return ESY_OK;
}

DMA_HandleTypeDef *esy_hb_stm32f4_dma_get_handle(const void *owner, uint32_t dreq, uint8_t channel) {
  uint32_t hal_channel;
  if (owner == NULL || dreq >= MAX_DMA_DREQ ||
      !stm32f4_dma_channel_to_hal(channel, &hal_channel)) return NULL;
  if (atomic_load_explicit(&s_ctx.owners[dreq], memory_order_acquire) != owner ||
      !s_ctx.configured[dreq])
    return NULL;
  DMA_HandleTypeDef *handle = &s_ctx.handles[dreq];
  return handle->Instance != NULL && handle->Init.Channel == hal_channel
             ? handle : NULL;
}

esy_err_t esy_hb_dma_start(const void *owner, uint32_t channel_mask) {
  if (owner == NULL || channel_mask == 0u || (channel_mask & ~DMA_RESOURCE_MASK) != 0u)
    return ESY_ERR_INVALID_ARG;
  /* Validate the whole physical-resource mask before enabling any stream.
   * The serialized task owner can then reject a batch without partial start. */
  for (uint8_t i = 0u; i < MAX_DMA_DREQ; ++i) {
    if ((channel_mask & (1u << i)) == 0u) continue;
    if (atomic_load_explicit(&s_ctx.owners[i], memory_order_acquire) != owner)
      return ESY_ERR_BUSY;
    DMA_HandleTypeDef *handle = &s_ctx.handles[i];
    if (!s_ctx.configured[i] || handle->Instance == NULL ||
        handle->Instance->NDTR == 0u)
      return ESY_ERR_STATE;
    if ((handle->Instance->CR & DMA_SxCR_EN) != 0u ||
        handle->State == HAL_DMA_STATE_BUSY) return ESY_ERR_BUSY;
  }
  for (uint8_t i = 0; i < MAX_DMA_DREQ; ++i) {
    if (!(channel_mask & (1u << i))) continue;

    DMA_HandleTypeDef *hdma = &s_ctx.handles[i];

    dma_clear_all_flags(hdma);
    hdma->State = HAL_DMA_STATE_BUSY;
    hdma->ErrorCode = HAL_DMA_ERROR_NONE;
    __HAL_DMA_ENABLE(hdma);
  }
  return ESY_OK;
}

esy_err_t esy_hb_dma_start_single(const void *owner, const esy_dma_config_t *transfer) {
  esy_err_t ret = esy_hb_dma_prepare(owner, transfer);
  if (ret != ESY_OK) return ret;
  return esy_hb_dma_start(owner, 1u << transfer->inner.dreq);
}

esy_err_t esy_hb_dma_abort(const void *owner, uint32_t dreq, uint8_t channel) {
  if (owner == NULL || dreq >= MAX_DMA_DREQ || channel > 7u) return ESY_ERR_INVALID_ARG;
  const void *current = atomic_load_explicit(&s_ctx.owners[dreq], memory_order_acquire);
  if (current != owner) return current == NULL &&
      (s_ctx.stream_table[dreq].stream->CR & DMA_SxCR_EN) == 0u ? ESY_OK : ESY_ERR_BUSY;
  DMA_HandleTypeDef *handle = esy_hb_stm32f4_dma_get_handle(owner, dreq, channel);
  if (handle == NULL) return ESY_ERR_STATE;
  /* Native error handlers can leave READY state while EN is still retiring.
   * Abort owns the task-side retry and must not treat that stream as reusable. */
  if ((handle->Instance->CR & DMA_SxCR_EN) != 0u)
    handle->State = HAL_DMA_STATE_BUSY;
  HAL_StatusTypeDef result = HAL_DMA_Abort(handle);
  if (result != HAL_OK && (handle->Instance->CR & DMA_SxCR_EN)) return ESY_ERR_FAIL;
  dma_disable_all_interrupts(handle);
  dma_clear_all_flags(handle);
  s_ctx.configured[dreq] = false;
  s_ctx.callbacks[dreq] = NULL;
  s_ctx.callback_args[dreq] = NULL;
  s_ctx.half_callbacks[dreq] = NULL;
  s_ctx.half_callback_args[dreq] = NULL;
  s_ctx.error_callbacks[dreq] = NULL;
  s_ctx.error_callback_args[dreq] = NULL;
  handle->XferCpltCallback = NULL;
  handle->XferHalfCpltCallback = NULL;
  handle->XferErrorCallback = NULL;
  handle->Parent = NULL;
  HAL_NVIC_ClearPendingIRQ(s_ctx.stream_table[dreq].irq);
  return esy_hb_dma_release(owner, dreq, channel);
}

bool esy_hb_dma_is_busy(uint32_t dreq, uint8_t channel) {
  (void)channel;
  if (dreq >= MAX_DMA_DREQ) return false;
  return s_ctx.handles[dreq].Instance &&
      ((s_ctx.handles[dreq].Instance->CR & DMA_SxCR_EN) != 0u);
}

esy_err_t esy_hb_dma_bind_irq_to_core(uint32_t irq_index,
                                          uint32_t core_id) {
  (void)irq_index;
  (void)core_id;
  return ESY_ERR_NOT_SUPPORTED;
}

static esy_err_t dma_control_handle(const void *owner, uint32_t dreq, uint8_t channel,
                                    DMA_HandleTypeDef **out_handle) {
  if (owner == NULL || dreq >= MAX_DMA_DREQ || channel > 7u) return ESY_ERR_INVALID_ARG;
  if (atomic_load_explicit(&s_ctx.owners[dreq], memory_order_acquire) != owner)
    return ESY_ERR_BUSY;
  DMA_HandleTypeDef *handle = esy_hb_stm32f4_dma_get_handle(owner, dreq, channel);
  if (handle == NULL || !s_ctx.configured[dreq]) return ESY_ERR_STATE;
  if ((handle->Instance->CR & DMA_SxCR_EN) != 0u ||
      handle->State == HAL_DMA_STATE_BUSY) return ESY_ERR_BUSY;
  if (handle->ErrorCode != HAL_DMA_ERROR_NONE) return ESY_ERR_STATE;
  *out_handle = handle;
  return ESY_OK;
}

esy_err_t esy_hb_dma_set_read_addr(const void *owner, uint32_t dreq, uint8_t channel,
                                  const void *addr, bool trigger_now) {
  DMA_HandleTypeDef *handle;
  if (addr == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = dma_control_handle(owner, dreq, channel, &handle);
  if (result != ESY_OK) return result;
  if ((uintptr_t)addr % s_ctx.unit_bytes[dreq] != 0u)
    return ESY_ERR_INVALID_ARG;
  if (trigger_now && handle->Instance->NDTR == 0u) return ESY_ERR_STATE;
  if (handle->Init.Direction == DMA_MEMORY_TO_PERIPH)
    handle->Instance->M0AR = (uint32_t)addr;
  else
    handle->Instance->PAR = (uint32_t)addr;
  return trigger_now ? esy_hb_dma_start(owner, 1u << dreq) : ESY_OK;
}

esy_err_t esy_hb_dma_set_write_addr(const void *owner, uint32_t dreq, uint8_t channel, void *addr,
                                   bool trigger_now) {
  DMA_HandleTypeDef *handle;
  if (addr == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = dma_control_handle(owner, dreq, channel, &handle);
  if (result != ESY_OK) return result;
  if ((uintptr_t)addr % s_ctx.unit_bytes[dreq] != 0u)
    return ESY_ERR_INVALID_ARG;
  if (trigger_now && handle->Instance->NDTR == 0u) return ESY_ERR_STATE;
  if (handle->Init.Direction == DMA_MEMORY_TO_PERIPH)
    handle->Instance->PAR = (uint32_t)addr;
  else
    handle->Instance->M0AR = (uint32_t)addr;
  return trigger_now ? esy_hb_dma_start(owner, 1u << dreq) : ESY_OK;
}

esy_err_t esy_hb_dma_set_trans_count(const void *owner, uint32_t dreq, uint8_t channel,
                                    size_t count, bool trigger_now) {
  DMA_HandleTypeDef *handle;
  if (count == 0u || count > UINT16_MAX) return ESY_ERR_INVALID_ARG;
  esy_err_t result = dma_control_handle(owner, dreq, channel, &handle);
  if (result != ESY_OK) return result;
  handle->Instance->NDTR = count;
  return trigger_now ? esy_hb_dma_start(owner, 1u << dreq) : ESY_OK;
}

esy_err_t esy_hb_dma_get_trans_count_remaining(uint32_t dreq,
                                                   uint8_t channel,
                                                   size_t *out_remaining) {
  (void)channel;
  if (dreq >= MAX_DMA_DREQ || !out_remaining) return ESY_ERR_INVALID_ARG;
  if (!s_ctx.handles[dreq].Instance) return ESY_ERR_STATE;

  *out_remaining = s_ctx.handles[dreq].Instance->NDTR;
  return ESY_OK;
}

esy_err_t esy_hb_dma_chain_to(const void *owner, uint32_t dreq, uint8_t channel,
                             uint8_t chain_channel) {
  if (owner == NULL || dreq >= MAX_DMA_DREQ || channel > 7u || chain_channel >= MAX_DMA_DREQ)
    return ESY_ERR_INVALID_ARG;
  if (atomic_load_explicit(&s_ctx.owners[dreq], memory_order_acquire) != owner)
    return ESY_ERR_BUSY;
  return ESY_ERR_NOT_SUPPORTED;
}

/* Only single-buffer streams are configured by this backend, including ADC's
 * native HAL_DMA_Start_IT path. Preserve native callback/State/Lock semantics
 * without the vendor IRQ's TE shutdown wait. Task abort owns final retirement. */
void esy_hb_dma_handle_irq(uint32_t dreq, uint8_t channel) {
  /* IRQ vectors identify the physical stream; their channel placeholder is
   * independent of the request selector chosen when that stream was prepared. */
  (void)channel;
  if (dreq >= MAX_DMA_DREQ) return;
  DMA_HandleTypeDef *handle = &s_ctx.handles[dreq];
  if (handle->Instance == NULL) return;
  const uint32_t status =
      *(volatile uint32_t *)(uintptr_t)handle->StreamBaseAddress >>
      handle->StreamIndex;
  const uint32_t control = handle->Instance->CR;
  const uint32_t fifo = handle->Instance->FCR;
  uint32_t error = HAL_DMA_ERROR_NONE;
  if ((status & DMA_FLAG_TEIF0_4) && (control & DMA_IT_TE))
    error |= HAL_DMA_ERROR_TE;
  if ((status & DMA_FLAG_DMEIF0_4) && (control & DMA_IT_DME))
    error |= HAL_DMA_ERROR_DME;
  if ((status & DMA_FLAG_FEIF0_4) && (fifo & DMA_IT_FE))
    error |= HAL_DMA_ERROR_FE;
  if (error != HAL_DMA_ERROR_NONE) {
    handle->ErrorCode |= error;
    dma_disable_all_interrupts(handle);
    __HAL_DMA_DISABLE(handle);
    dma_clear_all_flags(handle);
    /* EN may retire asynchronously. Do not claim readiness until it clears. */
    if ((handle->Instance->CR & DMA_SxCR_EN) == 0u) {
      handle->State = HAL_DMA_STATE_READY;
      __HAL_UNLOCK(handle);
    }
    if (handle->XferErrorCallback != NULL) handle->XferErrorCallback(handle);
    return;
  }
  if (handle->ErrorCode != HAL_DMA_ERROR_NONE) {
    dma_disable_all_interrupts(handle);
    dma_clear_all_flags(handle);
    return;
  }
  if ((status & DMA_FLAG_HTIF0_4) && (control & DMA_IT_HT)) {
    __HAL_DMA_CLEAR_FLAG(handle, __HAL_DMA_GET_HT_FLAG_INDEX(handle));
    if ((control & DMA_SxCR_CIRC) == 0u) __HAL_DMA_DISABLE_IT(handle, DMA_IT_HT);
    if (handle->XferHalfCpltCallback != NULL) handle->XferHalfCpltCallback(handle);
  }
  if ((status & DMA_FLAG_TCIF0_4) && (control & DMA_IT_TC)) {
    __HAL_DMA_CLEAR_FLAG(handle, __HAL_DMA_GET_TC_FLAG_INDEX(handle));
    if ((control & DMA_SxCR_CIRC) == 0u) {
      __HAL_DMA_DISABLE_IT(handle, DMA_IT_TC | DMA_IT_HT);
      handle->State = HAL_DMA_STATE_READY;
      __HAL_UNLOCK(handle);
    }
    if (handle->XferCpltCallback != NULL) handle->XferCpltCallback(handle);
  }
}
