/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_modbus_manager.h"

#include <string.h>

#include "esy_hal/esy_hal_system.h"
#include "esy_common/utils/base/esy_seq.h"

#define MODBUS_EXCEPTION_FUNC_MASK 0x80u

/* -------------------------------------------------------------------------- */
/* Private context (Scheme C)                                                 */
/* -------------------------------------------------------------------------- */

/* Master private context stored inside esy_proto_modbus_master_t::priv.bytes */
typedef struct {
  esy_proto_modbus_master_config_t cfg;
  bool initialized;
  bool tx_pending;
  bool aborting;
  bool closing;
  esy_proto_modbus_master_state_t state;

  esy_prio_slot_queue_t queue; /* Slot metadata only */
  esy_proto_modbus_payload_t *payloads;
  uint16_t slot_count;

  int16_t current_index;        /* -1 means none */
  uint32_t current_deadline_ms; /* absolute tick */
  uint8_t current_retry_count;
  uint8_t current_retry_max;
  uint32_t current_timeout_ms;

  uint16_t current_transaction_id;
  uint16_t transaction_id; /* TCP: incremented per request */
} esy_proto_modbus_master_ctx_t;

/* Slave private context stored inside esy_proto_modbus_slave_t::priv.bytes */
typedef struct {
  esy_proto_modbus_slave_config_t cfg;
  bool initialized;

  uint8_t *tx_frame_buf;
  uint16_t tx_frame_buf_size;

} esy_proto_modbus_slave_ctx_t;

/* Ensure opaque storage is large enough. */
_Static_assert(ESY_PROTO_MODBUS_MASTER_PRIV_BYTES >=
                   sizeof(esy_proto_modbus_master_ctx_t),
               "ESY_PROTO_MODBUS_MASTER_PRIV_BYTES too small");
_Static_assert(ESY_PROTO_MODBUS_SLAVE_PRIV_BYTES >=
                   sizeof(esy_proto_modbus_slave_ctx_t),
               "ESY_PROTO_MODBUS_SLAVE_PRIV_BYTES too small");

static inline esy_proto_modbus_master_ctx_t *master_ctx(
    esy_proto_modbus_master_t *m) {
  return (esy_proto_modbus_master_ctx_t *)(void *)m->priv.bytes;
}

static inline const esy_proto_modbus_master_ctx_t *master_ctx_c(
    const esy_proto_modbus_master_t *m) {
  return (const esy_proto_modbus_master_ctx_t *)(const void *)m->priv.bytes;
}

static inline esy_proto_modbus_slave_ctx_t *slave_ctx(
    esy_proto_modbus_slave_t *s) {
  return (esy_proto_modbus_slave_ctx_t *)(void *)s->priv.bytes;
}

static inline const esy_proto_modbus_slave_ctx_t *slave_ctx_c(
    const esy_proto_modbus_slave_t *s) {
  return (const esy_proto_modbus_slave_ctx_t *)(const void *)s->priv.bytes;
}

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

static inline uint8_t clamp_prio(esy_proto_modbus_priority_t p) {
  if (p > ESY_PROTO_MODBUS_PRIORITY_HIGH)
    return (uint8_t)ESY_PROTO_MODBUS_PRIORITY_HIGH;
  return (uint8_t)p;
}

/* Base function code for exception responses (raw | 0x80). */
static inline esy_proto_modbus_func_t func_base(uint8_t raw_func) {
  return (esy_proto_modbus_func_t)(raw_func & 0x7Fu);
}

/* -------------------------------------------------------------------------- */
/* Master default cfg                                                         */
/* -------------------------------------------------------------------------- */

esy_proto_modbus_master_config_t esy_proto_modbus_master_default_cfg(void) {
  esy_proto_modbus_master_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.frame_type = ESY_PROTO_MODBUS_FRAME_RTU;
  cfg.timeout_ms = 100u;
  cfg.retry_max = 0u;
  return cfg;
}

