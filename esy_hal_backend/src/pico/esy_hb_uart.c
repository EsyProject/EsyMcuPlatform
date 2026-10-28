/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_uart.h"
#include "esy_common/ds/esy_ring_buffer.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"

#include <string.h>
#include <stdatomic.h>
#include "hardware/sync.h"
#include "pico/platform.h"

#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hardware/structs/uart.h"

#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_board/esy_board.h"

#define RP2040_UART_MAX 2
#define PICO_UART_CORE_COUNT 2u
#define UART_RX_IRQ_BYTE_BUDGET 32u

enum { UART_DMA_DONE = 1u, UART_DMA_ERROR = 2u };

typedef struct {
  esy_dma_config_t config;
  _Atomic uint32_t events;
  bool active;
} pico_uart_dma_t;

typedef struct {
    uart_inst_t        *inst;
    uart_hw_t          *hw;
    uint8_t             phys_index;
    bool                enabled;
    bool                suspended;
    uint32_t            baudrate;
    uint8_t             data_bits;
    uint8_t             stop_bits;
    uint8_t             irq_priority;
    esy_uart_parity_t   parity;
    esy_uart_t         *bound_handle;
    pico_uart_dma_t    tx_dma;
    pico_uart_dma_t    rx_dma;
    esy_ring_buffer_t   rx_stream;
    esy_uart_rx_stream_callback_t rx_stream_callback;
    void               *rx_stream_user_data;
    size_t              rx_stream_peek_length;
    bool                rx_stream_active;
    atomic_bool         rx_stream_overflowed;
} rp2040_uart_ctx_t;

_Static_assert(ESY_UART_PRIV_BYTES >= sizeof(rp2040_uart_ctx_t),
               "ESY_UART_PRIV_BYTES too small");

typedef struct {
  esy_uart_t *bound_uarts[RP2040_UART_MAX];
  _Atomic(esy_uart_t *) owners[RP2040_UART_MAX];
  atomic_flag rx_lifecycle;
  bool rx_irq_priority_set[PICO_UART_CORE_COUNT];
  uint8_t rx_irq_priority[PICO_UART_CORE_COUNT];
} esy_hb_uart_ctx_t;
static esy_hb_uart_ctx_t s_ctx = {.rx_lifecycle = ATOMIC_FLAG_INIT};

static rp2040_uart_ctx_t *uart_context(const esy_uart_t *uart);

/* IRQ+task lifecycle exclusion protects handle lookup, callback and storage
 * retirement across cores. Ring endpoints remain SPSC. Local IRQ masking
 * prevents preemption of a task owner; UART IRQs on one core share a priority
 * so they cannot preempt the guard owner. IRQ entry only tries, never waits.
 * The ISR has a fixed byte budget and its callback must be bounded/nonblocking. */
static uint32_t uart_rx_lock(void) {
  uint32_t key = save_and_disable_interrupts();
  while (atomic_flag_test_and_set_explicit(&s_ctx.rx_lifecycle, memory_order_acquire)) {
    tight_loop_contents();
  }
  return key;
}
static void uart_rx_unlock(uint32_t key) {
  atomic_flag_clear_explicit(&s_ctx.rx_lifecycle, memory_order_release);
  restore_interrupts(key);
}

static void uart_stream_handle_irq(uint8_t phys_index) {
  if (phys_index >= RP2040_UART_MAX ||
      atomic_flag_test_and_set_explicit(&s_ctx.rx_lifecycle, memory_order_acquire)) return;
  esy_uart_t *uart = s_ctx.bound_uarts[phys_index];
  rp2040_uart_ctx_t *c = uart ? uart_context(uart) : NULL;
  if (c && c->rx_stream_active && !atomic_load(&c->rx_stream_overflowed)) {
    bool written = false;
    esy_uart_rx_stream_event_t event = ESY_UART_RX_STREAM_EVENT_DATA;
    for (uint32_t i = 0u; i < UART_RX_IRQ_BYTE_BUDGET && uart_is_readable(c->inst); ++i) {
      uint8_t *dst = NULL;
      if (!esy_ring_buffer_get_write_ptr(&c->rx_stream, &dst)) {
        atomic_store(&c->rx_stream_overflowed, true);
        uart_set_irq_enables(c->inst, false, false);
        event = ESY_UART_RX_STREAM_EVENT_OVERFLOW;
        break;
      }
      *dst = (uint8_t)uart_getc(c->inst);
      esy_ring_buffer_commit_write(&c->rx_stream, 1u);
      written = true;
    }
    if ((written || event != ESY_UART_RX_STREAM_EVENT_DATA) && c->rx_stream_callback)
      c->rx_stream_callback(uart, event, c->rx_stream_user_data);
  }
  atomic_flag_clear_explicit(&s_ctx.rx_lifecycle, memory_order_release);
}

