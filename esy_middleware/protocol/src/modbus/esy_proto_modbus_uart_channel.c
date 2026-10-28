/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_proto_modbus_uart_channel_internal.h"

#include <string.h>

#include "esy_hal/esy_hal_system.h"       /* esy_hal_system_get_tick() */
#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler.h"

static bool time_expired(uint32_t now_ms, uint32_t deadline_ms) {
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

/* -------------------------------------------------------------------------- */
/* UART RX poll + RTU idle-gap framing                                         */
/* -------------------------------------------------------------------------- */

/* ISR publishes only an error latch. The owner polls data independently of TX. */
static void esy_proto_modbus_uart_rx_event(esy_uart_t *uart,
    esy_uart_rx_stream_event_t event, void *arg) {
  (void)uart;
  esy_proto_modbus_uart_rx_t *rx = arg;
  if (event != ESY_UART_RX_STREAM_EVENT_DATA)
    atomic_store_explicit(&rx->error, true, memory_order_release);
}

static esy_err_t esy_proto_modbus_uart_rx_start(esy_proto_modbus_uart_rx_t *rx) {
  const uint8_t *existing_span = NULL;
  size_t existing_length = 0u;
  esy_err_t err = esy_hal_uart_rx_stream_peek(rx->uart,
      &existing_span, &existing_length);
  if (err != ESY_ERR_STATE) return err == ESY_OK ? ESY_ERR_STATE : err;
  rx->active = true;
  err = esy_hal_uart_rx_stream_start(rx->uart, rx->stream_storage,
      sizeof(rx->stream_storage), esy_proto_modbus_uart_rx_event, rx);
  if (err == ESY_ERR_INVALID_ARG || err == ESY_ERR_NOT_SUPPORTED ||
      err == ESY_ERR_NOT_IMPLEMENTED) rx->active = false;
  return err;
}

static esy_err_t esy_proto_modbus_uart_rx_stop(esy_proto_modbus_uart_rx_t *rx) {
  if (!rx->active) return ESY_OK;
  esy_err_t err = esy_hal_uart_rx_stream_stop(rx->uart);
  if (err == ESY_OK) rx->active = false;
  return err;
}

static esy_err_t esy_proto_modbus_uart_rx_poll(esy_proto_modbus_uart_rx_t *rx, uint32_t gap,
    bool dispatch, esy_proto_modbus_uart_frame_handler_t on_frame, void *ctx) {
  uint32_t now = (uint32_t)esy_hal_system_get_tick();
  if (atomic_exchange_explicit(&rx->error, false, memory_order_acquire)) {
    /* Never parse a prefix or a tail after loss. Retry failed stop/start while
     * retaining storage; discard until an observed idle gap after recovery. */
    rx->length = 0u;
    rx->discarding = true;
    rx->last_tick = now;
    esy_err_t err = esy_proto_modbus_uart_rx_stop(rx);
    if (err == ESY_OK) err = esy_proto_modbus_uart_rx_start(rx);
    if (err != ESY_OK) atomic_store_explicit(&rx->error, true, memory_order_release);
    return err == ESY_OK ? ESY_ERR_IO : err;
  }
  size_t budget = ESY_PROTO_MODBUS_UART_RX_BUF_SIZE;
  while (budget) {
    const uint8_t *data = NULL;
    size_t length = 0u;
    esy_err_t err = esy_hal_uart_rx_stream_peek(rx->uart, &data, &length);
    if (err != ESY_OK) {
      atomic_store_explicit(&rx->error, true, memory_order_release);
      return err;
    }
    if (!length) break;
    if (length > budget) length = budget;
    if (!rx->discarding && length <= sizeof(rx->frame) - rx->length) {
      memcpy(rx->frame + rx->length, data, length);
      rx->length += (uint16_t)length;
    } else {
      rx->discarding = true;
      rx->length = 0u;
    }
    err = esy_hal_uart_rx_stream_consume(rx->uart, length);
    if (err != ESY_OK) {
      atomic_store_explicit(&rx->error, true, memory_order_release);
      return err;
    }
    budget -= length;
    rx->last_tick = now;
  }
  /* A concurrent error invalidates the whole assembled frame. */
  if (atomic_load_explicit(&rx->error, memory_order_acquire)) return ESY_ERR_IO;
  if (!gap) gap = 10u;
  if ((rx->length || rx->discarding) && time_expired(now, rx->last_tick + gap)) {
    if (rx->discarding) {
      rx->discarding = false;
      rx->length = 0u;
    } else if (dispatch) {
      on_frame(ctx, rx->frame, rx->length);
      rx->length = 0u;
    }
  }
  return ESY_OK;
}

/* =============================================================================
 * Master channel
 * =============================================================================
 */

_Static_assert(sizeof(esy_proto_modbus_uart_master_channel_t) <=
                   ESY_PROTO_MODBUS_UART_MASTER_CHANNEL_STORAGE_BYTES,
               "ESY_PROTO_MODBUS_UART_MASTER_CHANNEL_STORAGE_BYTES is too small");

static void esy_proto_modbus_uart_master_tx_done(const uint8_t *data, size_t length,
    esy_uart_tx_scheduler_tx_status_t status, esy_err_t error, void *arg) {
  (void)data;
  (void)length;
  esy_proto_modbus_uart_master_channel_t *gw = arg;
  uint32_t delay = gw->rtu_frame_timeout_ms ? gw->rtu_frame_timeout_ms : 10u;
  if (gw->last_tx_broadcast && gw->broadcast_turnaround_ms > delay)
    delay = gw->broadcast_turnaround_ms;
  gw->tx_not_before = (uint32_t)esy_hal_system_get_tick() + delay + 1u;
  gw->tx_gap_pending = true;
  (void)esy_proto_modbus_master_on_tx_complete(&gw->master,
      status == ESY_UART_TX_SCHEDULER_TX_OK ? ESY_OK :
      (error != ESY_OK ? error : ESY_ERR_IO));
}

static bool esy_proto_modbus_uart_master_is_tx_busy(void *arg) {
  esy_proto_modbus_uart_master_channel_t *gw = arg;
  if (!gw || esy_uart_tx_scheduler_is_busy(&gw->tx)) return true;
  if (gw->tx_gap_pending && !time_expired((uint32_t)esy_hal_system_get_tick(), gw->tx_not_before)) return true;
  gw->tx_gap_pending = false;
  return false;
}

static esy_err_t esy_proto_modbus_uart_master_tx_frame_fn(
    const uint8_t *frame, uint16_t length, void *arg) {
  esy_proto_modbus_uart_master_channel_t *gw = arg;
  if (!gw || !gw->initialized ||
      gw->stopping) return ESY_ERR_STATE;
  esy_uart_tx_scheduler_tx_request_t request = {
      .data = frame, .length = length,
      .on_complete = esy_proto_modbus_uart_master_tx_done, .user_data = gw};
  gw->last_tx_broadcast = frame[0] == 0u;
  return esy_uart_tx_scheduler_submit(&gw->tx, &request);
}

static void esy_proto_modbus_uart_master_req_complete_trampoline(
    esy_proto_modbus_master_t     *master,
    esy_proto_modbus_req_status_t  status,
    uint8_t                        unit_id,
    esy_proto_modbus_func_t        func_code,
    const uint8_t                 *resp_pdu,
    uint16_t                       resp_pdu_len,
    void                          *user_ctx) {
  (void)master;

  esy_proto_modbus_uart_master_channel_t *gw = (esy_proto_modbus_uart_master_channel_t*)user_ctx;
  if (!gw || !gw->initialized) return;

  if (gw->on_complete) {
    gw->on_complete(gw, status, unit_id, func_code, resp_pdu, resp_pdu_len, gw->user_ctx);
  }
}

static esy_err_t esy_proto_modbus_uart_master_submit_owned(
    esy_proto_modbus_uart_master_channel_t *gw, const esy_proto_modbus_request_t *req) {
  bool processing = gw->processing;
  gw->processing = true;
  gw->submit_request = *req;
  gw->submit_request.on_complete = esy_proto_modbus_uart_master_req_complete_trampoline;
  gw->submit_request.user_ctx = gw;
  esy_err_t err = esy_proto_modbus_master_submit(&gw->master, &gw->submit_request);
  gw->processing = processing;
  return err;
}

static void esy_proto_modbus_uart_master_on_frame(void *ctx, const uint8_t *frame, uint16_t length) {
  esy_proto_modbus_uart_master_channel_t *gw = (esy_proto_modbus_uart_master_channel_t*)ctx;
  if (!gw || !gw->initialized || !frame ||
      length == 0u) {
    return;
  }

  esy_proto_modbus_master_on_rx_frame(&gw->master, frame, length);
}

static esy_err_t esy_proto_modbus_uart_master_cleanup(esy_proto_modbus_uart_master_channel_t *gw) {
  esy_err_t err = esy_proto_modbus_uart_rx_stop(&gw->rx);
  if (err != ESY_OK) return err;
  err = esy_uart_tx_scheduler_deinit(&gw->tx);
  if (err != ESY_OK) return err;
  gw->tx_gap_pending = false;
  err = esy_proto_modbus_master_deinit(&gw->master);
  if (err == ESY_OK) gw->initialized = false;
  return err;
}

esy_proto_modbus_uart_master_channel_config_t esy_proto_modbus_uart_master_channel_default_cfg(void) {
  esy_proto_modbus_uart_master_channel_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  cfg.frame_type           = ESY_PROTO_MODBUS_FRAME_RTU;
  cfg.uart                 = NULL;

  cfg.timeout_ms           = 100u;
  cfg.retry_max            = 0u;

  cfg.rtu_frame_timeout_ms = 10u;
  cfg.broadcast_turnaround_ms = 100u;
  cfg.queue_depth          = 0u;
  cfg.static_resources.slot_storage = NULL;
  cfg.static_resources.slot_storage_count = 0u;
  cfg.static_resources.payload_storage = NULL;
  cfg.static_resources.payload_storage_count = 0u;

  cfg.on_complete          = NULL;
  cfg.user_ctx             = NULL;

  return cfg;
}

static esy_err_t esy_proto_modbus_uart_master_channel_init_storage(
    esy_proto_modbus_uart_master_channel_t *gw,
    const esy_proto_modbus_uart_master_channel_config_t *cfg) {
  if (!gw || !cfg) return ESY_ERR_INVALID_ARG;
  if (!cfg->uart || cfg->queue_depth == 0u || cfg->queue_depth > INT16_MAX ||
      cfg->timeout_ms > INT32_MAX ||
      cfg->rtu_frame_timeout_ms >= INT32_MAX ||
      cfg->broadcast_turnaround_ms >= INT32_MAX ||
      cfg->frame_type != ESY_PROTO_MODBUS_FRAME_RTU) return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_is_busy(cfg->uart, true) ||
      esy_hal_uart_is_busy(cfg->uart, false)) return ESY_ERR_BUSY;
  if (!cfg->static_resources.slot_storage ||
      (uintptr_t)cfg->static_resources.slot_storage % alignof(esy_prio_slot_t) ||
      cfg->static_resources.slot_storage_count < cfg->queue_depth) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!cfg->static_resources.payload_storage ||
      (uintptr_t)cfg->static_resources.payload_storage % alignof(esy_proto_modbus_payload_t) ||
      cfg->static_resources.payload_storage_count < cfg->queue_depth) {
    return ESY_ERR_INVALID_ARG;
  }
  gw->uart = cfg->uart;
  gw->ready = false;
  gw->initialized = false;
  gw->stopping = false;
  gw->rx.uart = cfg->uart;
  atomic_init(&gw->rx.error, false);
  gw->rtu_frame_timeout_ms = cfg->rtu_frame_timeout_ms;
  gw->broadcast_turnaround_ms = cfg->broadcast_turnaround_ms ? cfg->broadcast_turnaround_ms : 100u;
  gw->on_complete = cfg->on_complete;
  gw->user_ctx = cfg->user_ctx;
  gw->queue_depth = cfg->queue_depth;

  gw->slots = cfg->static_resources.slot_storage;
  gw->payloads = cfg->static_resources.payload_storage;

  memset(gw->slots, 0, (size_t)gw->queue_depth * sizeof(esy_prio_slot_t));
  memset(gw->payloads, 0,
         (size_t)gw->queue_depth * sizeof(esy_proto_modbus_payload_t));

  esy_proto_modbus_master_config_t mcfg = esy_proto_modbus_master_default_cfg();
  mcfg.frame_type = cfg->frame_type;
  mcfg.defer_tx_completion = true;

  mcfg.is_tx_busy = esy_proto_modbus_uart_master_is_tx_busy;
  mcfg.tx_fn = esy_proto_modbus_uart_master_tx_frame_fn;
  mcfg.tx_user_ctx = gw;

  mcfg.timeout_ms = cfg->timeout_ms;
  mcfg.retry_max  = cfg->retry_max;

  mcfg.tx_frame_buf = gw->tx_frame_buf;
  mcfg.tx_frame_buf_size = (uint16_t)sizeof(gw->tx_frame_buf);

  mcfg.slots = gw->slots;
  mcfg.payloads = gw->payloads;
  mcfg.slot_count = gw->queue_depth;

  esy_err_t err = esy_proto_modbus_master_init(&gw->master, &mcfg);
  if (err != ESY_OK) {
    gw->payloads = NULL;
    gw->slots = NULL;
    return err;
  }

  esy_uart_tx_scheduler_static_resources_t tx_resources = {gw->tx_requests, 2u};
  err = esy_uart_tx_scheduler_init(&gw->tx, gw->uart, &tx_resources);
  if (err != ESY_OK) return err;
  err = esy_proto_modbus_uart_rx_start(&gw->rx);
  if (err != ESY_OK) return err;

  gw->initialized = true;

  return ESY_OK;
}

