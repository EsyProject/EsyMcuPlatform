/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/xio/uart_rx/esy_hb_xio_uart_rx.h"

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

#include "esy_hb_xio_uart_rx.pio.h"

#define XIO_UART_PIN_MASK_BITS 32u
#define XIO_UART_CYCLES_PER_BIT 8u

typedef struct {
  esy_hb_xio_program_t prog;
  esy_hb_xio_sm_t sm;
  uint8_t rx_pin;
  uint32_t baudrate;
  uint32_t dma_dreq;
  uint8_t dma_channel;
  esy_dma_callback_t dma_complete_cb;
  void *dma_complete_arg;
  esy_dma_callback_t dma_error_cb;
  void *dma_error_arg;
  _Atomic uint32_t dma_events;
  bool dma_active;
} esy_uart_rx_ctx_t;

typedef struct {
  esy_hb_xio_program_t prog;
  esy_hb_xio_sm_t sm;
  uint8_t tx_pin;
  uint32_t baudrate;
} __tx_ctx_align_probe_t;

#define ESY_ALIGN_UP(v, a) (((v) + ((a) - 1u)) & ~((a) - 1u))

enum {
  ESY_UART_TX_HALF_BYTES = ESY_ALIGN_UP(ESY_UART_PRIV_BYTES / 2u,
                                        alignof(__tx_ctx_align_probe_t))
};

_Static_assert(
    ESY_UART_PRIV_BYTES - ESY_UART_TX_HALF_BYTES >= sizeof(esy_uart_rx_ctx_t),
    "ESY_UART_PRIV_BYTES second half too small for esy_uart_rx_ctx_t");

static inline esy_uart_rx_ctx_t *get_rx_ctx_slot(esy_uart_t *uart) {
  return (esy_uart_rx_ctx_t *)(uart->backend.bytes + ESY_UART_TX_HALF_BYTES);
}

esy_err_t esy_hb_xio_uart_rx_init(esy_uart_t *uart,
                                      const esy_xio_uart_config_t *cfg) {
  if (!uart || !cfg) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[1] != NULL) return ESY_ERR_STATE;
  if (cfg->pin >= XIO_UART_PIN_MASK_BITS || cfg->pin >= NUM_BANK0_GPIOS)
    return ESY_ERR_INVALID_ARG;
  if (cfg->baudrate == 0u || cfg->baudrate > UINT32_MAX / XIO_UART_CYCLES_PER_BIT)
    return ESY_ERR_INVALID_ARG;
  uint32_t clk_sys_hz = clock_get_hz(clk_sys);
  if (clk_sys_hz == 0u) return ESY_ERR_STATE;

  esy_uart_rx_ctx_t *rx_ctx = get_rx_ctx_slot(uart);
  memset(rx_ctx, 0, sizeof(*rx_ctx));
  atomic_init(&rx_ctx->dma_events, 0u);

  esy_hb_xio_program_desc_t d = {
      .instructions = NULL,
      .length = 0,
      .origin = -1,
      .name = "xio_uart_rx",
      .native_prog = &xio_uart_rx_program,
  };
  esy_err_t rc = esy_hb_xio_program_init(&rx_ctx->prog, &d);
  if (rc != ESY_OK) return rc;

  rc = esy_hb_xio_sm_claim_manual(&rx_ctx->prog, &rx_ctx->sm,
                                      cfg->instance, cfg->sm_index);
  if (rc != ESY_OK) {
    (void)esy_hb_xio_program_deinit(&rx_ctx->prog);
    return rc;
  }

  esy_hb_xio_sm_config_t c = (esy_hb_xio_sm_config_t){0};

  c.out_pin_base = ESY_PIN_UNUSED;
  c.set_pin_base = ESY_PIN_UNUSED;
  c.sideset_pin_base = ESY_PIN_UNUSED;
  c.jmp_pin = ESY_PIN_UNUSED;

  c.in_pin_base = cfg->pin;
  c.in_pin_count = 1;
  c.in_shift_right = 1;
  c.in_autopush = 0;
  c.in_push_thresh = 32;
  c.enable_on_init = 0;

  c.clk_div_mul = cfg->baudrate * XIO_UART_CYCLES_PER_BIT;
  c.clk_div_div = clk_sys_hz;

  rc = esy_hb_xio_sm_init(&rx_ctx->sm, &c);
  if (rc != ESY_OK) {
    (void)esy_hb_xio_sm_release(&rx_ctx->sm);
    (void)esy_hb_xio_program_deinit(&rx_ctx->prog);
    return rc;
  }

  uint8_t inst = 0, smi = 0;
  rc = esy_hb_xio_sm_get_location(&rx_ctx->sm, &inst, &smi);
  if (rc != ESY_OK) {
    (void)esy_hb_xio_sm_release(&rx_ctx->sm);
    (void)esy_hb_xio_program_deinit(&rx_ctx->prog);
    return rc;
  }
  PIO pio = inst ? pio1 : pio0;

  pio_gpio_init(pio, cfg->pin);
  pio_sm_set_consecutive_pindirs(pio, smi, cfg->pin, 1, false);
  gpio_pull_up(cfg->pin);

  pio_sm_set_in_pins(pio, smi, cfg->pin);
  pio_sm_set_jmp_pin(pio, smi, cfg->pin);

  pio_sm_clear_fifos(pio, smi);
  pio_sm_restart(pio, smi);

  rc = esy_hb_xio_sm_start(&rx_ctx->sm);
  if (rc != ESY_OK) {
    (void)esy_hb_xio_sm_release(&rx_ctx->sm);
    (void)esy_hb_xio_program_deinit(&rx_ctx->prog);
    return rc;
  }

  rx_ctx->rx_pin = cfg->pin;
  rx_ctx->baudrate = cfg->baudrate;
  uart->ctx[1] = (void *)rx_ctx;
  return ESY_OK;
}

