/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_uart.h"
#include "esy_common/ds/esy_ring_buffer.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"

#include <stdint.h>
#include <string.h>

#include "stm32f4xx_hal.h"

#include "esy_board/esy_board.h"
#include "esy_hal_backend/esy_hb_dma.h"

#define STM32F4_UART_MAX 4u

enum { UART_DMA_DONE = 1u, UART_DMA_ERROR = 2u, UART_DMA_HALF = 4u };

typedef struct {
  esy_uart_t *owner;
  esy_dma_config_t config;
  _Atomic uint32_t events;
  bool active;
  bool is_tx;
} stm32_uart_dma_t;

typedef struct {
    USART_TypeDef       *instance;
    UART_HandleTypeDef   huart;
    uint8_t              phys_index;
    bool                 enabled;
    bool                 suspended;
    bool                 native_active;

    uint32_t             baudrate;
    uint8_t              data_bits;
    uint8_t              stop_bits;
    uint8_t              irq_priority;
    esy_uart_parity_t    parity;

    esy_uart_t          *bound_handle;
    stm32_uart_dma_t     tx_dma;
    stm32_uart_dma_t     rx_dma;
    esy_ring_buffer_t    rx_stream;
    esy_dma_config_t     rx_stream_dma;
    esy_uart_rx_stream_callback_t rx_stream_callback;
    void                *rx_stream_user_data;
    size_t               rx_stream_peek_length;
    size_t               rx_stream_dma_position;
    bool                 rx_stream_active;
    bool                 rx_stream_overflowed;
} stm32f4_uart_ctx_t;

_Static_assert(ESY_UART_PRIV_BYTES >= sizeof(stm32f4_uart_ctx_t),
               "ESY_UART_PRIV_BYTES too small");

typedef struct {
  esy_uart_t *bound_uarts[STM32F4_UART_MAX];
  _Atomic(esy_uart_t *) owners[STM32F4_UART_MAX];
} esy_hb_uart_ctx_t;

static esy_hb_uart_ctx_t s_ctx;

static stm32f4_uart_ctx_t *uart_context(const esy_uart_t *uart);

/* One task owns lifecycle; UART/DMA IRQs touch CR3 and publish events.
 * PRIMASK protects only the shared CR3 read-modify-write, never retirement. */
static void uart_dma_requests(USART_TypeDef *instance, bool is_tx, bool enable) {
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  uint32_t mask = is_tx ? USART_CR3_DMAT : USART_CR3_DMAR;
  if (enable) SET_BIT(instance->CR3, mask);
  else CLEAR_BIT(instance->CR3, mask);
  __set_PRIMASK(key);
}

static void uart_dma_publish(stm32_uart_dma_t *direction, uint32_t event) {
  stm32f4_uart_ctx_t *c = direction->owner->ctx[0];
  if (event != UART_DMA_HALF)
    uart_dma_requests(c->instance, direction->is_tx, false);
  atomic_fetch_or_explicit(&direction->events, event, memory_order_release);
}
static void uart_dma_complete_callback(void *arg) {
  uart_dma_publish(arg, UART_DMA_DONE);
}
static void uart_dma_error_callback(void *arg) {
  uart_dma_publish(arg, UART_DMA_ERROR);
}
static void uart_dma_half_callback(void *arg) {
  uart_dma_publish(arg, UART_DMA_HALF);
}

static IRQn_Type uart_irqn(uint8_t phys_index) {
  switch (phys_index) {
    case 1u:
      return USART1_IRQn;
    case 2u:
      return USART2_IRQn;
#if defined(USART3)
    case 3u:
      return USART3_IRQn;
#endif
    default:
      return NonMaskableInt_IRQn;
  }
}

static void uart_stream_latch_error(esy_uart_t *uart,
                                    esy_uart_rx_stream_event_t event) {
  stm32f4_uart_ctx_t *c;

  if (!uart || uart_context(uart) == NULL) return;
  c = uart_context(uart);
  /* UART IDLE/error and DMA IRQs are independent producers. Exclude only
   * publication metadata; callbacks run after restoring the entry IRQ mask. */
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  if (!c->rx_stream_active || c->rx_stream_overflowed) {
    __set_PRIMASK(key);
    return;
  }
  c->rx_stream_overflowed = true;
  uart_dma_requests(c->instance, false, false);
  esy_uart_rx_stream_callback_t callback = c->rx_stream_callback;
  void *user_data = c->rx_stream_user_data;
  __set_PRIMASK(key);
  /* ISR only stops requests. The owner retires DMA through stream_stop;
   * failed stop retains the active stream and its borrowed storage. */
  if (callback) callback(uart, event, user_data);
}