esy_err_t esy_proto_modbus_uart_master_channel_create(
    const esy_proto_modbus_uart_master_channel_config_t *cfg,
    esy_proto_modbus_uart_master_channel_storage_t      *storage,
    esy_proto_modbus_uart_master_channel_t             **out_gw) {
  if (!cfg || !storage || !out_gw || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  *out_gw = NULL;

  esy_proto_modbus_uart_master_channel_t *gw =
      (esy_proto_modbus_uart_master_channel_t *)storage->bytes;
  if (storage->bytes[0] != 0u) return ESY_ERR_STATE;
  memset(gw, 0, sizeof(*gw));
  gw->allocated = 1u;
  gw->ready = false;
  gw->initialized = false;
  gw->stopping = false;

  esy_err_t err = esy_proto_modbus_uart_master_channel_init_storage(gw, cfg);
  if (err != ESY_OK) {
    if (esy_proto_modbus_uart_master_cleanup(gw) != ESY_OK) {
      gw->stopping = true;
      *out_gw = gw;
    } else {
      gw->allocated = 0u;
    }
    return err;
  }

  gw->ready = true;
  *out_gw = gw;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_uart_master_channel_destroy(esy_proto_modbus_uart_master_channel_t *gw) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!gw || !gw->allocated) return ESY_OK;
  if (gw->processing) return ESY_ERR_STATE;
  gw->stopping = true;
  gw->processing = true;
  (void)esy_uart_tx_scheduler_process(&gw->tx);
  esy_err_t err = esy_proto_modbus_uart_master_cleanup(gw);
  gw->processing = false;
  if (err != ESY_OK) return err;
  gw->allocated = 0u;
  return ESY_OK;
}