static void uart0_stream_irq_handler(void) { uart_stream_handle_irq(0u); }

static void uart1_stream_irq_handler(void) { uart_stream_handle_irq(1u); }

static inline uart_parity_t rp_parity(esy_uart_parity_t p) {
    return (p == ESY_UART_PARITY_NONE) ? UART_PARITY_NONE
         : (p == ESY_UART_PARITY_EVEN) ? UART_PARITY_EVEN
                                       : UART_PARITY_ODD;
}

esy_err_t esy_hb_uart_validate_config(const esy_uart_config_t *cfg) {
    if (cfg == NULL || cfg->phys_index >= RP2040_UART_MAX ||
        cfg->baudrate == 0u || cfg->data_bits == 0u || cfg->data_bits > 9u ||
        (cfg->stop_bits != 1u && cfg->stop_bits != 2u) ||
        (cfg->parity != ESY_UART_PARITY_NONE &&
         cfg->parity != ESY_UART_PARITY_EVEN &&
         cfg->parity != ESY_UART_PARITY_ODD)) return ESY_ERR_INVALID_ARG;
    return cfg->data_bits >= 5u && cfg->data_bits <= 8u
        ? ESY_OK : ESY_ERR_NOT_SUPPORTED;
}

/* Each controller has one owner across init, suspend and failed cleanup.
 * Atomic slot claims cover independent owner tasks/cores; per-handle operations
 * remain serialized by its caller. IRQ dispatch is published only after init. */
static rp2040_uart_ctx_t *uart_context(const esy_uart_t *uart) {
  if (uart == NULL || uart->ctx[0] != (const void *)uart->backend.bytes)
    return NULL;
  /* Establish the concrete backend type through the claim table before reading
   * opaque storage: an XIO handle may also place ctx[0] at backend.bytes. */
  for (uint8_t index = 0u; index < RP2040_UART_MAX; ++index) {
    if (atomic_load_explicit(&s_ctx.owners[index], memory_order_acquire) != uart)
      continue;
    rp2040_uart_ctx_t *c = (rp2040_uart_ctx_t *)uart->ctx[0];
    return c->phys_index == index && c->bound_handle == uart ? c : NULL;
  }
  return NULL;
}

esy_err_t esy_hb_uart_claim(esy_uart_t *uart, uint8_t phys_index) {
  if (uart == NULL || phys_index >= RP2040_UART_MAX) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] != NULL || uart->ctx[1] != NULL) {
    rp2040_uart_ctx_t *c = uart_context(uart);
    return c != NULL && c->phys_index == phys_index && !c->enabled && !c->suspended
               ? ESY_OK : ESY_ERR_STATE;
  }
  esy_uart_t *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          &s_ctx.owners[phys_index], &expected, uart, memory_order_acq_rel,
          memory_order_acquire)) return ESY_ERR_BUSY;
  rp2040_uart_ctx_t *c = (rp2040_uart_ctx_t *)uart->backend.bytes;
  memset(c, 0, sizeof(*c));
  c->phys_index = phys_index;
  c->bound_handle = uart;
  atomic_init(&c->tx_dma.events, 0u);
  atomic_init(&c->rx_dma.events, 0u);
  atomic_init(&c->rx_stream_overflowed, false);
  uart->ctx[0] = c;
  return ESY_OK;
}