static void uart_stream_publish_dma_position(esy_uart_t *uart) {
  stm32f4_uart_ctx_t *c;
  size_t remaining;
  size_t position;
  size_t produced;

  if (!uart || uart_context(uart) == NULL) return;
  c = uart_context(uart);
  /* The SPSC ring has one logical ISR producer. PRIMASK serializes UART IDLE
   * with DMA HT/TC even when their board-selected priorities differ. The
   * bounded position query and cursor commit cannot block; notify outside. */
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  if (!c->rx_stream_active || c->rx_stream_overflowed) {
    __set_PRIMASK(key);
    return;
  }

  if (esy_hb_dma_get_trans_count_remaining(c->rx_stream_dma.inner.dreq,
                                               c->rx_stream_dma.inner.channel,
                                               &remaining) != ESY_OK ||
      remaining > c->rx_stream_dma.length) {
    __set_PRIMASK(key);
    uart_stream_latch_error(uart, ESY_UART_RX_STREAM_EVENT_ERROR);
    return;
  }

  position = c->rx_stream_dma.length - remaining;
  if (position == c->rx_stream_dma.length) position = 0u;
  produced =
      (position >= c->rx_stream_dma_position)
          ? (position - c->rx_stream_dma_position)
          : (c->rx_stream_dma.length - c->rx_stream_dma_position + position);
  if (produced == 0u) {
    __set_PRIMASK(key);
    return;
  }

  if (produced > esy_ring_buffer_free(&c->rx_stream)) {
    __set_PRIMASK(key);
    uart_stream_latch_error(uart, ESY_UART_RX_STREAM_EVENT_OVERFLOW);
    return;
  }

  esy_ring_buffer_commit_write(&c->rx_stream, (uint32_t)produced);
  c->rx_stream_dma_position = position;
  esy_uart_rx_stream_callback_t callback = c->rx_stream_callback;
  void *user_data = c->rx_stream_user_data;
  __set_PRIMASK(key);
  if (callback) callback(uart, ESY_UART_RX_STREAM_EVENT_DATA, user_data);
}

static void uart_stream_dma_callback(void *arg) {
  uart_stream_publish_dma_position((esy_uart_t *)arg);
}

static void uart_stream_dma_error_callback(void *arg) {
  uart_stream_latch_error((esy_uart_t *)arg,
                          ESY_UART_RX_STREAM_EVENT_ERROR);
}

static void uart_clear_rx_flags(UART_HandleTypeDef *huart) {
  volatile uint32_t clear;

  clear = huart->Instance->SR;
  clear = huart->Instance->DR;
  (void)clear;
}

static inline uint32_t to_hal_word_len(uint8_t data_bits,
                                        esy_uart_parity_t parity) {
    /* Native word length includes parity; public data_bits counts payload. */
    return (data_bits == 8u && parity != ESY_UART_PARITY_NONE)
               ? UART_WORDLENGTH_9B : UART_WORDLENGTH_8B;
}
static inline uint32_t to_hal_stop_bits(uint8_t stop_bits) {
    return (stop_bits == 2) ? UART_STOPBITS_2 : UART_STOPBITS_1;
}
static inline uint32_t to_hal_parity(esy_uart_parity_t p) {
    return (p == ESY_UART_PARITY_EVEN) ? UART_PARITY_EVEN
         : (p == ESY_UART_PARITY_ODD)  ? UART_PARITY_ODD
                                       : UART_PARITY_NONE;
}

/* Each controller has one owner across init, suspend and failed cleanup.
 * Atomic slot claims cover independent owner tasks/cores; per-handle operations
 * remain serialized by its caller. IRQ dispatch is published only after init. */