/* -------------------------------------------------------------------------- */
/* Master internal */
/* -------------------------------------------------------------------------- */

static void master_finish_current(
    esy_proto_modbus_master_t *m, esy_proto_modbus_req_status_t status,
    const esy_proto_modbus_frame_view_t *resp_view) {
  if (!m) return;

  esy_proto_modbus_master_ctx_t *ctx = master_ctx(m);
  if (ctx->current_index < 0) return;

  const uint16_t idx = (uint16_t)ctx->current_index;
  esy_proto_modbus_payload_t *pl = &ctx->payloads[idx];

  const uint8_t unit_id = pl->unit_id;
  const esy_proto_modbus_func_t fc =
      (resp_view) ? func_base(resp_view->func) : pl->func_code;

  const uint8_t *resp_pdu = (resp_view) ? resp_view->pdu : NULL;
  const uint16_t resp_pdu_len = (resp_view) ? resp_view->pdu_len : 0;

  const esy_proto_modbus_request_complete_cb callback = pl->on_complete;
  void* user_ctx = pl->user_ctx;

  /* Free slot metadata; payload memory is owned by caller and remains
   * allocated. */
  esy_prio_slot_queue_free_slot(&ctx->queue, idx);

  /* Clear current transaction. */
  ctx->current_index = -1;
  ctx->tx_pending = false;
  ctx->state = ESY_PROTO_MODBUS_MASTER_STATE_IDLE;
  ctx->current_retry_count = 0;
  ctx->current_deadline_ms = 0;
  ctx->current_retry_max = 0;
  ctx->current_timeout_ms = 0;
  if (callback)
    callback(m, status, unit_id, fc, resp_pdu, resp_pdu_len, user_ctx);
}

static esy_err_t master_build_and_send_current(esy_proto_modbus_master_t *m) {
  if (!m) return ESY_ERR_STATE;

  esy_proto_modbus_master_ctx_t *ctx = master_ctx(m);

  if (ctx->current_index < 0) return ESY_ERR_STATE;
  if (!ctx->cfg.tx_fn || !ctx->cfg.tx_frame_buf ||
      ctx->cfg.tx_frame_buf_size == 0) {
    return ESY_ERR_STATE;
  }

  if (ctx->cfg.is_tx_busy && ctx->cfg.is_tx_busy(ctx->cfg.tx_user_ctx))
    return ESY_ERR_BUSY;
  const uint16_t idx = (uint16_t)ctx->current_index;
  const esy_proto_modbus_payload_t *pl = &ctx->payloads[idx];

  esy_proto_modbus_frame_t f;
  (void)esy_proto_modbus_frame_init(&f, ctx->cfg.tx_frame_buf,
                                    ctx->cfg.tx_frame_buf_size);

  esy_err_t br = ESY_ERR_FAIL;

  if (ctx->cfg.frame_type == ESY_PROTO_MODBUS_FRAME_RTU) {
    br =
        esy_proto_modbus_build_rtu_frame(&f, pl->unit_id, pl->pdu, pl->pdu_len);
  } else {
    /* TCP: transaction id increments per request */
    const uint16_t tid = ctx->current_transaction_id;
    br = esy_proto_modbus_build_tcp_frame(&f, tid, pl->unit_id, pl->pdu,
                                          pl->pdu_len);
  }

  if (br != ESY_OK) return br;

  esy_err_t txr = ctx->cfg.tx_fn(f.buf, (uint16_t)f.len, ctx->cfg.tx_user_ctx);
  if (txr != ESY_OK) return txr;

  if (ctx->cfg.defer_tx_completion) {
    ctx->tx_pending = true;
    ctx->state = ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP;
    return ESY_OK;
  }

  /* RTU broadcasts have no response and must never be retried. The busy
   * hook still protects the borrowed workspace before the next request. */
  if (ctx->cfg.frame_type == ESY_PROTO_MODBUS_FRAME_RTU && pl->unit_id == 0u) {
    master_finish_current(m, ESY_PROTO_MODBUS_REQ_STATUS_OK, NULL);
    return ESY_OK;
  }
  /* Arm waiting state and deadline. */
  ctx->state = ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP;
  const uint32_t tnow = esy_hal_system_get_tick();
  ctx->current_deadline_ms = tnow + ctx->current_timeout_ms;

  return ESY_OK;
}