esy_err_t esy_hb_uart_release(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] == NULL) return ESY_OK;
  rp2040_uart_ctx_t *c = uart_context(uart);
  if (c == NULL) return ESY_ERR_STATE;
  if (c->enabled || c->tx_dma.active || c->rx_dma.active || c->rx_stream_active) return ESY_ERR_BUSY;
  uint8_t index = c->phys_index;
  uint32_t key = uart_rx_lock();
  s_ctx.bound_uarts[index] = NULL;
  c->bound_handle = NULL;
  uart->ctx[0] = NULL;
  uart_rx_unlock(key);
  atomic_store_explicit(&s_ctx.owners[index], NULL, memory_order_release);
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_init(esy_uart_t *uart,
                                      const esy_uart_config_t *cfg)
{
    if (!uart) return ESY_ERR_INVALID_ARG;
    esy_err_t ret = esy_hb_uart_validate_config(cfg);
    if (ret != ESY_OK) return ret;
    ret = esy_hb_uart_claim(uart, cfg->phys_index);
    if (ret != ESY_OK) return ret;
    rp2040_uart_ctx_t *c = uart_context(uart);
    if(cfg->phys_index == 0) {
      c->inst = uart0;
    }
    else {
      c->inst = uart1;
    }
    c->phys_index = cfg->phys_index;
    c->hw        = uart_get_hw(c->inst);
    c->baudrate  = cfg->baudrate;
    c->data_bits = cfg->data_bits;
    c->stop_bits = cfg->stop_bits;
    c->irq_priority = cfg->inner.irq_priority;
    c->parity    = cfg->parity;

    /* init hw */
    uart_init(c->inst, c->baudrate);
    uart_set_format(c->inst, c->data_bits, c->stop_bits, rp_parity(c->parity));
    uart_set_fifo_enabled(c->inst, true);

    c->enabled   = true;
    c->suspended = false;

    atomic_init(&c->rx_stream_overflowed, false);
    uint32_t key = uart_rx_lock();
    c->bound_handle = uart;
    uart->ctx[0] = c;
    s_ctx.bound_uarts[cfg->phys_index] = uart;
    uart_rx_unlock(key);
    return ESY_OK;
}

esy_err_t esy_hb_uart_deinit(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] == NULL) return ESY_OK;
  rp2040_uart_ctx_t *c = uart_context(uart);
  if (c == NULL) return ESY_ERR_STATE;
  if (c->tx_dma.active || c->rx_dma.active || c->rx_stream_active ||
      esy_hb_uart_is_transmitting(uart)) return ESY_ERR_BUSY;
  if (c->enabled && !c->suspended) uart_deinit(c->inst);
  uint32_t key = uart_rx_lock();
  s_ctx.bound_uarts[c->phys_index] = NULL;
  c->enabled = false;
  c->suspended = true;
  uart_rx_unlock(key);
  return ESY_OK;
}

extern int32_t esy_hb_uart_send_blocking(esy_uart_t *uart,
                                             const uint8_t *data, size_t len)
{
    if (!uart || uart_context(uart) == NULL || !data || !len) return ESY_ERR_INVALID_ARG;
    rp2040_uart_ctx_t *c = uart_context(uart);
    if (!c->enabled || c->suspended) return ESY_ERR_STATE;

    uart_write_blocking(c->inst, data, len);
    return (int32_t)len;
}

extern int32_t esy_hb_uart_receive_blocking(esy_uart_t *uart, uint8_t *data,
                                                size_t len) {
    if (!uart || uart_context(uart) == NULL || !data || !len) return ESY_ERR_INVALID_ARG;
    rp2040_uart_ctx_t *c = uart_context(uart);
    if (!c->enabled || c->suspended) return ESY_ERR_STATE;
    if (c->rx_stream_active || c->rx_dma.active) return ESY_ERR_BUSY;

    uart_read_blocking(c->inst, data, len);
    return (int32_t)len;
}

/* IRQs only publish terminal events. The task owner retires the native
 * dispatch and its pending/peer-core callbacks before releasing caller state. */
