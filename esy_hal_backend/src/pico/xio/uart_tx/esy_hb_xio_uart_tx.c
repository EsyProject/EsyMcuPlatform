/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/xio/uart_tx/esy_hb_xio_uart_tx.h"

#include <string.h>
#include <stdalign.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "esy_hal_backend/esy_hb_xio.h"
#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_uart_defs.h"

#include "esy_hb_xio_uart_tx.pio.h"

#define XIO_UART_PIN_MASK_BITS 32u
#define XIO_UART_CYCLES_PER_BIT 8u

typedef struct {
  esy_hb_xio_program_t prog;
  esy_hb_xio_sm_t sm;
  uint8_t tx_pin;
  uint32_t baudrate;
  uint32_t dma_dreq;
  uint8_t dma_channel;
  esy_dma_callback_t dma_complete_cb;
  void *dma_complete_arg;
  esy_dma_callback_t dma_error_cb;
  void *dma_error_arg;
  _Atomic uint32_t dma_events;
  bool dma_active;
  bool tx_pending;
  bool wire_check_started;
} esy_uart_tx_ctx_t;

#define ESY_ALIGN_UP(v, a) (((v) + ((a) - 1u)) & ~((a) - 1u))
enum {
  ESY_UART_TX_HALF_BYTES =
      ESY_ALIGN_UP(ESY_UART_PRIV_BYTES / 2u, alignof(esy_uart_tx_ctx_t))
};

_Static_assert(
    ESY_UART_TX_HALF_BYTES >= sizeof(esy_uart_tx_ctx_t),
    "ESY_UART_PRIV_BYTES first half too small for esy_uart_tx_ctx_t");

static inline esy_uart_tx_ctx_t *get_tx_ctx_slot(esy_uart_t *uart) {
  return (esy_uart_tx_ctx_t *)(uart->backend.bytes);
}

bool esy_hb_xio_uart_tx_is_busy(esy_uart_t *uart) {
  if (uart == NULL || uart->ctx[0] == NULL) return false;
  esy_uart_tx_ctx_t *ctx = uart->ctx[0];
  if (ctx->dma_active) return true;
  if (!ctx->tx_pending) return false;

  uint8_t instance = 0u, sm = 0u;
  uint32_t entry = 0u;
  if (esy_hb_xio_sm_get_location(&ctx->sm, &instance, &sm) != ESY_OK ||
      esy_hb_xio_sm_get_entry_offset(&ctx->sm, &entry) != ESY_OK)
    return true;
  PIO pio = instance == 0u ? pio0 : pio1;
  if ((pio->ctrl & (1u << (PIO_CTRL_SM_ENABLE_LSB + sm))) == 0u ||
      !pio_sm_is_tx_fifo_empty(pio, sm)) return true;

  /* One task/core owns this state and TXSTALL; IRQs only publish DMA events.
   * FIFO-empty does not include the OSR/stop-bit delay. Discard stalls from
   * earlier FIFO starvation, then require a new blocking PULL at the entry.
   * The program reaches that PULL only after the final stop bit's full delay.
   * No producer may enqueue between these observations except the tracked DMA,
   * which must already be retired above. */
  const uint32_t stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
  if (!ctx->wire_check_started) {
    pio->fdebug = stall_mask;
    ctx->wire_check_started = true;
    return true;
  }
  if ((pio->fdebug & stall_mask) == 0u || pio_sm_get_pc(pio, sm) != entry)
    return true;
  ctx->tx_pending = false;
  return false;
}