static stm32f4_uart_ctx_t *uart_context(const esy_uart_t *uart) {
  if (uart == NULL || uart->ctx[0] != (const void *)uart->backend.bytes)
    return NULL;
  /* Establish the concrete backend type through the claim table before reading
   * opaque storage: an XIO handle may also place ctx[0] at backend.bytes. */
  for (uint8_t index = 0u; index < STM32F4_UART_MAX; ++index) {
    if (atomic_load_explicit(&s_ctx.owners[index], memory_order_acquire) != uart)
      continue;
    stm32f4_uart_ctx_t *c = (stm32f4_uart_ctx_t *)uart->ctx[0];
    return c->phys_index == index && c->bound_handle == uart ? c : NULL;
  }
  return NULL;
}

esy_err_t esy_hb_uart_claim(esy_uart_t *uart, uint8_t phys_index) {
  if (uart == NULL || !((phys_index == 1u || phys_index == 2u
#if defined(USART3)
      || phys_index == 3u
#endif
  ))) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] != NULL || uart->ctx[1] != NULL) {
    stm32f4_uart_ctx_t *c = uart_context(uart);
    return c != NULL && c->phys_index == phys_index && !c->enabled && !c->suspended &&
           !c->native_active
               ? ESY_OK : ESY_ERR_STATE;
  }
  esy_uart_t *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          &s_ctx.owners[phys_index], &expected, uart, memory_order_acq_rel,
          memory_order_acquire)) return ESY_ERR_BUSY;
  stm32f4_uart_ctx_t *c = (stm32f4_uart_ctx_t *)uart->backend.bytes;
  memset(c, 0, sizeof(*c));
  c->phys_index = phys_index;
  c->bound_handle = uart;
  atomic_init(&c->tx_dma.events, 0u);
  atomic_init(&c->rx_dma.events, 0u);
  uart->ctx[0] = c;
  return ESY_OK;
}

esy_err_t esy_hb_uart_release(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] == NULL) return ESY_OK;
  stm32f4_uart_ctx_t *c = uart_context(uart);
  if (c == NULL) return ESY_ERR_STATE;
  if (c->enabled || c->native_active || c->tx_dma.active || c->rx_dma.active || c->rx_stream_active) return ESY_ERR_BUSY;
  uint8_t index = c->phys_index;
  /* Task+ISR binding handoff; PRIMASK covers pointer publication only. */
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  s_ctx.bound_uarts[index] = NULL;
  c->bound_handle = NULL;
  uart->ctx[0] = NULL;
  __set_PRIMASK(key);
  atomic_store_explicit(&s_ctx.owners[index], NULL, memory_order_release);
  return ESY_OK;
}