static void uart_dma_complete_callback(void *arg) {
  pico_uart_dma_t *direction = arg;
  atomic_fetch_or_explicit(&direction->events, UART_DMA_DONE, memory_order_release);
}
static void uart_dma_error_callback(void *arg) {
  pico_uart_dma_t *direction = arg;
  atomic_fetch_or_explicit(&direction->events, UART_DMA_ERROR, memory_order_release);
}

static esy_err_t uart_start_dma(esy_uart_t *uart, esy_dma_config_t *config,
                                 bool is_tx) {
  if (!uart || uart_context(uart) == NULL || !config || config->length == 0u ||
      (is_tx ? config->src == NULL : config->dst == NULL))
    return ESY_ERR_INVALID_ARG;
  if (config->circular) return ESY_ERR_NOT_SUPPORTED;
  rp2040_uart_ctx_t *c = uart_context(uart);
  if (!c->enabled || c->suspended) return ESY_ERR_STATE;
  pico_uart_dma_t *direction = is_tx ? &c->tx_dma : &c->rx_dma;
  if (direction->active || (!is_tx && c->rx_stream_active)) return ESY_ERR_BUSY;
  esy_dma_config_t transfer = *config;
  transfer.inner.direction = is_tx ? ESY_DMA_MEM_TO_PERIPH : ESY_DMA_PERIPH_TO_MEM;
  if (is_tx) transfer.dst = (void *)&c->hw->dr;
  else transfer.src = (void *)&c->hw->dr;
  transfer.callback = uart_dma_complete_callback;
  transfer.callback_arg = direction;
  transfer.error_callback = uart_dma_error_callback;
  transfer.error_callback_arg = direction;
  memset(&direction->config, 0, sizeof(direction->config));
  esy_err_t result = esy_hb_dma_prepare(direction, &transfer);
  if (result != ESY_OK) return result;
  direction->config = *config;
  atomic_store_explicit(&direction->events, 0u, memory_order_release);
  direction->active = true;
  result = esy_hb_dma_start(direction, 1u << config->inner.channel);
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_hb_uart_abort_dma(uart, is_tx);
    return cleanup != ESY_OK ? cleanup : result;
  }
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
  rp2040_uart_ctx_t *c = uart_context(uart);
  pico_uart_dma_t *direction = is_tx ? &c->tx_dma : &c->rx_dma;
  if (!direction->active) return ESY_OK;
  esy_err_t result = esy_hb_dma_abort(direction, direction->config.inner.dreq,
                                    direction->config.inner.channel);
  if (result != ESY_OK) return result;
  direction->active = false;
  memset(&direction->config, 0, sizeof(direction->config));
  return ESY_OK;
}

bool esy_hb_uart_is_dma_busy(const esy_uart_t *uart, bool is_tx) {
  if (uart_context(uart) == NULL) return false;
  const rp2040_uart_ctx_t *c = uart_context(uart);
  return is_tx ? c->tx_dma.active : c->rx_dma.active;
}

