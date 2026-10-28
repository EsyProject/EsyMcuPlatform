/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/usb/esy_usb_cdc.h"

#include <string.h>

#include "esy_usb_cdc_backend.h"

/* ------------------------- instance registry by itf ------------------------ */

#define ESY_USB_CDC_LINE_STATE_DTR_MASK 0x01u

typedef struct {
  uint8_t                   itf;
  esy_usb_cdc_rx_cb         rx_cb;
  esy_usb_cdc_line_state_cb line_cb;
  void                     *user_arg;
  uint32_t session_id;
  bool connected;
} esy_usb_cdc_context_t;

typedef struct {
  esy_usb_cdc_t *cdc_by_itf[ESY_USB_CDC_MAX_IFACES];
} esy_usb_cdc_module_ctx_t;

_Static_assert(ESY_USB_CDC_PRIV_BYTES >= sizeof(esy_usb_cdc_context_t),
               "ESY_USB_CDC_PRIV_BYTES too small");

static esy_usb_cdc_module_ctx_t s_ctx;

static inline bool itf_valid(uint8_t itf) {
  uint8_t backend_count = esy_usb_cdc_backend_get_interface_count();
  return itf < ESY_USB_CDC_MAX_IFACES && itf < backend_count;
}

static inline esy_usb_cdc_context_t *ctx_of(esy_usb_cdc_t *cdc) {
  return (esy_usb_cdc_context_t *)(void *)cdc->backend.bytes;
}

static inline const esy_usb_cdc_context_t *ctx_of_const(const esy_usb_cdc_t *cdc) {
  return (const esy_usb_cdc_context_t *)(const void *)cdc->backend.bytes;
}

static bool handle_registered(const esy_usb_cdc_t *cdc) {
  for (size_t itf = 0u; itf < ESY_USB_CDC_MAX_IFACES; ++itf) {
    if (s_ctx.cdc_by_itf[itf] == cdc) {
      return true;
    }
  }

  return false;
}

static void close_session(esy_usb_cdc_context_t *ctx) {
  ctx->connected = false;
  esy_usb_cdc_backend_discard_buffers(ctx->itf);
  ++ctx->session_id;
  if (ctx->session_id == 0u) ++ctx->session_id;
}

static void observe_sessions(void) {
  for (size_t itf = 0u; itf < ESY_USB_CDC_MAX_IFACES; ++itf) {
    esy_usb_cdc_t *cdc = s_ctx.cdc_by_itf[itf];
    if (cdc == NULL) continue;
    esy_usb_cdc_context_t *ctx = ctx_of(cdc);
    if (ctx->session_id == 0u) continue;
    const bool connected = esy_usb_cdc_connected(cdc);
    if (ctx->connected && !connected) close_session(ctx);
    else if (!connected) esy_usb_cdc_backend_discard_buffers(ctx->itf);
    ctx->connected = connected;
  }
}

/* ---------------------------------- API ----------------------------------- */

void esy_usb_cdc_default_config(esy_usb_cdc_config_t *cfg) {
  if (!cfg) return;
  cfg->itf      = 0;
  cfg->rx_cb    = NULL;
  cfg->line_cb  = NULL;
  cfg->user_arg = NULL;
  cfg->discard_on_disconnect = false;
}

esy_err_t esy_usb_cdc_init(esy_usb_cdc_t *cdc, const esy_usb_cdc_config_t *cfg) {
  if (!cdc || !cfg) return ESY_ERR_INVALID_ARG;
  if (!itf_valid(cfg->itf)) return ESY_ERR_INVALID_ARG;

  /* Registry membership is independent of the caller's initial storage bytes. */
  if (handle_registered(cdc)) return ESY_ERR_STATE;
  if (s_ctx.cdc_by_itf[cfg->itf] != NULL) return ESY_ERR_STATE;

  memset(cdc, 0, sizeof(*cdc));

  esy_usb_cdc_context_t *ctx = ctx_of(cdc);
  ctx->itf      = cfg->itf;
  ctx->rx_cb    = cfg->rx_cb;
  ctx->line_cb  = cfg->line_cb;
  ctx->user_arg = cfg->user_arg;
  if (cfg->discard_on_disconnect) {
    ctx->session_id = 1u;
  }

  cdc->ctx = ctx;
  ctx->connected = esy_usb_cdc_connected(cdc);

  s_ctx.cdc_by_itf[cfg->itf] = cdc;
  return ESY_OK;
}

void esy_usb_cdc_deinit(esy_usb_cdc_t *cdc) {
  if (!cdc) return;
  for (size_t itf = 0u; itf < ESY_USB_CDC_MAX_IFACES; ++itf) {
    if (s_ctx.cdc_by_itf[itf] == cdc) {
      s_ctx.cdc_by_itf[itf] = NULL;
      break;
    }
  }

  memset(cdc, 0, sizeof(*cdc));
}