static void master_try_start_next(esy_proto_modbus_master_t *m) {
  if (!m) return;

  esy_proto_modbus_master_ctx_t *ctx = master_ctx(m);
  if (!ctx->initialized) return;
  if (ctx->state != ESY_PROTO_MODBUS_MASTER_STATE_IDLE) return;

  if (ctx->cfg.is_tx_busy && ctx->cfg.is_tx_busy(ctx->cfg.tx_user_ctx)) return;
  int16_t next = esy_prio_slot_queue_pick_next(&ctx->queue);
  if (next < 0) return;

  const uint16_t idx = (uint16_t)next;
  esy_prio_slot_queue_mark_in_progress(&ctx->queue, idx);

  ctx->current_index = (int16_t)idx;
  ctx->current_retry_count = 0;
  ctx->current_transaction_id = ctx->transaction_id++;

  const esy_proto_modbus_payload_t *pl = &ctx->payloads[idx];
  ctx->current_timeout_ms =
      (pl->timeout_ms != 0u)
          ? pl->timeout_ms
          : ((ctx->cfg.timeout_ms != 0u) ? ctx->cfg.timeout_ms : 100u);
  ctx->current_retry_max =
      (pl->retry_max != 0u) ? pl->retry_max : ctx->cfg.retry_max;

  if (master_build_and_send_current(m) != ESY_OK) {
    master_finish_current(m, ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, NULL);
  }
}

/* -------------------------------------------------------------------------- */
/* Master API                                                                 */
/* -------------------------------------------------------------------------- */