bool esy_proto_modbus_uart_master_channel_is_initialized(const esy_proto_modbus_uart_master_channel_t *gw) {
  return gw && gw->initialized;
}

esy_err_t esy_proto_modbus_uart_master_channel_submit(
    esy_proto_modbus_uart_master_channel_t *gw, const esy_proto_modbus_request_t *req) {
  if (!gw || !req || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!gw->initialized ||
      gw->stopping) return ESY_ERR_STATE;
  if (req->on_complete || req->user_ctx) return ESY_ERR_INVALID_ARG;
  return esy_proto_modbus_uart_master_submit_owned(gw, req);
}

uint16_t esy_proto_modbus_uart_master_channel_get_queue_depth(const esy_proto_modbus_uart_master_channel_t *gw) {
  if (!gw || !gw->initialized) return 0u;
  return esy_proto_modbus_master_get_queue_depth(&gw->master);
}

esy_proto_modbus_master_state_t esy_proto_modbus_uart_master_channel_get_state(
    const esy_proto_modbus_uart_master_channel_t *gw) {
  if (!gw || !gw->initialized)
    return ESY_PROTO_MODBUS_MASTER_STATE_IDLE;
  return esy_proto_modbus_master_get_state(&gw->master);
}

static esy_err_t esy_proto_modbus_uart_master_abort_owned(
    esy_proto_modbus_uart_master_channel_t *gw, bool current) {
  bool processing = gw->processing;
  gw->processing = true;
  esy_err_t err = esy_proto_modbus_master_abort_all(&gw->master, current);
  gw->processing = processing;
  return err;
}