esy_err_t esy_hb_xio_uart_tx_init(esy_uart_t *uart,
                                      const esy_xio_uart_config_t *cfg) {
  if (!uart || !cfg) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] != NULL) return ESY_ERR_STATE;
  if (cfg->pin >= XIO_UART_PIN_MASK_BITS || cfg->pin >= NUM_BANK0_GPIOS)
    return ESY_ERR_INVALID_ARG;
  if (cfg->baudrate == 0u || cfg->baudrate > UINT32_MAX / XIO_UART_CYCLES_PER_BIT)
    return ESY_ERR_INVALID_ARG;
  uint32_t clk_sys_hz = clock_get_hz(clk_sys);
  if (clk_sys_hz == 0u) return ESY_ERR_STATE;

  esy_uart_tx_ctx_t *tx_ctx = get_tx_ctx_slot(uart);
  memset(tx_ctx, 0, sizeof(*tx_ctx));
  atomic_init(&tx_ctx->dma_events, 0u);

  esy_hb_xio_program_desc_t d = {
      .instructions = NULL,
      .length = 0,
      .origin = -1,
      .name = "xio_uart_tx",
      .native_prog = &xio_uart_tx_program,
  };
  esy_err_t rc = esy_hb_xio_program_init(&tx_ctx->prog, &d);
  if (rc != ESY_OK) return rc;

  rc = esy_hb_xio_sm_claim_manual(&tx_ctx->prog, &tx_ctx->sm,
                                      cfg->instance, cfg->sm_index);
  if (rc != ESY_OK) {
    (void)esy_hb_xio_program_deinit(&tx_ctx->prog);
    return rc;
  }

  esy_hb_xio_sm_config_t c = (esy_hb_xio_sm_config_t){0};

  c.out_pin_base = ESY_PIN_UNUSED;
  c.set_pin_base = ESY_PIN_UNUSED;
  c.sideset_pin_base = ESY_PIN_UNUSED;
  c.jmp_pin = ESY_PIN_UNUSED;

  c.out_pin_base = cfg->pin;
  c.out_pin_count = 1;
  c.set_pin_base = cfg->pin;
  c.set_pin_count = 1;

  c.out_shift_right = 1;
  c.out_autopull = 0;
  c.out_pull_thresh = 0;

  c.enable_on_init = 0;

  c.clk_div_mul = cfg->baudrate * XIO_UART_CYCLES_PER_BIT;
  c.clk_div_div = clk_sys_hz;

  rc = esy_hb_xio_sm_init(&tx_ctx->sm, &c);
  if (rc != ESY_OK) {
    (void)esy_hb_xio_sm_release(&tx_ctx->sm);
    (void)esy_hb_xio_program_deinit(&tx_ctx->prog);
    return rc;
  }

  (void)esy_hb_xio_write_pins_mask(&tx_ctx->sm, cfg->pin, 1, 1u);

  rc = esy_hb_xio_sm_start(&tx_ctx->sm);
  if (rc != ESY_OK) {
    (void)esy_hb_xio_sm_release(&tx_ctx->sm);
    (void)esy_hb_xio_program_deinit(&tx_ctx->prog);
    return rc;
  }

  tx_ctx->tx_pin = cfg->pin;
  tx_ctx->baudrate = cfg->baudrate;
  uart->ctx[0] = (void *)tx_ctx;
  return ESY_OK;
}

int32_t esy_hb_xio_uart_tx_write_blocking(esy_uart_t *uart,
                                              const uint8_t *data, size_t len) {
  if (!uart || !data || len == 0u) return ESY_ERR_INVALID_ARG;

  esy_uart_tx_ctx_t *tx_ctx = (esy_uart_tx_ctx_t *)uart->ctx[0];
  if (!tx_ctx) return ESY_ERR_STATE;
  if (len > INT32_MAX) return ESY_ERR_INVALID_ARG;
  if (tx_ctx->dma_active) return ESY_ERR_BUSY;
  uint8_t instance = 0u, sm = 0u;
  if (esy_hb_xio_sm_get_location(&tx_ctx->sm, &instance, &sm) != ESY_OK)
    return ESY_ERR_STATE;
  PIO pio = instance == 0u ? pio0 : pio1;
  if ((pio->ctrl & (1u << (PIO_CTRL_SM_ENABLE_LSB + sm))) == 0u)
    return ESY_ERR_STATE;

  tx_ctx->tx_pending = true;
  tx_ctx->wire_check_started = false;

  for (uint32_t i = 0; i < (uint32_t)len; ++i) {
    uint32_t word = (uint32_t)data[i];
    esy_err_t rc = esy_hb_xio_tx_put32_blocking(&tx_ctx->sm, word);
    if (rc != ESY_OK) return rc;
  }
  while (esy_hb_xio_uart_tx_is_busy(uart)) tight_loop_contents();
  return (int32_t)len;
}

enum { XIO_DMA_DONE = 1u, XIO_DMA_ERROR = 2u };

static void uart_tx_dma_complete(void *arg) {
  esy_uart_tx_ctx_t *ctx = arg;
  atomic_fetch_or_explicit(&ctx->dma_events, XIO_DMA_DONE, memory_order_release);
}
static void uart_tx_dma_error(void *arg) {
  esy_uart_tx_ctx_t *ctx = arg;
  atomic_fetch_or_explicit(&ctx->dma_events, XIO_DMA_ERROR, memory_order_release);
}