esy_err_t esy_proto_modbus_master_init(
    esy_proto_modbus_master_t *master,
    const esy_proto_modbus_master_config_t *config) {
  if (!master || !config) return ESY_ERR_INVALID_ARG;
  if (master_ctx(master)->initialized) return ESY_ERR_STATE;
  if (config->timeout_ms > INT32_MAX || config->slot_count > INT16_MAX ||
      (config->frame_type != ESY_PROTO_MODBUS_FRAME_RTU &&
       config->frame_type != ESY_PROTO_MODBUS_FRAME_TCP))
    return ESY_ERR_INVALID_ARG;
  if (!config->tx_fn || (config->defer_tx_completion && !config->is_tx_busy))
    return ESY_ERR_INVALID_ARG;
  if (!config->tx_frame_buf || config->tx_frame_buf_size == 0)
    return ESY_ERR_INVALID_ARG;
  if (!config->slots || !config->payloads || config->slot_count == 0)
    return ESY_ERR_INVALID_ARG;

  /* Clear opaque handle storage. */
  memset(master, 0, sizeof(*master));

  esy_proto_modbus_master_ctx_t *ctx = master_ctx(master);
  memset(ctx, 0, sizeof(*ctx));

  ctx->cfg = *config;
  ctx->payloads = config->payloads;
  ctx->slot_count = config->slot_count;
  ctx->state = ESY_PROTO_MODBUS_MASTER_STATE_IDLE;
  ctx->current_index = -1;
  ctx->transaction_id = 1;

  esy_prio_slot_queue_config_t qcfg = esy_prio_slot_queue_default_cfg();
  qcfg.slots = config->slots;
  qcfg.slot_count = config->slot_count;

  esy_err_t er = esy_prio_slot_queue_init(&ctx->queue, &qcfg);
  if (er != ESY_OK) {
    memset(master, 0, sizeof(*master));
    return er;
  }

  ctx->initialized = true;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_master_deinit(esy_proto_modbus_master_t* master) {
  if (!master) return ESY_ERR_INVALID_ARG;
  esy_proto_modbus_master_ctx_t *ctx = master_ctx(master);
  if (ctx->initialized && ctx->cfg.is_tx_busy &&
      ctx->cfg.is_tx_busy(ctx->cfg.tx_user_ctx))
    return ESY_ERR_BUSY;
  if (ctx->initialized) {
    ctx->closing = true;
    (void)esy_proto_modbus_master_abort_all(master, true);
  }
  esy_prio_slot_queue_deinit(&ctx->queue);

  memset(master, 0, sizeof(*master));
  return ESY_OK;
}

bool esy_proto_modbus_master_is_initialized(
    const esy_proto_modbus_master_t *master) {
  if (!master) return false;
  const esy_proto_modbus_master_ctx_t *ctx = master_ctx_c(master);
  return ctx->initialized;
}

esy_proto_modbus_master_state_t esy_proto_modbus_master_get_state(
    const esy_proto_modbus_master_t *master) {
  if (!master) return ESY_PROTO_MODBUS_MASTER_STATE_IDLE;

  const esy_proto_modbus_master_ctx_t *ctx = master_ctx_c(master);
  if (!ctx->initialized) return ESY_PROTO_MODBUS_MASTER_STATE_IDLE;

  return ctx->state;
}

uint16_t esy_proto_modbus_master_get_queue_depth(
    const esy_proto_modbus_master_t *master) {
  if (!master) return 0;

  const esy_proto_modbus_master_ctx_t *ctx = master_ctx_c(master);
  if (!ctx->initialized) return 0;

  return esy_prio_slot_queue_get_depth(&ctx->queue);
}

esy_err_t esy_proto_modbus_master_submit(
    esy_proto_modbus_master_t *master, const esy_proto_modbus_request_t *req) {
  if (!master || !req) return ESY_ERR_INVALID_ARG;

  esy_proto_modbus_master_ctx_t *ctx = master_ctx(master);
  if (!ctx->initialized) return ESY_ERR_INVALID_ARG;
  if (ctx->closing) return ESY_ERR_STATE;

  if (req->timeout_ms > INT32_MAX || req->pdu[0] != (uint8_t)req->func_code)
    return ESY_ERR_INVALID_ARG;
  if (req->pdu_len == 0 || req->pdu_len > ESY_PROTO_MODBUS_MANAGER_MAX_PDU_LEN)
    return ESY_ERR_INVALID_ARG;

  /* Allocate slot. */
  int16_t slot = esy_prio_slot_queue_alloc_slot(&ctx->queue, (uintptr_t)0);
  if (slot < 0) return ESY_ERR_NO_MEM;

  const uint16_t idx = (uint16_t)slot;
  esy_proto_modbus_payload_t *pl = &ctx->payloads[idx];
  memset(pl, 0, sizeof(*pl));

  pl->unit_id = req->unit_id;
  pl->func_code = req->func_code;
  pl->pdu_len = req->pdu_len;
  memcpy(pl->pdu, req->pdu, req->pdu_len);

  pl->priority = clamp_prio(req->priority);
  pl->timeout_ms = req->timeout_ms;
  pl->retry_max = req->retry_max;

  pl->on_complete = req->on_complete;
  pl->user_ctx = req->user_ctx;

  esy_err_t er = esy_prio_slot_queue_enqueue(&ctx->queue, idx, pl->priority);
  if (er != ESY_OK) {
    esy_prio_slot_queue_free_slot(&ctx->queue, idx);
    return er;
  }

  return ESY_OK;
}

void esy_proto_modbus_master_on_rx_frame(esy_proto_modbus_master_t *master,
                                         const uint8_t *frame,
                                         uint16_t frame_len) {
  if (!master || !frame || frame_len == 0) return;

  esy_proto_modbus_master_ctx_t *ctx = master_ctx(master);
  if (!ctx->initialized) return;
  if (ctx->state != ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP) return;
  if (ctx->current_index < 0 || ctx->tx_pending) return;

  esy_proto_modbus_frame_view_t view;
  esy_err_t pr = esy_proto_modbus_parse_frame(ctx->cfg.frame_type, frame,
                                              frame_len, &view);
  if (pr != ESY_OK) {
    master_finish_current(master, ESY_PROTO_MODBUS_REQ_STATUS_BAD_FRAME, NULL);
    return;
  }

  if (ctx->cfg.frame_type == ESY_PROTO_MODBUS_FRAME_TCP &&
      view.transaction_id != ctx->current_transaction_id)
    return;
  /* Basic match: unit-id must match current request. */
  const esy_proto_modbus_payload_t *pl =
      &ctx->payloads[(uint16_t)ctx->current_index];
  if (view.unit_id != pl->unit_id) {
    /* Not for us: ignore. */
    return;
  }

  /* Match function base (allow exception response func|0x80). */
  const esy_proto_modbus_func_t rfc = func_base(view.func);
  if (rfc != pl->func_code) {
    /* No concurrency is supported; mismatch is treated as bad frame. */
    master_finish_current(master, ESY_PROTO_MODBUS_REQ_STATUS_BAD_FRAME, NULL);
    return;
  }

  const esy_proto_modbus_req_status_t status =
      ((view.func & MODBUS_EXCEPTION_FUNC_MASK) != 0u)
          ? ESY_PROTO_MODBUS_REQ_STATUS_EXCEPTION
          : ESY_PROTO_MODBUS_REQ_STATUS_OK;

  master_finish_current(master, status, &view);

  /* Start next immediately after completing current. */
  master_try_start_next(master);
}

void esy_proto_modbus_master_tick(esy_proto_modbus_master_t *master) {
  if (!master) return;

  esy_proto_modbus_master_ctx_t *ctx = master_ctx(master);
  if (!ctx->initialized) return;

  if (ctx->tx_pending) return;

  if (ctx->state == ESY_PROTO_MODBUS_MASTER_STATE_IDLE) {
    master_try_start_next(master);
    return;
  }

  if (ctx->state == ESY_PROTO_MODBUS_MASTER_STATE_WAIT_RESP) {
    const uint32_t t = esy_hal_system_get_tick();
    if (esy_seq32_reached(t, ctx->current_deadline_ms)) {
      /* Do not rewrite a buffer still borrowed by the physical transmitter. */
      if (ctx->cfg.is_tx_busy && ctx->cfg.is_tx_busy(ctx->cfg.tx_user_ctx))
        return;
      /* Timeout */
      if (ctx->current_retry_count < ctx->current_retry_max) {
        ctx->current_retry_count++;
        if (master_build_and_send_current(master) != ESY_OK) {
          master_finish_current(
              master, ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, NULL);
        }
      } else {
        master_finish_current(master, ESY_PROTO_MODBUS_REQ_STATUS_TIMEOUT,
                              NULL);
        master_try_start_next(master);
      }
    }
  }
}

esy_err_t esy_proto_modbus_master_on_tx_complete(
    esy_proto_modbus_master_t *master, esy_err_t result) {
  if (!master) return ESY_ERR_INVALID_ARG;
  esy_proto_modbus_master_ctx_t *ctx = master_ctx(master);
  if (!ctx->initialized || !ctx->cfg.defer_tx_completion) return ESY_ERR_STATE;
  /* An aborted request may still own an in-flight physical transfer. */
  if (!ctx->tx_pending || ctx->current_index < 0) return ESY_OK;
  ctx->tx_pending = false;
  if (result != ESY_OK) {
    master_finish_current(master, ESY_PROTO_MODBUS_REQ_STATUS_TRANSPORT_ERROR, NULL);
  } else if (ctx->cfg.frame_type == ESY_PROTO_MODBUS_FRAME_RTU &&
             ctx->payloads[ctx->current_index].unit_id == 0u) {
    master_finish_current(master, ESY_PROTO_MODBUS_REQ_STATUS_OK, NULL);
  } else {
    ctx->current_deadline_ms = esy_hal_system_get_tick() + ctx->current_timeout_ms;
  }
  return ESY_OK;
}

esy_err_t esy_proto_modbus_master_abort_all(esy_proto_modbus_master_t *master,
                                            bool abort_current) {
  if (!master) return ESY_ERR_INVALID_ARG;

  esy_proto_modbus_master_ctx_t *ctx = master_ctx(master);
  if (!ctx->initialized) return ESY_ERR_INVALID_ARG;

  if (ctx->aborting) return ESY_ERR_BUSY;
  ctx->aborting = true;
  /* Detach the entry snapshot before invoking callbacks. Reentrant submissions
   * remain QUEUED and survive this abort; nested abort returns BUSY. */
  for (uint16_t i = 0; i < ctx->slot_count; ++i) {
    if (esy_prio_slot_queue_get_slot_state(&ctx->queue, i) == ESY_PRIO_SLOT_STATE_QUEUED)
      esy_prio_slot_queue_mark_in_progress(&ctx->queue, i);
  }
  for (uint16_t i = 0; i < ctx->slot_count; ++i) {
    if ((int16_t)i == ctx->current_index ||
        esy_prio_slot_queue_get_slot_state(&ctx->queue, i) != ESY_PRIO_SLOT_STATE_IN_PROGRESS)
      continue;
    esy_proto_modbus_payload_t *pl = &ctx->payloads[i];
    esy_proto_modbus_request_complete_cb callback = pl->on_complete;
    void *arg = pl->user_ctx;
    uint8_t unit = pl->unit_id;
    esy_proto_modbus_func_t function = pl->func_code;
    esy_prio_slot_queue_free_slot(&ctx->queue, i);
    if (callback) callback(master, ESY_PROTO_MODBUS_REQ_STATUS_ABORTED,
                           unit, function, NULL, 0u, arg);
  }
  if (abort_current && ctx->current_index >= 0)
    master_finish_current(master, ESY_PROTO_MODBUS_REQ_STATUS_ABORTED, NULL);
  ctx->aborting = false;

  return ESY_OK;
}

/* -------------------------------------------------------------------------- */
/* Slave API                                                                  */
/* -------------------------------------------------------------------------- */

esy_err_t esy_proto_modbus_slave_init(
    esy_proto_modbus_slave_t *slave, const esy_proto_modbus_slave_config_t *cfg,
    uint8_t *tx_frame_buf, uint16_t tx_frame_buf_size) {
  if (!slave || !cfg) return ESY_ERR_INVALID_ARG;
  if (slave_ctx(slave)->initialized) return ESY_ERR_STATE;
  if (cfg->frame_type != ESY_PROTO_MODBUS_FRAME_RTU &&
      cfg->frame_type != ESY_PROTO_MODBUS_FRAME_TCP)
    return ESY_ERR_INVALID_ARG;
  if (!cfg->tx_fn || !cfg->on_request) return ESY_ERR_INVALID_ARG;
  if (!cfg->resp_pdu_buf || cfg->resp_pdu_buf_size == 0)
    return ESY_ERR_INVALID_ARG;
  if (!tx_frame_buf || tx_frame_buf_size == 0) return ESY_ERR_INVALID_ARG;

  /* Clear opaque handle storage. */
  memset(slave, 0, sizeof(*slave));

  esy_proto_modbus_slave_ctx_t *ctx = slave_ctx(slave);
  memset(ctx, 0, sizeof(*ctx));

  ctx->cfg = *cfg;
  ctx->tx_frame_buf = tx_frame_buf;
  ctx->tx_frame_buf_size = tx_frame_buf_size;
  ctx->initialized = true;

  return ESY_OK;
}

esy_err_t esy_proto_modbus_slave_deinit(esy_proto_modbus_slave_t* slave) {
  if (!slave) return ESY_ERR_INVALID_ARG;
  esy_proto_modbus_slave_ctx_t* ctx = slave_ctx(slave);
  if (ctx->initialized && ctx->cfg.is_tx_busy &&
      ctx->cfg.is_tx_busy(ctx->cfg.tx_user_ctx))
    return ESY_ERR_BUSY;
  memset(slave, 0, sizeof(*slave));
  return ESY_OK;
}

bool esy_proto_modbus_slave_is_initialized(
    const esy_proto_modbus_slave_t *slave) {
  if (!slave) return false;
  const esy_proto_modbus_slave_ctx_t *ctx = slave_ctx_c(slave);
  return ctx->initialized;
}

void esy_proto_modbus_slave_on_rx_frame(esy_proto_modbus_slave_t *slave,
                                        const uint8_t *frame,
                                        uint16_t frame_len) {
  if (!slave || !frame || frame_len == 0) return;

  esy_proto_modbus_slave_ctx_t *ctx = slave_ctx(slave);
  if (!ctx->initialized) return;

  esy_proto_modbus_frame_view_t view;
  esy_err_t pr = esy_proto_modbus_parse_frame(ctx->cfg.frame_type, frame,
                                              frame_len, &view);
  if (pr != ESY_OK) return;

  /* Address match or broadcast (unit id 0). */
  const uint8_t my_id = ctx->cfg.unit_id;
  const bool is_broadcast =
      ctx->cfg.frame_type == ESY_PROTO_MODBUS_FRAME_RTU && view.unit_id == 0u;
  if (!is_broadcast && (view.unit_id != my_id)) return;

  /* A serial slave cannot accept another request while its response is
   * borrowed. */
  if (ctx->cfg.is_tx_busy && ctx->cfg.is_tx_busy(ctx->cfg.tx_user_ctx)) return;
  /* Call request handler. */
  uint16_t resp_pdu_len = 0;
  const esy_proto_modbus_func_t fc = func_base(view.func);

  esy_err_t hr = ctx->cfg.on_request(
      slave, view.unit_id, fc, view.pdu, view.pdu_len, ctx->cfg.resp_pdu_buf,
      &resp_pdu_len, ctx->cfg.resp_pdu_buf_size, ctx->cfg.user_ctx);
  if (hr != ESY_OK || resp_pdu_len > ctx->cfg.resp_pdu_buf_size) return;

  /* Broadcast: per Modbus convention, do not respond. */
  if (is_broadcast) return;

  /* Build response frame. */
  esy_proto_modbus_frame_t out;
  (void)esy_proto_modbus_frame_init(&out, ctx->tx_frame_buf,
                                    ctx->tx_frame_buf_size);

  esy_err_t br = ESY_ERR_FAIL;
  if (ctx->cfg.frame_type == ESY_PROTO_MODBUS_FRAME_RTU) {
    br = esy_proto_modbus_build_rtu_frame(&out, view.unit_id,
                                          ctx->cfg.resp_pdu_buf, resp_pdu_len);
  } else {
    /* Echo transaction id for TCP. */
    br = esy_proto_modbus_build_tcp_frame(&out, view.transaction_id,
                                          view.unit_id, ctx->cfg.resp_pdu_buf,
                                          resp_pdu_len);
  }

  if (br != ESY_OK) return;

  (void)ctx->cfg.tx_fn(out.buf, (uint16_t)out.len, ctx->cfg.tx_user_ctx);
}

uint8_t esy_proto_modbus_slave_get_unit_id(
    const esy_proto_modbus_slave_t *slave) {
  if (!slave) return 0u;
  const esy_proto_modbus_slave_ctx_t *ctx = slave_ctx_c(slave);
  if (!ctx->initialized) return 0u;
  return ctx->cfg.unit_id;
}