esy_err_t esy_proto_modbus_uart_master_channel_abort_all(
    esy_proto_modbus_uart_master_channel_t *gw, bool abort_current) {
  if (!gw || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!gw->initialized ||
      gw->stopping) return ESY_ERR_STATE;
  return esy_proto_modbus_uart_master_abort_owned(gw, abort_current);
}

/* =============================================================================
 * Slave channel
 * =============================================================================
 */

_Static_assert(sizeof(esy_proto_modbus_uart_slave_channel_t) <=
                   ESY_PROTO_MODBUS_UART_SLAVE_CHANNEL_STORAGE_BYTES,
               "ESY_PROTO_MODBUS_UART_SLAVE_CHANNEL_STORAGE_BYTES is too small");

static void esy_proto_modbus_uart_slave_tx_done(const uint8_t *data, size_t length,
    esy_uart_tx_scheduler_tx_status_t status, esy_err_t error, void *arg) {
  (void)data;
  (void)length;
  (void)status;
  (void)error;
  (void)arg;
}

static bool esy_proto_modbus_uart_slave_is_tx_busy(void *arg) {
  esy_proto_modbus_uart_slave_channel_t *gw = arg;
  return !gw || esy_uart_tx_scheduler_is_busy(&gw->tx);
}

static esy_err_t esy_proto_modbus_uart_slave_tx_frame_fn(
    const uint8_t *frame, uint16_t length, void *arg) {
  esy_proto_modbus_uart_slave_channel_t *gw = arg;
  if (!gw || !gw->initialized ||
      gw->stopping) return ESY_ERR_STATE;
  esy_uart_tx_scheduler_tx_request_t request = {
      .data = frame, .length = length,
      .on_complete = esy_proto_modbus_uart_slave_tx_done, .user_data = gw};
  return esy_uart_tx_scheduler_submit(&gw->tx, &request);
}