esy_err_t esy_hb_xio_uart_tx_write_dma(esy_uart_t *uart,
                                           esy_dma_config_t *dma_cfg) {
  if (!uart || !dma_cfg) return ESY_ERR_INVALID_ARG;

  esy_uart_tx_ctx_t *tx_ctx = (esy_uart_tx_ctx_t *)uart->ctx[0];
  if (!tx_ctx) return ESY_ERR_STATE;
  if (tx_ctx->dma_active) return ESY_ERR_BUSY;
  if (dma_cfg->circular) return ESY_ERR_NOT_SUPPORTED;

  uint8_t inst = 0, sm = 0;
  esy_err_t rc = esy_hb_xio_sm_get_location(&tx_ctx->sm, &inst, &sm);
  if (rc != ESY_OK) return rc;

  PIO pio = (inst == 0) ? pio0 : pio1;

  dma_cfg->inner.direction = ESY_DMA_MEM_TO_PERIPH;
  dma_cfg->dst = (void *)&pio->txf[sm];

  esy_dma_config_t transfer = *dma_cfg;
  transfer.callback = uart_tx_dma_complete;
  transfer.callback_arg = tx_ctx;
  transfer.error_callback = uart_tx_dma_error;
  transfer.error_callback_arg = tx_ctx;
  esy_err_t result = esy_hb_dma_prepare(tx_ctx, &transfer);
  if (result != ESY_OK) return result;
  tx_ctx->dma_dreq = dma_cfg->inner.dreq;
  tx_ctx->dma_channel = dma_cfg->inner.channel;
  tx_ctx->dma_complete_cb = dma_cfg->callback;
  tx_ctx->dma_complete_arg = dma_cfg->callback_arg;
  tx_ctx->dma_error_cb = dma_cfg->error_callback;
  tx_ctx->dma_error_arg = dma_cfg->error_callback_arg;
  atomic_store_explicit(&tx_ctx->dma_events, 0u, memory_order_release);
  tx_ctx->dma_active = true;
  tx_ctx->tx_pending = true;
  tx_ctx->wire_check_started = false;
  result = esy_hb_dma_start(tx_ctx, 1u << transfer.inner.channel);
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_hb_xio_uart_tx_abort_dma(uart);
    return cleanup != ESY_OK ? cleanup : result;
  }
  return ESY_OK;
}

esy_err_t esy_hb_xio_uart_tx_abort_dma(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_ctx_t *ctx = uart->ctx[0];
  if (ctx == NULL || !ctx->dma_active) return ESY_OK;
  esy_err_t result = esy_hb_dma_abort(ctx, ctx->dma_dreq, ctx->dma_channel);
  if (result != ESY_OK) return result;
  ctx->dma_active = false;
  ctx->dma_complete_cb = NULL;
  ctx->dma_complete_arg = NULL;
  ctx->dma_error_cb = NULL;
  ctx->dma_error_arg = NULL;
  return ESY_OK;
}

esy_err_t esy_hb_xio_uart_tx_poll(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_ctx_t *ctx = uart->ctx[0];
  if (ctx == NULL || !ctx->dma_active) return ESY_OK;
  uint32_t events = atomic_load_explicit(&ctx->dma_events, memory_order_acquire);
  if (events == 0u) return ESY_OK;
  esy_dma_callback_t complete_cb = ctx->dma_complete_cb;
  void *complete_arg = ctx->dma_complete_arg;
  esy_dma_callback_t error_cb = ctx->dma_error_cb;
  void *error_arg = ctx->dma_error_arg;
  esy_err_t result = esy_hb_xio_uart_tx_abort_dma(uart);
  if (result != ESY_OK) return result;
  events = atomic_load_explicit(&ctx->dma_events, memory_order_acquire);
  esy_dma_callback_t callback = (events & XIO_DMA_ERROR) != 0u
      ? error_cb : complete_cb;
  void *arg = (events & XIO_DMA_ERROR) != 0u
      ? error_arg : complete_arg;
  if (callback != NULL) callback(arg);
  return ESY_OK;
}

uint8_t esy_hb_xio_uart_tx_get_pin(esy_uart_t *uart) {
  if (!uart) return ESY_PIN_UNUSED;

  esy_uart_tx_ctx_t *tx_ctx = (esy_uart_tx_ctx_t *)uart->ctx[0];
  return tx_ctx ? tx_ctx->tx_pin : ESY_PIN_UNUSED;
}

void esy_hb_xio_uart_tx_deinit(esy_uart_t *uart) {
  if (!uart) return;

  esy_uart_tx_ctx_t *tx_ctx = (esy_uart_tx_ctx_t *)uart->ctx[0];
  if (!tx_ctx) return;
  if (esy_hb_xio_uart_tx_is_busy(uart)) return;

  (void)esy_hb_xio_sm_stop(&tx_ctx->sm);
  (void)esy_hb_xio_sm_release(&tx_ctx->sm);
  (void)esy_hb_xio_program_deinit(&tx_ctx->prog);

  memset(tx_ctx, 0, sizeof(*tx_ctx));
  uart->ctx[0] = NULL;
}