esy_err_t esy_hb_uart_validate_config(const esy_uart_config_t *cfg) {
  if (cfg == NULL || cfg->baudrate == 0u ||
      cfg->data_bits < 5u || cfg->data_bits > 9u ||
      (cfg->stop_bits != 1u && cfg->stop_bits != 2u) ||
      (cfg->parity != ESY_UART_PARITY_NONE &&
       cfg->parity != ESY_UART_PARITY_EVEN &&
       cfg->parity != ESY_UART_PARITY_ODD)) return ESY_ERR_INVALID_ARG;
  /* Byte APIs cannot represent 9-bit payloads. Native 8/9-bit words represent
   * only 8N, 7E/7O and 8E/8O without reading/writing halfword buffer elements. */
  if (cfg->data_bits != 7u && cfg->data_bits != 8u)
    return ESY_ERR_NOT_SUPPORTED;
  if (cfg->data_bits == 7u && cfg->parity == ESY_UART_PARITY_NONE)
    return ESY_ERR_NOT_SUPPORTED;
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_init(esy_uart_t *uart,
                                      const esy_uart_config_t *cfg)
{
    if (!uart || !cfg) return ESY_ERR_INVALID_ARG;

    esy_err_t ret = esy_hb_uart_validate_config(cfg);
    if (ret != ESY_OK) return ret;
    ret = esy_hb_uart_claim(uart, cfg->phys_index);
    if (ret != ESY_OK) return ret;
    stm32f4_uart_ctx_t *c = uart_context(uart);

    if(cfg->phys_index == 1) {
        c->instance = USART1;
    }
    else if(cfg->phys_index == 2) {
        c->instance = USART2;
    }
#if defined(USART3)
    else if(cfg->phys_index == 3) {
        c->instance = USART3;
    }
#endif
    else {
        return ESY_ERR_INVALID_ARG;
    }

    c->phys_index = cfg->phys_index;
    c->baudrate  = cfg->baudrate;
    c->data_bits = cfg->data_bits;
    c->stop_bits = cfg->stop_bits;
    c->irq_priority = cfg->inner.irq_priority;
    c->parity    = cfg->parity;

    c->huart.Instance        = c->instance;
    c->huart.Init.BaudRate   = c->baudrate;
    c->huart.Init.WordLength = to_hal_word_len(c->data_bits, c->parity);
    c->huart.Init.StopBits   = to_hal_stop_bits(c->stop_bits);
    c->huart.Init.Parity     = to_hal_parity(c->parity);
    c->huart.Init.Mode       = UART_MODE_TX_RX;
    c->huart.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    c->huart.Init.OverSampling = UART_OVERSAMPLING_16;

    c->native_active = true;
    if (HAL_UART_Init(&c->huart) != HAL_OK) return ESY_ERR_FAIL;

    c->enabled   = true;
    c->suspended = false;

    c->bound_handle = uart;
    uart->ctx[0] = c;
    s_ctx.bound_uarts[cfg->phys_index] = uart;
    return ESY_OK;
}

esy_err_t esy_hb_uart_deinit(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] == NULL) return ESY_OK;
  stm32f4_uart_ctx_t *c = uart_context(uart);
  if (c == NULL) return ESY_ERR_STATE;
  if (c->tx_dma.active || c->rx_dma.active || c->rx_stream_active ||
      esy_hb_uart_is_transmitting(uart)) return ESY_ERR_BUSY;
  c->enabled = false;
  c->suspended = true;
  if (c->native_active) {
    if (HAL_UART_DeInit(&c->huart) != HAL_OK) return ESY_ERR_IO;
    c->native_active = false;
  }
  /* Native UART/DMA work is quiescent before detaching ISR dispatch. */
  uint32_t key = __get_PRIMASK();
  __disable_irq();
  s_ctx.bound_uarts[c->phys_index] = NULL;
  __set_PRIMASK(key);
  return ESY_OK;
}

extern int32_t esy_hb_uart_send_blocking(esy_uart_t *uart,
                                             const uint8_t *data, size_t len)
{
    if (!uart || uart_context(uart) == NULL || !data || !len || len > UINT16_MAX) return ESY_ERR_INVALID_ARG;
    stm32f4_uart_ctx_t *c = uart_context(uart);
    if (!c->enabled || c->suspended) return ESY_ERR_STATE;

    if (HAL_UART_Transmit(&c->huart, (uint8_t*)data, (uint16_t)len, HAL_MAX_DELAY) != HAL_OK)
        return ESY_ERR_FAIL;
    return (int32_t)len;
}

extern int32_t esy_hb_uart_receive_blocking(esy_uart_t *uart,
                                                uint8_t *data, size_t len)
{
    if (!uart || uart_context(uart) == NULL || !data || !len || len > UINT16_MAX) return ESY_ERR_INVALID_ARG;
    stm32f4_uart_ctx_t *c = uart_context(uart);
    if (!c->enabled || c->suspended) return ESY_ERR_STATE;
    if (c->rx_stream_active || c->rx_dma.active) return ESY_ERR_BUSY;

    if (HAL_UART_Receive(&c->huart, data, (uint16_t)len, HAL_MAX_DELAY) != HAL_OK)
        return ESY_ERR_FAIL;
    return (int32_t)len;
}