static void esy_proto_modbus_uart_slave_on_frame(void *ctx, const uint8_t *frame, uint16_t length) {
  esy_proto_modbus_uart_slave_channel_t *gw = (esy_proto_modbus_uart_slave_channel_t*)ctx;
  if (!gw || !gw->initialized || !frame ||
      length == 0u) {
    return;
  }

  esy_proto_modbus_slave_on_rx_frame(&gw->slave, frame, length);
}

static esy_err_t esy_proto_modbus_uart_slave_cleanup(esy_proto_modbus_uart_slave_channel_t *gw) {
  esy_err_t err = esy_proto_modbus_uart_rx_stop(&gw->rx);
  if (err != ESY_OK) return err;
  err = esy_uart_tx_scheduler_deinit(&gw->tx);
  if (err != ESY_OK) return err;
  err = esy_proto_modbus_slave_deinit(&gw->slave);
  if (err == ESY_OK) gw->initialized = false;
  return err;
}

esy_proto_modbus_uart_slave_channel_config_t esy_proto_modbus_uart_slave_channel_default_cfg(void) {
  esy_proto_modbus_uart_slave_channel_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  cfg.frame_type           = ESY_PROTO_MODBUS_FRAME_RTU;
  cfg.uart                 = NULL;
  cfg.unit_id              = 1u;

  cfg.rtu_frame_timeout_ms = 10u;

  cfg.on_request           = NULL;
  cfg.user_ctx             = NULL;

  return cfg;
}