esy_err_t esy_hb_uart_poll(esy_uart_t *uart) {
  if (uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  rp2040_uart_ctx_t *c = uart_context(uart);
  esy_err_t first_error = ESY_OK;
  for (unsigned i = 0u; i < 2u; ++i) {
    pico_uart_dma_t *direction = i == 0u ? &c->tx_dma : &c->rx_dma;
    if (!direction->active) continue;
    uint32_t events = atomic_load_explicit(&direction->events, memory_order_acquire);
    if (events == 0u) continue;
    if ((events & UART_DMA_ERROR) == 0u && i == 0u &&
        (c->hw->fr & UART_UARTFR_BUSY_BITS) != 0u) continue;
    esy_dma_config_t completed = direction->config;
    esy_err_t result = esy_hb_uart_abort_dma(uart, i == 0u);
    if (result != ESY_OK) {
      if (first_error == ESY_OK) first_error = result;
      continue;
    }
    /* Abort excludes pending and peer dispatch; prioritize any late error. */
    events = atomic_load_explicit(&direction->events, memory_order_acquire);
    esy_dma_callback_t callback = (events & UART_DMA_ERROR) != 0u
        ? completed.error_callback : completed.callback;
    void *arg = (events & UART_DMA_ERROR) != 0u
        ? completed.error_callback_arg : completed.callback_arg;
    if (callback != NULL) callback(arg);
  }
  return first_error;
}

extern esy_err_t esy_hb_uart_rx_stream_start(
    esy_uart_t *uart, uint8_t *storage, size_t storage_size,
    esy_uart_rx_stream_callback_t callback, void *user_data) {
  rp2040_uart_ctx_t *c;
  esy_err_t ret;
  uint irq;

  if (!uart || uart_context(uart) == NULL || !storage || storage_size < 2u || !callback ||
      storage_size > UINT32_MAX) {
    return ESY_ERR_INVALID_ARG;
  }

  c = uart_context(uart);
  if (!c->enabled || c->suspended) return ESY_ERR_STATE;
  if (c->rx_stream_active) return ESY_ERR_STATE;
  if (c->rx_dma.active) return ESY_ERR_BUSY;

  uint core = get_core_num();
  if (core >= PICO_UART_CORE_COUNT) return ESY_ERR_STATE;
  uint32_t key = uart_rx_lock();
  /* Static dispatchers stay installed after stop, so their per-core priority
   * remains reserved until reset, including across stream/controller reuse. */
  if (s_ctx.rx_irq_priority_set[core] &&
      s_ctx.rx_irq_priority[core] != c->irq_priority) {
    uart_rx_unlock(key);
    return ESY_ERR_BUSY;
  }
  ret = esy_ring_buffer_init(&c->rx_stream, storage, (uint32_t)storage_size);
  if (ret != ESY_OK) { uart_rx_unlock(key); return ret; }

  s_ctx.rx_irq_priority[core] = c->irq_priority;
  s_ctx.rx_irq_priority_set[core] = true;
  c->rx_stream_callback = callback;
  c->rx_stream_user_data = user_data;
  c->rx_stream_peek_length = 0u;
  c->rx_stream_overflowed = false;
  c->rx_stream_active = true;

  irq = (c->phys_index == 0u) ? UART0_IRQ : UART1_IRQ;
  irq_set_exclusive_handler(irq, (c->phys_index == 0u)
                                     ? uart0_stream_irq_handler
                                     : uart1_stream_irq_handler);
  irq_set_priority(irq, c->irq_priority);
  irq_set_enabled(irq, true);
  uart_set_irq_enables(c->inst, true, false);
  uart_rx_unlock(key);
  return ESY_OK;
}

extern bool esy_hb_uart_is_transmitting(const esy_uart_t *uart) {
  const rp2040_uart_ctx_t *c;

  if (!uart || uart_context(uart) == NULL) return false;
  c = uart_context(uart);
  if (!c->enabled || c->suspended) return false;
  return (c->hw->fr & UART_UARTFR_BUSY_BITS) != 0u;
}

extern esy_err_t esy_hb_uart_rx_stream_peek(esy_uart_t *uart,
                                                const uint8_t **data,
                                                size_t *length) {
  rp2040_uart_ctx_t *c;
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
  rp2040_uart_ctx_t *c;

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
  rp2040_uart_ctx_t *c;

  if (!uart || uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  c = uart_context(uart);
  if (!c->rx_stream_active) return ESY_OK;

  uint32_t key = uart_rx_lock();
  uart_set_irq_enables(c->inst, false, false);
  /* Keep the static dispatcher installed. An IRQ already pending on another
   * core can safely enter later and observe inactive/NULL under the same guard. */
  c->rx_stream_callback = NULL;
  c->rx_stream_user_data = NULL;
  c->rx_stream_peek_length = 0u;
  c->rx_stream_overflowed = false;
  c->rx_stream_active = false;
  uart_rx_unlock(key);
  return ESY_OK;
}

extern void esy_hb_uart_handle_irq(uint8_t phys_index) {
  uart_stream_handle_irq(phys_index);
}

extern esy_err_t esy_hb_uart_suspend(esy_uart_t *uart) {
  rp2040_uart_ctx_t *c;

  if (!uart || uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  c = uart_context(uart);
  if (c->suspended) return ESY_OK;
  if (!c->enabled) return ESY_ERR_STATE;
  if (c->rx_stream_active || c->tx_dma.active || c->rx_dma.active ||
      esy_hb_uart_is_transmitting(uart)) return ESY_ERR_BUSY;

  uart_deinit(c->inst);
  c->enabled = false;
  c->suspended = true;
  return ESY_OK;
}

extern esy_err_t esy_hb_uart_resume(esy_uart_t *uart) {
    if (!uart || uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
    rp2040_uart_ctx_t *c = uart_context(uart);
    if (c->enabled && !c->suspended) return ESY_OK;
    if (!c->suspended) return ESY_ERR_STATE;

    uart_init(c->inst, c->baudrate);
    uart_set_format(c->inst, c->data_bits, c->stop_bits, rp_parity(c->parity));
    uart_set_fifo_enabled(c->inst, true);
    c->enabled = true;
    c->suspended = false;
    return ESY_OK;
}

extern bool esy_hb_uart_is_readable(const esy_uart_t *uart)
{
    if (!uart || uart_context(uart) == NULL) return false;
    const rp2040_uart_ctx_t *c = uart_context(uart);
    if (!c->enabled || c->suspended) return false;
    return uart_is_readable(c->inst);
}

extern esy_err_t esy_hb_uart_set_baudrate(esy_uart_t *uart,
                                              uint32_t baudrate)
{
    if (!uart || uart_context(uart) == NULL || !baudrate) return ESY_ERR_INVALID_ARG;
    rp2040_uart_ctx_t *c = uart_context(uart);
    if (!c->enabled || c->suspended) return ESY_ERR_STATE;

    uart_set_baudrate(c->inst, baudrate);
    c->baudrate = baudrate;
    return ESY_OK;
}

extern esy_err_t esy_hb_uart_get_caps(const esy_uart_t *uart,
                                          uint32_t *out_caps) {
    if (!uart || uart_context(uart) == NULL || !out_caps) return ESY_ERR_INVALID_ARG;
    *out_caps = ESY_UART_CAP_TX_DMA | ESY_UART_CAP_RX_DMA |
                ESY_UART_CAP_RX_STREAM | ESY_UART_CAP_DMA_REMAINING;
    return ESY_OK;
}

extern esy_err_t esy_hb_uart_get_phys_index(const esy_uart_t *uart,
                                                uint8_t *out_index)
{
    if (!uart || uart_context(uart) == NULL || !out_index) return ESY_ERR_INVALID_ARG;
    const rp2040_uart_ctx_t *c = uart_context(uart);
    *out_index = c->phys_index;
    return ESY_OK;
}

extern esy_err_t esy_hb_uart_get_dma_trans_count_remaining(const esy_uart_t *uart,
                                                               size_t *out_tx_remaining,
                                                               size_t *out_rx_remaining) {
  esy_dma_inner_config_t tx_inner = {0};
  esy_dma_inner_config_t rx_inner = {0};
  const rp2040_uart_ctx_t *c;
  esy_err_t ret;

  if (!uart || uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
  if ((out_tx_remaining == NULL) && (out_rx_remaining == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  c = uart_context(uart);
  if (!esy_board_uart_get_dma_info(c->phys_index, &tx_inner, true)) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  if (!esy_board_uart_get_dma_info(c->phys_index, &rx_inner, false)) {
    return ESY_ERR_NOT_SUPPORTED;
  }

  if (out_tx_remaining != NULL) {
    ret = esy_hb_dma_get_trans_count_remaining(
        tx_inner.dreq, tx_inner.channel, out_tx_remaining);
    if (ret != ESY_OK) return ret;
  }
  if (out_rx_remaining != NULL) {
    ret = esy_hb_dma_get_trans_count_remaining(
        rx_inner.dreq, rx_inner.channel, out_rx_remaining);
    if (ret != ESY_OK) return ret;
  }

  return ESY_OK;
}