static esy_err_t uart_start_dma(esy_uart_t *uart, esy_dma_config_t *config,
                                 bool is_tx) {
  if (!uart || uart_context(uart) == NULL || !config || config->length == 0u ||
      config->length > UINT16_MAX ||
      (is_tx ? config->src == NULL : config->dst == NULL))
    return ESY_ERR_INVALID_ARG;
  /* Finite-only DMA: continuous reception uses the dedicated RX stream API. */
  if (config->circular) return ESY_ERR_NOT_SUPPORTED;
  stm32f4_uart_ctx_t *c = uart_context(uart);
  if (!c->enabled || c->suspended) return ESY_ERR_STATE;
  stm32_uart_dma_t *direction = is_tx ? &c->tx_dma : &c->rx_dma;
  if (direction->active || (!is_tx && c->rx_stream_active)) return ESY_ERR_BUSY;
  esy_dma_config_t transfer = *config;
  transfer.inner.direction = is_tx ? ESY_DMA_MEM_TO_PERIPH : ESY_DMA_PERIPH_TO_MEM;
  if (is_tx) transfer.dst = (void *)&c->instance->DR;
  else transfer.src = (void *)&c->instance->DR;
  transfer.callback = uart_dma_complete_callback;
  transfer.callback_arg = direction;
  transfer.error_callback = uart_dma_error_callback;
  transfer.error_callback_arg = direction;
  transfer.half_callback = uart_dma_half_callback;
  transfer.half_callback_arg = direction;
  esy_err_t result = esy_hb_dma_prepare(direction, &transfer);
  if (result != ESY_OK) return result;
  direction->owner = uart;
  direction->config = *config;
  direction->is_tx = is_tx;
  atomic_store_explicit(&direction->events, 0u, memory_order_release);
  direction->active = true;
  result = esy_hb_dma_start(direction, 1u << config->inner.dreq);
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_hb_uart_abort_dma(uart, is_tx);
    return cleanup != ESY_OK ? cleanup : result;
  }
  uart_dma_requests(c->instance, is_tx, true);
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_send_dma(esy_uart_t *uart, esy_dma_config_t *config) {
  return uart_start_dma(uart, config, true);
}
extern esy_err_t esy_hb_uart_receive_dma(esy_uart_t *uart, esy_dma_config_t *config) {
  return uart_start_dma(uart, config, false);
}