static esy_err_t esy_proto_modbus_uart_slave_request(
    esy_proto_modbus_slave_t *slave, uint8_t unit, esy_proto_modbus_func_t function,
    const uint8_t *request, uint16_t request_length, uint8_t *response,
    uint16_t *response_length, uint16_t response_capacity, void *arg) {
  (void)slave;
  esy_proto_modbus_uart_slave_channel_t *gw = arg;
  return gw->on_request(gw, unit, function, request, request_length, response,
      response_length, response_capacity, gw->user_ctx);
}

static esy_err_t esy_proto_modbus_uart_slave_channel_init_storage(
    esy_proto_modbus_uart_slave_channel_t *gw,
    const esy_proto_modbus_uart_slave_channel_config_t *cfg) {
  if (!gw || !cfg) return ESY_ERR_INVALID_ARG;
  if (!cfg->uart || !cfg->on_request ||
      cfg->rtu_frame_timeout_ms > INT32_MAX ||
      cfg->frame_type != ESY_PROTO_MODBUS_FRAME_RTU) return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_is_busy(cfg->uart, true) ||
      esy_hal_uart_is_busy(cfg->uart, false)) return ESY_ERR_BUSY;
  gw->uart = cfg->uart;
  gw->ready = false;
  gw->initialized = false;
  gw->stopping = false;
  gw->rx.uart = cfg->uart;
  atomic_init(&gw->rx.error, false);
  gw->rtu_frame_timeout_ms = cfg->rtu_frame_timeout_ms;

  esy_proto_modbus_slave_config_t scfg;
  memset(&scfg, 0, sizeof(scfg));
  scfg.frame_type  = cfg->frame_type;
  scfg.unit_id     = cfg->unit_id;
  gw->unit_id = cfg->unit_id;

  scfg.is_tx_busy = esy_proto_modbus_uart_slave_is_tx_busy;
  scfg.tx_fn       = esy_proto_modbus_uart_slave_tx_frame_fn;
  scfg.tx_user_ctx = gw;

  scfg.resp_pdu_buf = gw->resp_pdu_buf;
  scfg.resp_pdu_buf_size = (uint16_t)sizeof(gw->resp_pdu_buf);

  gw->on_request = cfg->on_request;
  gw->user_ctx = cfg->user_ctx;
  scfg.on_request = esy_proto_modbus_uart_slave_request;
  scfg.user_ctx = gw;

  esy_err_t err = esy_proto_modbus_slave_init(
      &gw->slave,
      &scfg,
      gw->tx_frame_buf,
      (uint16_t)sizeof(gw->tx_frame_buf));
  if (err != ESY_OK) {
    return err;
  }

  esy_uart_tx_scheduler_static_resources_t tx_resources = {gw->tx_requests, 2u};
  err = esy_uart_tx_scheduler_init(&gw->tx, gw->uart, &tx_resources);
  if (err != ESY_OK) return err;
  err = esy_proto_modbus_uart_rx_start(&gw->rx);
  if (err != ESY_OK) return err;

  gw->initialized = true;

  return ESY_OK;
}