int32_t esy_hb_xio_uart_rx_read_blocking(esy_uart_t *uart,
                                             uint8_t *buf, size_t len) {
  if (!uart || !buf || len == 0u) return ESY_ERR_INVALID_ARG;

  esy_uart_rx_ctx_t *rx_ctx = (esy_uart_rx_ctx_t *)uart->ctx[1];
  if (!rx_ctx) return ESY_ERR_STATE;

  for (uint32_t i = 0; i < (uint32_t)len; ++i) {
    uint32_t word = 0;
    esy_err_t rc = esy_hb_xio_rx_get32_blocking(&rx_ctx->sm, &word);
    if (rc != ESY_OK) return rc;
    buf[i] = (uint8_t)(word >> 24);
  }
  return (int32_t)len;
}

bool esy_hb_xio_uart_rx_is_readable(const esy_uart_t *uart) {
  if (!uart) return false;

  esy_uart_rx_ctx_t *rx_ctx = (esy_uart_rx_ctx_t *)uart->ctx[1];
  if (!rx_ctx) return false;

  esy_hb_xio_sm_t *sm = (esy_hb_xio_sm_t *)&rx_ctx->sm;
  if (!sm->claimed) return false;

  uint8_t inst = 0, sm_index = 0;
  esy_err_t rc = esy_hb_xio_sm_get_location(&rx_ctx->sm, &inst, &sm_index);
  if (rc != ESY_OK) return false;

  PIO pio = (inst == 0) ? pio0 : pio1;

  return !pio_sm_is_rx_fifo_empty(pio, sm_index);
}

enum { XIO_DMA_DONE = 1u, XIO_DMA_ERROR = 2u };

static void uart_rx_dma_complete(void *arg) {
  esy_uart_rx_ctx_t *ctx = arg;
  atomic_fetch_or_explicit(&ctx->dma_events, XIO_DMA_DONE, memory_order_release);
}
static void uart_rx_dma_error(void *arg) {
  esy_uart_rx_ctx_t *ctx = arg;
  atomic_fetch_or_explicit(&ctx->dma_events, XIO_DMA_ERROR, memory_order_release);
}