bool esy_usb_cdc_connected(const esy_usb_cdc_t *cdc) {
  if (!cdc || !cdc->ctx) return false;

  const esy_usb_cdc_context_t *ctx = ctx_of_const(cdc);
  if (!itf_valid(ctx->itf)) return false;

  /* DTR indicates the host terminal opened the port for the default backend. */
  uint8_t state = esy_usb_cdc_backend_get_line_state(ctx->itf);
  bool dtr = (state & ESY_USB_CDC_LINE_STATE_DTR_MASK) != 0;

  return esy_usb_cdc_backend_connected(ctx->itf) && dtr;
}

uint32_t esy_usb_cdc_get_session_id(const esy_usb_cdc_t *cdc) {
  if (cdc == NULL || cdc->ctx == NULL) return 0u;
  return ctx_of_const(cdc)->session_id;
}

/* ------------------------------ RX (read) --------------------------------- */

uint32_t esy_usb_cdc_available(const esy_usb_cdc_t *cdc) {
  if (!cdc || !cdc->ctx) return 0;

  const esy_usb_cdc_context_t *ctx = ctx_of_const(cdc);
  if (!itf_valid(ctx->itf)) return 0;

  return esy_usb_cdc_backend_available(ctx->itf);
}

uint32_t esy_usb_cdc_read(esy_usb_cdc_t *cdc, void *buf, uint32_t len) {
  if (!cdc || !cdc->ctx || !buf || (len == 0u)) return 0;

  const esy_usb_cdc_context_t *ctx = ctx_of_const(cdc);
  if (!itf_valid(ctx->itf)) return 0;

  return esy_usb_cdc_backend_read(ctx->itf, buf, len);
}

/* ------------------------------ TX (write) -------------------------------- */

uint32_t esy_usb_cdc_write_available(const esy_usb_cdc_t *cdc) {
  if (!cdc || !cdc->ctx) return 0;

  const esy_usb_cdc_context_t *ctx = ctx_of_const(cdc);
  if (!itf_valid(ctx->itf)) return 0;

  return esy_usb_cdc_backend_write_available(ctx->itf);
}

uint32_t esy_usb_cdc_write(esy_usb_cdc_t *cdc, const void *buf, uint32_t len) {
  if (!cdc || !cdc->ctx || !buf || (len == 0u)) return 0;

  const esy_usb_cdc_context_t *ctx = ctx_of_const(cdc);
  if (!itf_valid(ctx->itf)) return 0;

  /* Non-blocking; may short-write if TX FIFO is full or host is not consuming. */
  return esy_usb_cdc_backend_write(ctx->itf, buf, len);
}

void esy_usb_cdc_flush(esy_usb_cdc_t *cdc) {
  if (!cdc || !cdc->ctx) return;

  const esy_usb_cdc_context_t *ctx = ctx_of_const(cdc);
  if (!itf_valid(ctx->itf)) return;

  esy_usb_cdc_backend_flush(ctx->itf);
}

/* -------------------------------- Task pump -------------------------------- */

void esy_usb_cdc_task(void) {
  /* Bare-metal: call periodically (e.g. 1~2ms). RTOS: run in a dedicated task. */
  observe_sessions();
  esy_usb_cdc_backend_task();
  observe_sessions();
}

/* ----------------------------- USB stack hooks ----------------------------- */

void tud_cdc_rx_cb(uint8_t itf) {
  if (!itf_valid(itf)) return;

  esy_usb_cdc_t *inst = s_ctx.cdc_by_itf[itf];
  if (!inst || !inst->ctx) return;

  esy_usb_cdc_context_t *ctx = ctx_of(inst);

  if (ctx->session_id != 0u && !esy_usb_cdc_connected(inst)) {
    esy_usb_cdc_backend_discard_buffers(itf);
    return;
  }

  if (ctx->rx_cb) {
    ctx->rx_cb(ctx->user_arg, itf);
  }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
  if (!itf_valid(itf)) return;

  esy_usb_cdc_t *inst = s_ctx.cdc_by_itf[itf];
  if (!inst || !inst->ctx) return;

  esy_usb_cdc_context_t *ctx = ctx_of(inst);
  if (ctx->session_id != 0u) {
    if (!dtr) {
      if (ctx->connected) close_session(ctx);
      else esy_usb_cdc_backend_discard_buffers(itf);
    } else {
      ctx->connected = esy_usb_cdc_backend_connected(itf);
    }
  }
  if (ctx->line_cb) {
    ctx->line_cb(ctx->user_arg, itf, dtr, rts);
  }
}