esy_err_t esy_proto_modbus_uart_slave_channel_create(
    const esy_proto_modbus_uart_slave_channel_config_t *cfg,
    esy_proto_modbus_uart_slave_channel_storage_t      *storage,
    esy_proto_modbus_uart_slave_channel_t             **out_gw) {
  if (!cfg || !storage || !out_gw || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  *out_gw = NULL;

  esy_proto_modbus_uart_slave_channel_t *gw =
      (esy_proto_modbus_uart_slave_channel_t *)storage->bytes;
  if (storage->bytes[0] != 0u) return ESY_ERR_STATE;
  memset(gw, 0, sizeof(*gw));
  gw->allocated = 1u;
  gw->ready = false;
  gw->initialized = false;
  gw->stopping = false;
  esy_err_t err = esy_proto_modbus_uart_slave_channel_init_storage(gw, cfg);
  if (err != ESY_OK) {
    if (esy_proto_modbus_uart_slave_cleanup(gw) != ESY_OK) {
      gw->stopping = true;
      *out_gw = gw;
    } else {
      gw->allocated = 0u;
    }
    return err;
  }

  gw->ready = true;
  *out_gw = gw;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_uart_slave_channel_destroy(esy_proto_modbus_uart_slave_channel_t *gw) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!gw || !gw->allocated) return ESY_OK;
  if (gw->processing) return ESY_ERR_STATE;
  gw->stopping = true;
  gw->processing = true;
  (void)esy_uart_tx_scheduler_process(&gw->tx);
  esy_err_t err = esy_proto_modbus_uart_slave_cleanup(gw);
  gw->processing = false;
  if (err != ESY_OK) return err;
  gw->allocated = 0u;
  return ESY_OK;
}

bool esy_proto_modbus_uart_slave_channel_is_initialized(const esy_proto_modbus_uart_slave_channel_t *gw) {
  return gw && gw->initialized;
}

uint8_t esy_proto_modbus_uart_slave_channel_get_addr(const esy_proto_modbus_uart_slave_channel_t *gw) {
  if (!gw || !gw->initialized) return 0u;
  return gw->unit_id;
}

static esy_err_t esy_proto_modbus_uart_master_process_context(esy_proto_modbus_uart_master_channel_t *gw) {
  if (!gw || !gw->allocated || !gw->ready ||
      gw->processing) return ESY_ERR_STATE;
  gw->processing = true;
  (void)esy_uart_tx_scheduler_process(&gw->tx);
  esy_err_t err;
  if (gw->stopping) {
    err = esy_proto_modbus_uart_master_cleanup(gw);
  } else {
    err = esy_proto_modbus_uart_rx_poll(&gw->rx, gw->rtu_frame_timeout_ms,
        !esy_uart_tx_scheduler_is_busy(&gw->tx), esy_proto_modbus_uart_master_on_frame, gw);
    esy_proto_modbus_master_tick(&gw->master);
    (void)esy_uart_tx_scheduler_process(&gw->tx);
    }
  gw->processing = false;
  return err;
}

esy_err_t esy_proto_modbus_uart_master_channel_poll(esy_proto_modbus_uart_master_channel_t *gw) {
  if (!gw || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  return esy_proto_modbus_uart_master_process_context(gw);
}

static esy_err_t esy_proto_modbus_uart_slave_process_context(esy_proto_modbus_uart_slave_channel_t *gw) {
  if (!gw || !gw->allocated || !gw->ready ||
      gw->processing) return ESY_ERR_STATE;
  gw->processing = true;
  (void)esy_uart_tx_scheduler_process(&gw->tx);
  esy_err_t err;
  if (gw->stopping) {
    err = esy_proto_modbus_uart_slave_cleanup(gw);
  } else {
    err = esy_proto_modbus_uart_rx_poll(&gw->rx, gw->rtu_frame_timeout_ms,
        !esy_uart_tx_scheduler_is_busy(&gw->tx), esy_proto_modbus_uart_slave_on_frame, gw);
    (void)esy_uart_tx_scheduler_process(&gw->tx);
  }
  gw->processing = false;
  return err;
}

esy_err_t esy_proto_modbus_uart_slave_channel_poll(esy_proto_modbus_uart_slave_channel_t *gw) {
  if (!gw || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  return esy_proto_modbus_uart_slave_process_context(gw);
}