esy_err_t esy_hb_xio_uart_rx_read_dma(esy_uart_t *uart,
                                          esy_dma_config_t *dma_cfg) {
  if (!uart || !dma_cfg) return ESY_ERR_INVALID_ARG;

  esy_uart_rx_ctx_t *rx_ctx = (esy_uart_rx_ctx_t *)uart->ctx[1];
  if (!rx_ctx) return ESY_ERR_STATE;
  if (rx_ctx->dma_active) return ESY_ERR_BUSY;
  if (dma_cfg->circular) return ESY_ERR_NOT_SUPPORTED;

  uint8_t inst = 0, sm = 0;
  esy_err_t rc = esy_hb_xio_sm_get_location(&rx_ctx->sm, &inst, &sm);
  if (rc != ESY_OK) return rc;

  PIO pio = (inst == 0) ? pio0 : pio1;

  uint8_t *rx_fifo_addr = (uint8_t *)&pio->rxf[sm];
  dma_cfg->src = (void *)(rx_fifo_addr + 3u);
  dma_cfg->inner.direction = ESY_DMA_PERIPH_TO_MEM;

  esy_dma_config_t transfer = *dma_cfg;
  transfer.callback = uart_rx_dma_complete;
  transfer.callback_arg = rx_ctx;
  transfer.error_callback = uart_rx_dma_error;
  transfer.error_callback_arg = rx_ctx;
  esy_err_t result = esy_hb_dma_prepare(rx_ctx, &transfer);
  if (result != ESY_OK) return result;
  rx_ctx->dma_dreq = dma_cfg->inner.dreq;
  rx_ctx->dma_channel = dma_cfg->inner.channel;
  rx_ctx->dma_complete_cb = dma_cfg->callback;
  rx_ctx->dma_complete_arg = dma_cfg->callback_arg;
  rx_ctx->dma_error_cb = dma_cfg->error_callback;
  rx_ctx->dma_error_arg = dma_cfg->error_callback_arg;
  atomic_store_explicit(&rx_ctx->dma_events, 0u, memory_order_release);
  rx_ctx->dma_active = true;
  result = esy_hb_dma_start(rx_ctx, 1u << transfer.inner.channel);
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_hb_xio_uart_rx_abort_dma(uart);
    return cleanup != ESY_OK ? cleanup : result;
  }
  return ESY_OK;
}

esy_err_t esy_hb_xio_uart_rx_abort_dma(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  esy_uart_rx_ctx_t *ctx = uart->ctx[1];
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

esy_err_t esy_hb_xio_uart_rx_poll(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  esy_uart_rx_ctx_t *ctx = uart->ctx[1];
  if (ctx == NULL || !ctx->dma_active) return ESY_OK;
  uint32_t events = atomic_load_explicit(&ctx->dma_events, memory_order_acquire);
  if (events == 0u) return ESY_OK;
  esy_dma_callback_t complete_cb = ctx->dma_complete_cb;
  void *complete_arg = ctx->dma_complete_arg;
  esy_dma_callback_t error_cb = ctx->dma_error_cb;
  void *error_arg = ctx->dma_error_arg;
  esy_err_t result = esy_hb_xio_uart_rx_abort_dma(uart);
  if (result != ESY_OK) return result;
  events = atomic_load_explicit(&ctx->dma_events, memory_order_acquire);
  esy_dma_callback_t callback = (events & XIO_DMA_ERROR) != 0u
      ? error_cb : complete_cb;
  void *arg = (events & XIO_DMA_ERROR) != 0u
      ? error_arg : complete_arg;
  if (callback != NULL) callback(arg);
  return ESY_OK;
}

uint8_t esy_hb_xio_uart_rx_get_pin(esy_uart_t *uart) {
  if (!uart) return ESY_PIN_UNUSED;
  esy_uart_rx_ctx_t *rx_ctx = (esy_uart_rx_ctx_t *)uart->ctx[1];
  return rx_ctx ? rx_ctx->rx_pin : ESY_PIN_UNUSED;
}

void esy_hb_xio_uart_rx_deinit(esy_uart_t *uart) {
  if (!uart) return;

  esy_uart_rx_ctx_t *rx_ctx = (esy_uart_rx_ctx_t *)uart->ctx[1];
  if (!rx_ctx) return;
  if (rx_ctx->dma_active) return;

  (void)esy_hb_xio_sm_stop(&rx_ctx->sm);
  (void)esy_hb_xio_sm_release(&rx_ctx->sm);
  (void)esy_hb_xio_program_deinit(&rx_ctx->prog);

  memset(rx_ctx, 0, sizeof(*rx_ctx));
  uart->ctx[1] = NULL;
}