esy_err_t esy_hb_uart_abort_dma(esy_uart_t *uart, bool is_tx) {
  if (uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_uart_ctx_t *c = uart_context(uart);
  stm32_uart_dma_t *direction = is_tx ? &c->tx_dma : &c->rx_dma;
  if (!direction->active) return ESY_OK;
  uart_dma_requests(c->instance, is_tx, false);
  esy_err_t result = esy_hb_dma_abort(direction, direction->config.inner.dreq,
                                    direction->config.inner.channel);
  if (result != ESY_OK) return result;
  direction->active = false;
  memset(&direction->config, 0, sizeof(direction->config));
  return ESY_OK;
}

bool esy_hb_uart_is_dma_busy(const esy_uart_t *uart, bool is_tx) {
  if (uart_context(uart) == NULL) return false;
  const stm32f4_uart_ctx_t *c = uart_context(uart);
  return is_tx ? c->tx_dma.active : c->rx_dma.active;
}

esy_err_t esy_hb_uart_poll(esy_uart_t *uart) {
  if (uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_uart_ctx_t *c = uart_context(uart);
  esy_err_t first_error = ESY_OK;
  for (unsigned i = 0u; i < 2u; ++i) {
    stm32_uart_dma_t *direction = i == 0u ? &c->tx_dma : &c->rx_dma;
    if (!direction->active) continue;
    uint32_t events = atomic_load_explicit(&direction->events, memory_order_acquire);
    if ((events & (UART_DMA_DONE | UART_DMA_ERROR)) != 0u) {
      if ((events & UART_DMA_ERROR) == 0u && direction->is_tx &&
          __HAL_UART_GET_FLAG(&c->huart, UART_FLAG_TC) == RESET) continue;
      esy_dma_config_t completed = direction->config;
      esy_err_t result = esy_hb_uart_abort_dma(uart, direction->is_tx);
      if (result != ESY_OK) {
        if (first_error == ESY_OK) first_error = result;
        continue;
      }
      events = atomic_load_explicit(&direction->events, memory_order_acquire);
      esy_dma_callback_t callback = (events & UART_DMA_ERROR) != 0u
          ? completed.error_callback : completed.callback;
      void *arg = (events & UART_DMA_ERROR) != 0u
          ? completed.error_callback_arg : completed.callback_arg;
      if (callback != NULL) callback(arg);
    } else if ((events & UART_DMA_HALF) != 0u) {
      atomic_fetch_and_explicit(&direction->events, ~UART_DMA_HALF, memory_order_acq_rel);
      if (direction->config.half_callback != NULL)
        direction->config.half_callback(direction->config.half_callback_arg);
    }
  }
  return first_error;
}

extern esy_err_t esy_hb_uart_rx_stream_start(
    esy_uart_t *uart, uint8_t *storage, size_t storage_size,
    esy_uart_rx_stream_callback_t callback, void *user_data) {
  stm32f4_uart_ctx_t *c;
  esy_dma_inner_config_t *inner;
  IRQn_Type irqn;
  esy_err_t ret;

  if (!uart || uart_context(uart) == NULL || !storage || storage_size < 2u ||
      storage_size > UINT16_MAX || storage_size > UINT32_MAX || !callback) {
    return ESY_ERR_INVALID_ARG;
  }

  c = uart_context(uart);
  if (!c->enabled || c->suspended) return ESY_ERR_STATE;
  if (c->rx_stream_active) return ESY_ERR_STATE;
  if (c->rx_dma.active) return ESY_ERR_BUSY;
  irqn = uart_irqn(c->phys_index);
  if (irqn == NonMaskableInt_IRQn) return ESY_ERR_NOT_SUPPORTED;

  ret = esy_ring_buffer_init(&c->rx_stream, storage, (uint32_t)storage_size);
  if (ret != ESY_OK) return ret;

  memset(&c->rx_stream_dma, 0, sizeof(c->rx_stream_dma));
  inner = &c->rx_stream_dma.inner;
  if (!esy_board_uart_get_dma_info(c->phys_index, inner, false)) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  inner->irq_index = esy_board_dma_get_irq_index(inner->dreq, inner->channel);
  inner->irq_priority =
      esy_board_dma_get_irq_priority(inner->dreq, inner->channel);

  c->rx_stream_dma.src = (void *)&c->instance->DR;
  c->rx_stream_dma.inner.direction = ESY_DMA_PERIPH_TO_MEM;
  c->rx_stream_dma.dst = storage;
  c->rx_stream_dma.length = storage_size;
  c->rx_stream_dma.transfer_size = ESY_DMA_TRANSFER_SIZE_8;
  c->rx_stream_dma.callback = uart_stream_dma_callback;
  c->rx_stream_dma.callback_arg = uart;
  c->rx_stream_dma.half_callback = uart_stream_dma_callback;
  c->rx_stream_dma.half_callback_arg = uart;
  c->rx_stream_dma.error_callback = uart_stream_dma_error_callback;
  c->rx_stream_dma.error_callback_arg = uart;
  c->rx_stream_dma.circular = true;

  c->rx_stream_callback = callback;
  c->rx_stream_user_data = user_data;
  c->rx_stream_peek_length = 0u;
  c->rx_stream_dma_position = 0u;
  c->rx_stream_overflowed = false;
  c->rx_stream_active = true;

  uart_clear_rx_flags(&c->huart);
  /* Continuous RX owns a distinct circular DMA lifecycle; ordinary UART DMA
   * is finite-only and must not intercept these stream callbacks. */
  ret = esy_hb_dma_prepare(&c->rx_stream_dma, &c->rx_stream_dma);
  if (ret != ESY_OK) {
    c->rx_stream_active = false;
    c->rx_stream_callback = NULL;
    c->rx_stream_user_data = NULL;
    return ret;
  }
  ret = esy_hb_dma_start(&c->rx_stream_dma, 1u << inner->dreq);
  if (ret != ESY_OK) {
    esy_err_t cleanup = esy_hb_uart_rx_stream_stop(uart);
    return cleanup != ESY_OK ? cleanup : ret;
  }

  uart_clear_rx_flags(&c->huart);
  uart_dma_requests(c->instance, false, true);
  __HAL_UART_ENABLE_IT(&c->huart, UART_IT_IDLE);
  HAL_NVIC_SetPriority(irqn, c->irq_priority, 0u);
  HAL_NVIC_EnableIRQ(irqn);
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_rx_stream_peek(esy_uart_t *uart,
                                                const uint8_t **data,
                                                size_t *length) {
  stm32f4_uart_ctx_t *c;
  uint8_t *read_ptr = NULL;
  uint32_t readable;

  if (!uart || uart_context(uart) == NULL || !data || !length) {
    return ESY_ERR_INVALID_ARG;
  }

  c = uart_context(uart);
  *data = NULL;
  *length = 0u;
  if (!c->rx_stream_active) return ESY_ERR_STATE;
  if (c->rx_stream_overflowed) return ESY_ERR_IO;

  readable = esy_ring_buffer_get_read_ptr(&c->rx_stream, &read_ptr);
  c->rx_stream_peek_length = readable;
  *data = read_ptr;
  *length = readable;
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_rx_stream_consume(esy_uart_t *uart,
                                                   size_t length) {
  stm32f4_uart_ctx_t *c;

  if (!uart || uart_context(uart) == NULL || length == 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  c = uart_context(uart);
  if (!c->rx_stream_active) return ESY_ERR_STATE;
  if (c->rx_stream_overflowed) return ESY_ERR_IO;
  if (length > c->rx_stream_peek_length) return ESY_ERR_INVALID_ARG;

  esy_ring_buffer_commit_read(&c->rx_stream, (uint32_t)length);
  c->rx_stream_peek_length = 0u;
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_rx_stream_stop(esy_uart_t *uart) {
  stm32f4_uart_ctx_t *c;
  IRQn_Type irqn;

  if (!uart || uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  c = uart_context(uart);
  if (!c->rx_stream_active) return ESY_OK;

  __HAL_UART_DISABLE_IT(&c->huart, UART_IT_IDLE);
  uart_dma_requests(c->instance, false, false);
  esy_err_t err = esy_hb_dma_abort(&c->rx_stream_dma, c->rx_stream_dma.inner.dreq,
                                       c->rx_stream_dma.inner.channel);
  if (err != ESY_OK) return err;
  irqn = uart_irqn(c->phys_index);
  if (irqn != NonMaskableInt_IRQn) {
    HAL_NVIC_DisableIRQ(irqn);
  }

  c->rx_stream_callback = NULL;
  c->rx_stream_user_data = NULL;
  c->rx_stream_peek_length = 0u;
  c->rx_stream_dma_position = 0u;
  c->rx_stream_overflowed = false;
  c->rx_stream_active = false;
  return ESY_OK;
}

extern void esy_hb_uart_handle_irq(uint8_t phys_index) {
  esy_uart_t *uart;
  stm32f4_uart_ctx_t *c;
  uint32_t status;

  if (phys_index >= STM32F4_UART_MAX) return;
  uart = s_ctx.bound_uarts[phys_index];
  if (!uart || uart_context(uart) == NULL) return;
  c = uart_context(uart);
  if (!c->rx_stream_active) return;

  status = c->huart.Instance->SR;
  if ((status & (USART_SR_PE | USART_SR_FE | USART_SR_NE | USART_SR_ORE)) !=
      0u) {
    uart_clear_rx_flags(&c->huart);
    uart_stream_latch_error(uart, ESY_UART_RX_STREAM_EVENT_ERROR);
    return;
  }

  if ((status & USART_SR_IDLE) != 0u) {
    uart_clear_rx_flags(&c->huart);
    uart_stream_publish_dma_position(uart);
  }
}

extern esy_err_t esy_hb_uart_get_dma_trans_count_remaining(
    const esy_uart_t *uart, size_t *out_tx_remaining,
    size_t *out_rx_remaining) {
  if (!uart || uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  if ((out_tx_remaining == NULL) && (out_rx_remaining == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  if (out_tx_remaining) *out_tx_remaining = 0u;
  if (out_rx_remaining) *out_rx_remaining = 0u;
  return ESY_ERR_NOT_SUPPORTED;
}

extern bool esy_hb_uart_is_transmitting(const esy_uart_t *uart) {
  const stm32f4_uart_ctx_t *c;

  if (!uart || uart_context(uart) == NULL) return false;
  c = uart_context(uart);
  if (!c->enabled || c->suspended) return false;
  return __HAL_UART_GET_FLAG(&c->huart, UART_FLAG_TC) == RESET;
}

extern esy_err_t esy_hb_uart_suspend(esy_uart_t *uart) {
  stm32f4_uart_ctx_t *c;

  if (!uart || uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  c = uart_context(uart);
  if (c->suspended && !c->native_active) return ESY_OK;
  if (!c->enabled) return ESY_ERR_STATE;
  if (c->rx_stream_active || c->tx_dma.active || c->rx_dma.active ||
      esy_hb_uart_is_transmitting(uart)) return ESY_ERR_BUSY;

  c->suspended = true;
  c->enabled = false;
  /* Native deinit may have changed hardware before failing. STATE faults the
   * graph instead of advertising the previous ACTIVE state as recoverable. */
  if (HAL_UART_DeInit(&c->huart) != HAL_OK) return ESY_ERR_STATE;
  c->native_active = false;
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_resume(esy_uart_t *uart) {
    if (!uart || uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
    stm32f4_uart_ctx_t *c = uart_context(uart);
    if (c->enabled && !c->suspended) return ESY_OK;
    if (!c->suspended || c->native_active) return ESY_ERR_STATE;

    c->huart.Instance        = c->instance;
    c->huart.Init.BaudRate   = c->baudrate;
    c->huart.Init.WordLength = to_hal_word_len(c->data_bits, c->parity);
    c->huart.Init.StopBits   = to_hal_stop_bits(c->stop_bits);
    c->huart.Init.Parity     = to_hal_parity(c->parity);
    c->huart.Init.Mode       = UART_MODE_TX_RX;
    c->huart.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    c->huart.Init.OverSampling = UART_OVERSAMPLING_16;

    c->native_active = true;
    if (HAL_UART_Init(&c->huart) != HAL_OK) return ESY_ERR_FAIL;
    c->suspended = false;
    c->enabled   = true;
    return ESY_OK;
}

extern bool esy_hb_uart_is_readable(const esy_uart_t *uart)
{
    if (!uart || uart_context(uart) == NULL) return false;
    const stm32f4_uart_ctx_t *c = uart_context(uart);
    if (!c->enabled || c->suspended) return false;
    return (__HAL_UART_GET_FLAG(&c->huart, UART_FLAG_RXNE) != RESET);
}

extern esy_err_t esy_hb_uart_set_baudrate(esy_uart_t *uart,
                                              uint32_t baudrate)
{
    if (!uart || uart_context(uart) == NULL || !baudrate) return ESY_ERR_INVALID_ARG;
    stm32f4_uart_ctx_t *c = uart_context(uart);
    if (!c->enabled || c->suspended) return ESY_ERR_STATE;

    bool was_enabled = c->enabled && !c->suspended;

    if (c->tx_dma.active || c->rx_dma.active || c->rx_stream_active ||
        esy_hb_uart_is_transmitting(uart)) return ESY_ERR_BUSY;
    c->enabled = false;
    c->suspended = true;
    if (HAL_UART_DeInit(&c->huart) != HAL_OK) return ESY_ERR_IO;
    c->native_active = false;
    c->baudrate = baudrate;

    c->huart.Instance        = c->instance;
    c->huart.Init.BaudRate   = c->baudrate;
    c->huart.Init.WordLength = to_hal_word_len(c->data_bits, c->parity);
    c->huart.Init.StopBits   = to_hal_stop_bits(c->stop_bits);
    c->huart.Init.Parity     = to_hal_parity(c->parity);
    c->huart.Init.Mode       = UART_MODE_TX_RX;
    c->huart.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    c->huart.Init.OverSampling = UART_OVERSAMPLING_16;

    c->native_active = true;
    if (HAL_UART_Init(&c->huart) != HAL_OK) return ESY_ERR_FAIL;

    c->suspended = false;
    c->enabled   = was_enabled;
    return ESY_OK;
}

extern esy_err_t esy_hb_uart_get_caps(const esy_uart_t *uart,
                                          uint32_t *out_caps) {
  if (!uart || uart_context(uart) == NULL || !out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_UART_CAP_TX_DMA | ESY_UART_CAP_RX_DMA |
              ESY_UART_CAP_RX_STREAM;
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_get_phys_index(const esy_uart_t *uart,
                                                uint8_t *out_index) {
    if (!uart || uart_context(uart) == NULL || !out_index) return ESY_ERR_INVALID_ARG;
    const stm32f4_uart_ctx_t *c = uart_context(uart);
    *out_index = c->phys_index;
    return ESY_OK;
}
