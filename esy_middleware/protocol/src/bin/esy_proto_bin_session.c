/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_bin_session.h"

#include <string.h>
#include <limits.h>
#include "esy_common/ds/esy_block_ring.h"

#include "esy_hal/esy_hal_system.h" /* esy_hal_system_get_tick() */
#include "esy_common/utils/base/esy_seq.h"

/* ------------------------------ small utils ------------------------------ */

static inline bool is_pow2_u32(uint32_t v) {
  return (v != 0u) && ((v & (v - 1u)) == 0u);
}

static inline uint32_t u32_min(uint32_t a, uint32_t b) {
  return (a < b) ? a : b;
}

/* TX ring block layout: [len:u32][transaction_id:u32][payload]. */
static uint32_t tx_block_size(uint32_t tx_payload_max) {
  return (uint32_t)ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES + tx_payload_max;
}

typedef enum {
  ESY_PROTO_BIN_TX_PHASE_TRANSMITTING = 0,
  ESY_PROTO_BIN_TX_PHASE_WAIT_ACK,
  ESY_PROTO_BIN_TX_PHASE_RETRY_PENDING,
} esy_proto_bin_tx_phase_t;

typedef enum {
  ESY_PROTO_BIN_BORROW_NONE = 0,
  ESY_PROTO_BIN_BORROW_DATA,
  ESY_PROTO_BIN_BORROW_CONTROL,
} esy_proto_bin_tx_borrow_kind_t;

typedef struct {
  esy_proto_bin_session_config_t cfg;
  esy_proto_bin_session_static_resources_t resources;
  bool polling;
  bool tx_borrowed;
  esy_proto_bin_tx_borrow_kind_t tx_borrow_kind;
  uint32_t tx_borrow_transaction_id;
  uint8_t tx_borrow_seq;

  esy_block_ring_t tx_ring;

  /* RX incremental parser */
  esy_proto_bin_parser_t parser;
  esy_proto_bin_frame_t rx_frame;

  /* TX build frame */
  esy_proto_bin_frame_t tx_frame;
  uint8_t control_tx_buf[ESY_PROTO_BIN_OVERHEAD_BYTES];

  /* stop-and-wait state */
  struct {
    bool active;
    esy_proto_bin_tx_phase_t phase;
    uint8_t seq; /* current seq */
    uint32_t transaction_id;
    uint32_t retries_left;
    uint32_t deadline_ms; /* absolute tick (ms) */
    bool retry_requested; /* NACK received before physical TX retirement. */
  } in_flight;

  uint8_t next_seq;
  uint32_t next_transaction_id;
  uint8_t last_received_seq;
  bool received_seq_valid;
  bool initialized;
  bool resetting;
  bool control_pending;
  uint8_t control_seq;
  bool control_retry_pending;
  uint8_t control_retry_seq;

  /* enforce get_tx_buffer/commit_tx pairing */
  uint8_t *reserved_wblk; /* points to ring write block header, NULL if none */
} esy_proto_bin_session_ctx_t;

/* ------------------------------ notifications ------------------------------
 */

static void notify_event(esy_proto_bin_session_ctx_t *c,
                         esy_proto_bin_tp_event_t ev, uint32_t transaction_id) {
  if (c->cfg.event_cb) {
    c->cfg.event_cb(ev, transaction_id, c->cfg.user_data);
  }
}

static bool tx_busy(esy_proto_bin_session_ctx_t *c) {
  const bool busy = c->cfg.io.is_tx_busy ? c->cfg.io.is_tx_busy(c->cfg.user_data) : false;
  if (!busy) {
    c->tx_borrowed = false;
    c->tx_borrow_kind = ESY_PROTO_BIN_BORROW_NONE;
  }
  return busy;
}

static bool rx_ready(esy_proto_bin_session_ctx_t *c) {
  return c->cfg.io.is_rx_ready(c->cfg.user_data);
}

static esy_err_t link_send(esy_proto_bin_session_ctx_t *c,
                           const uint8_t *data, uint32_t len,
                           esy_proto_bin_tx_borrow_kind_t kind, uint8_t seq) {
  const esy_err_t result = c->cfg.io.send(c->cfg.user_data, data, len);
  if (result == ESY_OK) {
    c->tx_borrowed = true;
    c->tx_borrow_kind = kind;
    c->tx_borrow_transaction_id = kind == ESY_PROTO_BIN_BORROW_DATA
        ? c->in_flight.transaction_id : ESY_PROTO_BIN_INVALID_TRANSACTION_ID;
    c->tx_borrow_seq = seq;
  }
  return result;
}

static bool link_recv_byte(esy_proto_bin_session_ctx_t *c, uint8_t *out) {
  return c->cfg.io.receive(c->cfg.user_data, out);
}

/* ------------------------------ frame helpers ------------------------------
 */

static bool build_and_send_ctrl(esy_proto_bin_session_ctx_t *c, uint8_t cmd,
                                uint8_t seq) {
  if (tx_busy(c)) return false;
  /* The preceding DATA has completed before we borrow the link for ACK.
   * Otherwise this control send could be mistaken for unfinished DATA TX. */
  if (c->in_flight.active &&
      c->in_flight.phase == ESY_PROTO_BIN_TX_PHASE_TRANSMITTING) {
    c->in_flight.phase = ESY_PROTO_BIN_TX_PHASE_WAIT_ACK;
    c->in_flight.deadline_ms = esy_hal_system_get_tick() +
        (c->in_flight.retry_requested ? 0u : c->cfg.acknowledgement_timeout_ms);
  }

  esy_proto_bin_frame_t f;
  if (esy_proto_bin_frame_init(&f, c->control_tx_buf,
                               sizeof(c->control_tx_buf)) != ESY_OK) {
    return false;
  }
  if (esy_proto_bin_frame_put_header(&f, cmd, seq) != ESY_OK) return false;
  if (esy_proto_bin_frame_finish(&f) != ESY_OK) return false;

  return link_send(c, f.buf, (uint32_t)f.len,
      ESY_PROTO_BIN_BORROW_CONTROL, seq) == ESY_OK;
}

static esy_err_t build_and_send_data(esy_proto_bin_session_ctx_t *c, uint8_t seq,
                                     const uint8_t *payload, uint32_t payload_len) {
  if (!c->resources.tx_work_buf || c->resources.tx_work_buf_size == 0u)
    return ESY_ERR_INVALID_ARG;

  if (payload_len > c->cfg.tx_payload_max) payload_len = c->cfg.tx_payload_max;

  if (esy_proto_bin_frame_init(&c->tx_frame, c->resources.tx_work_buf,
                               c->resources.tx_work_buf_size) != ESY_OK) {
    return ESY_ERR_INVALID_ARG;
  }

  if (esy_proto_bin_frame_put_header(&c->tx_frame, ESY_PROTO_BIN_CMD_DATA,
                                     seq) != ESY_OK) {
    return ESY_ERR_INVALID_ARG;
  }
  if (payload_len > 0u) {
    if (esy_proto_bin_frame_put_data(&c->tx_frame, payload, payload_len) !=
        ESY_OK) {
      return ESY_ERR_INVALID_ARG;
    }
  }
  if (esy_proto_bin_frame_finish(&c->tx_frame) != ESY_OK) {
    return ESY_ERR_INVALID_ARG;
  }

  return link_send(c, c->tx_frame.buf, (uint32_t)c->tx_frame.len,
      ESY_PROTO_BIN_BORROW_DATA, seq);
}

static esy_err_t send_in_flight_attempt(esy_proto_bin_session_ctx_t *c,
                                        const uint8_t *payload,
                                        uint32_t payload_len) {
  esy_err_t result =
      build_and_send_data(c, c->in_flight.seq, payload, payload_len);
  if (result != ESY_OK) return result;
  const bool transmitting = tx_busy(c);
  c->in_flight.retry_requested = false;
  /* Sending and callbacks may advance time before this phase actually starts. */
  const uint32_t now = esy_hal_system_get_tick();
  if (transmitting) {
    c->in_flight.phase = ESY_PROTO_BIN_TX_PHASE_TRANSMITTING;
    c->in_flight.deadline_ms = now + c->cfg.tx_completion_timeout_ms;
  } else {
    c->in_flight.phase = ESY_PROTO_BIN_TX_PHASE_WAIT_ACK;
    c->in_flight.deadline_ms = now + c->cfg.acknowledgement_timeout_ms;
  }
  return ESY_OK;
}

static void fail_in_flight(esy_proto_bin_session_ctx_t *c,
                           esy_proto_bin_tp_event_t event) {
  const uint32_t id = c->in_flight.transaction_id;
  (void)esy_block_ring_get_read_block(&c->tx_ring);
  esy_block_ring_commit_read(&c->tx_ring);
  c->in_flight.active = false;
  notify_event(c, event, id);
}

static void complete_in_flight_ack(esy_proto_bin_session_ctx_t *c) {
  fail_in_flight(c, ESY_PROTO_BIN_TP_EVENT_ACK);
}

static bool bin_is_idle(void *ctx) {
  esy_proto_bin_session_ctx_t *c = (esy_proto_bin_session_ctx_t *)ctx;
  if (!c || !c->initialized || c->resetting || c->reserved_wblk || c->in_flight.active ||
      c->control_pending || c->control_retry_pending || tx_busy(c)) {
    return false;
  }
  return esy_block_ring_get_read_block(&c->tx_ring) == NULL;
}

static uint8_t *bin_get_tx_buffer(void *ctx, uint32_t *out_cap) {
  esy_proto_bin_session_ctx_t *c = (esy_proto_bin_session_ctx_t *)ctx;
  if (out_cap) *out_cap = 0;
  if (!c || !c->initialized || c->resetting) return NULL;

  /* enforce pairing: must commit before next get */
  if (c->reserved_wblk) return NULL;

  uint8_t *wblk = esy_block_ring_get_write_block(&c->tx_ring);
  if (!wblk) return NULL;

  c->reserved_wblk = wblk;
  if (out_cap) *out_cap = c->cfg.tx_payload_max;
  return wblk + ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES;
}

static esy_err_t bin_cancel_tx(void *ctx) {
  esy_proto_bin_session_ctx_t *c = (esy_proto_bin_session_ctx_t *)ctx;
  if (c == NULL) return ESY_ERR_INVALID_ARG;
  if (c->resetting) return ESY_ERR_STATE;
  c->reserved_wblk = NULL;
  return ESY_OK;
}

static esy_err_t bin_commit_tx(void *ctx, uint32_t len,
                               uint32_t *transaction_id) {
  esy_proto_bin_session_ctx_t *c = (esy_proto_bin_session_ctx_t *)ctx;
  uint32_t id;

  if (!c || !c->initialized || c->resetting) return ESY_ERR_STATE;
  if (!c->reserved_wblk) return ESY_ERR_STATE;
  if (len > c->cfg.tx_payload_max) return ESY_ERR_INVALID_ARG;

  id = c->next_transaction_id++;
  if (c->next_transaction_id == ESY_PROTO_BIN_INVALID_TRANSACTION_ID) {
    c->next_transaction_id++;
  }
  memcpy(c->reserved_wblk, &len, sizeof(len));
  memcpy(c->reserved_wblk + sizeof(len), &id, sizeof(id));
  c->reserved_wblk = NULL;
  esy_block_ring_commit_write(&c->tx_ring);
  if (transaction_id) {
    *transaction_id = id;
  }
  return ESY_OK;
}

static void handle_rx_frame_ok(esy_proto_bin_session_ctx_t *c,
                               const esy_proto_bin_frame_t *frame) {
  const uint8_t cmd = esy_proto_bin_frame_get_cmd(frame);
  const uint8_t seq = esy_proto_bin_frame_get_seq(frame);

  if (cmd == ESY_PROTO_BIN_CMD_DATA) {
    /* ACK DATA before handing it upward. The callback may queue a response. */
    c->control_seq = seq;
    c->control_pending = !build_and_send_ctrl(c, ESY_PROTO_BIN_CMD_ACK, seq);

    /* Retransmitted DATA is acknowledged again but delivered only once. */
    if (c->received_seq_valid && seq == c->last_received_seq) {
      return;
    }
    c->last_received_seq = seq;
    c->received_seq_valid = true;

    /* Report the first delivery of the received data frame. */
    if (c->cfg.data_cb) c->cfg.data_cb(frame, c->cfg.user_data);
    return;
  }

  if (cmd == ESY_PROTO_BIN_CMD_ACK || cmd == ESY_PROTO_BIN_CMD_NACK) {
    if (!c->in_flight.active) return;

    /* Only accept ACK/NACK for the current sequence number. */
    if (seq != c->in_flight.seq) return;

    if (cmd == ESY_PROTO_BIN_CMD_ACK) {
      complete_in_flight_ack(c);
    } else {
      notify_event(c, ESY_PROTO_BIN_TP_EVENT_NACK, c->in_flight.transaction_id);

      /* Retain the physical deadline and borrowed work buffer until TX ends. */
      c->in_flight.retry_requested = true;
      if (c->in_flight.phase == ESY_PROTO_BIN_TX_PHASE_WAIT_ACK) {
        c->in_flight.deadline_ms = esy_hal_system_get_tick();
      }
    }
    return;
  }

  /* Ignore other commands for now. */
}

static void bin_poll(void *ctx) {
  esy_proto_bin_session_ctx_t *c = (esy_proto_bin_session_ctx_t *)ctx;
  if (!c || !c->initialized || c->resetting) return;

  /* 1) Bound RX work so readable input cannot starve TX and its deadlines. */
  uint32_t rx_bytes_remaining = c->cfg.rx_byte_budget;
  if (c->control_retry_pending) {
    c->control_retry_pending = !build_and_send_ctrl(c,
        ESY_PROTO_BIN_CMD_ACK, c->control_retry_seq);
  }
  if (!c->control_retry_pending && c->control_pending) {
    c->control_pending =
        !build_and_send_ctrl(c, ESY_PROTO_BIN_CMD_ACK, c->control_seq);
  }
  while (rx_bytes_remaining != 0u && !c->control_retry_pending &&
         !c->control_pending && rx_ready(c)) {
    uint8_t b = 0;
    if (!link_recv_byte(c, &b)) break;
    --rx_bytes_remaining;

    esy_proto_bin_feed_result_t r =
        esy_proto_bin_parser_feed(&c->parser, &c->rx_frame, b);

    if (r == ESY_PROTO_BIN_FEED_OK) {
      handle_rx_frame_ok(c, &c->rx_frame);

      /* Reset parser/frame after a complete frame to continue receiving. */
      esy_proto_bin_parser_reset(&c->parser, &c->rx_frame);
    } else if (r == ESY_PROTO_BIN_FEED_CRC_ERROR) {
      notify_event(c, ESY_PROTO_BIN_TP_EVENT_RX_CRC_ERROR,
                   ESY_PROTO_BIN_INVALID_TRANSACTION_ID);

      /* Drop the in-flight frame on CRC errors to keep the parser moving. */
      esy_proto_bin_parser_reset(&c->parser, &c->rx_frame);
    } else {
      /* incomplete: continue feeding */
    }
  }

  /* 2) Advance physical transmission, ACK timeout, and retry state. */
  if (c->in_flight.active) {
    if (c->in_flight.phase == ESY_PROTO_BIN_TX_PHASE_TRANSMITTING) {
      if (!tx_busy(c)) {
        c->in_flight.phase = ESY_PROTO_BIN_TX_PHASE_WAIT_ACK;
        c->in_flight.deadline_ms = esy_hal_system_get_tick() +
            (c->in_flight.retry_requested ? 0u : c->cfg.acknowledgement_timeout_ms);
      } else if (esy_seq32_reached(esy_hal_system_get_tick(),
                                  c->in_flight.deadline_ms)) {
        fail_in_flight(c, ESY_PROTO_BIN_TP_EVENT_TX_TIMEOUT);
      }
      return;
    }

    if (c->in_flight.phase == ESY_PROTO_BIN_TX_PHASE_WAIT_ACK) {
      if (!esy_seq32_reached(esy_hal_system_get_tick(),
                             c->in_flight.deadline_ms)) {
        return;
      }
      if (c->in_flight.retries_left == 0u) {
        fail_in_flight(c, ESY_PROTO_BIN_TP_EVENT_TIMEOUT);
        return;
      }
      /* The expired wait stays inactive while link backpressure defers retry,
       * including across a full clock period. Only an accepted send rearms it. */
      c->in_flight.phase = ESY_PROTO_BIN_TX_PHASE_RETRY_PENDING;
    }
    if (tx_busy(c)) {
      return;
    }

    uint8_t *rblk = esy_block_ring_get_read_block(&c->tx_ring);
    if (rblk == NULL) {
      fail_in_flight(c, ESY_PROTO_BIN_TP_EVENT_IO_ERROR);
      return;
    }
    uint32_t plen;
    memcpy(&plen, rblk, sizeof(plen));
    const uint8_t *payload = rblk + ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES;
    plen = u32_min(plen, c->cfg.tx_payload_max);
    const esy_err_t result = send_in_flight_attempt(c, payload, plen);
    if (result == ESY_ERR_BUSY) return;
    if (result != ESY_OK) {
      fail_in_flight(c, ESY_PROTO_BIN_TP_EVENT_IO_ERROR);
      return;
    }
    c->in_flight.retries_left--;
    return; /* Stop-and-wait: do not start another transaction while waiting. */
  }

  /* 3) Idle: send the next queued payload, if any. */
  if (c->control_pending || c->control_retry_pending || tx_busy(c)) return;

  uint8_t *rblk = esy_block_ring_get_read_block(&c->tx_ring);
  if (!rblk) return;

  uint32_t plen, transaction_id;
  memcpy(&plen, rblk, sizeof(plen));
  memcpy(&transaction_id, rblk + sizeof(plen), sizeof(transaction_id));
  const uint8_t *payload = rblk + ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES;
  plen = u32_min(plen, c->cfg.tx_payload_max);

  const uint8_t seq = c->next_seq;
  c->in_flight.active = true;
  c->in_flight.seq = seq;
  c->in_flight.transaction_id = transaction_id;
  c->in_flight.retries_left = c->cfg.retries;
  const esy_err_t result = send_in_flight_attempt(c, payload, plen);
  if (result == ESY_ERR_BUSY) {
    c->in_flight.active = false;
  } else if (result != ESY_OK) {
    fail_in_flight(c, ESY_PROTO_BIN_TP_EVENT_IO_ERROR);
  } else {
    ++c->next_seq;
  }
}

/* ------------------------------ Public API -------------------------------- */

static esy_proto_bin_session_ctx_t *session_context(esy_proto_bin_session_t *session) {
  return session == NULL ? NULL :
      (esy_proto_bin_session_ctx_t *)(void *)session->priv.bytes;
}

_Static_assert(sizeof(esy_proto_bin_session_ctx_t) <=
                   ESY_PROTO_BIN_SESSION_STORAGE_BYTES,
               "ProtoBin session storage is too small");
_Static_assert(_Alignof(esy_proto_bin_session_ctx_t) <=
                   _Alignof(esy_proto_bin_session_t),
               "ProtoBin session storage is misaligned");

void esy_proto_bin_session_default_cfg(esy_proto_bin_session_config_t *cfg) {
  if (cfg == NULL) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->rx_byte_budget = ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET;
}

static bool ranges_overlap(const void *a, size_t a_size,
                             const void *b, size_t b_size) {
  const uintptr_t a_begin = (uintptr_t)a;
  const uintptr_t b_begin = (uintptr_t)b;
  if (a_size > UINTPTR_MAX - a_begin || b_size > UINTPTR_MAX - b_begin)
    return true;
  return a_begin < b_begin + b_size && b_begin < a_begin + a_size;
}

esy_err_t esy_proto_bin_session_init(esy_proto_bin_session_t *session,
                                     const esy_proto_bin_session_config_t *cfg) {
  if (session == NULL || cfg == NULL) return ESY_ERR_INVALID_ARG;
  esy_proto_bin_session_ctx_t *current = session_context(session);
  if (current->initialized) return ESY_ERR_STATE;
  const esy_proto_bin_session_static_resources_t *resources = cfg->static_resources;
  if (resources == NULL || resources->tx_queue_buf == NULL ||
      resources->tx_work_buf == NULL || resources->rx_work_buf == NULL ||
      cfg->tx_queue_block_count < 2u || !is_pow2_u32(cfg->tx_queue_block_count) ||
      cfg->tx_payload_max == 0u || cfg->rx_payload_max == 0u ||
      cfg->tx_payload_max > UINT16_MAX || cfg->rx_payload_max > UINT16_MAX ||
      cfg->tx_completion_timeout_ms == 0u ||
      cfg->acknowledgement_timeout_ms == 0u ||
      cfg->tx_completion_timeout_ms > INT32_MAX ||
      cfg->acknowledgement_timeout_ms > INT32_MAX ||
      !cfg->io.send || !cfg->io.receive || !cfg->io.is_rx_ready) {
    return ESY_ERR_INVALID_ARG;
  }
  if (ranges_overlap(session, sizeof(*session), cfg, sizeof(*cfg)) ||
      ranges_overlap(session, sizeof(*session), resources, sizeof(*resources)) ||
      ranges_overlap(session, sizeof(*session), resources->tx_queue_buf,
                     resources->tx_queue_buf_size) ||
      ranges_overlap(session, sizeof(*session), resources->tx_work_buf,
                     resources->tx_work_buf_size) ||
      ranges_overlap(session, sizeof(*session), resources->rx_work_buf,
                     resources->rx_work_buf_size) ||
      ranges_overlap(resources->tx_queue_buf, resources->tx_queue_buf_size,
                     resources->tx_work_buf, resources->tx_work_buf_size) ||
      ranges_overlap(resources->tx_queue_buf, resources->tx_queue_buf_size,
                     resources->rx_work_buf, resources->rx_work_buf_size) ||
      ranges_overlap(resources->tx_work_buf, resources->tx_work_buf_size,
                     resources->rx_work_buf, resources->rx_work_buf_size)) {
    return ESY_ERR_INVALID_ARG;
  }
  const uint32_t block_size = tx_block_size(cfg->tx_payload_max);
  if (cfg->tx_queue_block_count > UINT32_MAX / block_size ||
      cfg->tx_queue_block_count > SIZE_MAX / block_size ||
      resources->tx_queue_buf_size < (size_t)block_size * cfg->tx_queue_block_count ||
      resources->tx_work_buf_size > UINT32_MAX ||
      resources->rx_work_buf_size > UINT32_MAX ||
      resources->tx_work_buf_size < ESY_PROTO_BIN_OVERHEAD_BYTES + cfg->tx_payload_max ||
      resources->rx_work_buf_size < ESY_PROTO_BIN_OVERHEAD_BYTES + cfg->rx_payload_max) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Validate into temporary state so every init failure preserves the handle. */
  esy_proto_bin_session_ctx_t next = {0};
  next.cfg = *cfg;
  next.cfg.static_resources = NULL;
  next.resources = *resources;
  if (next.cfg.rx_byte_budget == 0u)
    next.cfg.rx_byte_budget = ESY_PROTO_BIN_DEFAULT_RX_BYTE_BUDGET;
  esy_err_t result = esy_block_ring_init(&next.tx_ring, resources->tx_queue_buf,
                                         block_size, cfg->tx_queue_block_count);
  if (result != ESY_OK) return result;
  result = esy_proto_bin_parser_init(&next.parser);
  if (result != ESY_OK) return result;
  result = esy_proto_bin_frame_init(&next.rx_frame, resources->rx_work_buf,
                                    ESY_PROTO_BIN_OVERHEAD_BYTES + cfg->rx_payload_max);
  if (result != ESY_OK) return result;
  next.next_seq = 1u;
  next.next_transaction_id = 1u;
  next.initialized = true;
  memcpy(current, &next, sizeof(next));
  return ESY_OK;
}

esy_err_t esy_proto_bin_session_deinit(esy_proto_bin_session_t *session) {
  if (session == NULL) return ESY_ERR_INVALID_ARG;
  esy_proto_bin_session_ctx_t *c = session_context(session);
  if (!c->initialized) return ESY_OK;
  if (c->polling || c->resetting || (c->tx_borrowed && tx_busy(c)))
    return ESY_ERR_BUSY;
  memset(session, 0, sizeof(*session));
  return ESY_OK;
}

esy_err_t esy_proto_bin_session_on_tx_complete(esy_proto_bin_session_t *session,
                                              esy_err_t result) {
  if (session == NULL || result > ESY_OK || esy_hal_system_is_in_isr())
    return ESY_ERR_INVALID_ARG;
  esy_proto_bin_session_ctx_t *c = session_context(session);
  if (!c->initialized) return ESY_ERR_STATE;
  if (c->polling || c->resetting) return ESY_ERR_BUSY;
  if (!c->tx_borrowed) return ESY_OK;

  c->polling = true;
  if (c->cfg.io.is_tx_busy && c->cfg.io.is_tx_busy(c->cfg.user_data)) {
    c->polling = false;
    return ESY_ERR_BUSY;
  }
  const esy_proto_bin_tx_borrow_kind_t kind = c->tx_borrow_kind;
  const uint32_t transaction_id = c->tx_borrow_transaction_id;
  const uint8_t seq = c->tx_borrow_seq;
  c->tx_borrowed = false;
  c->tx_borrow_kind = ESY_PROTO_BIN_BORROW_NONE;

  if (kind == ESY_PROTO_BIN_BORROW_DATA && c->in_flight.active &&
      c->in_flight.transaction_id == transaction_id && c->in_flight.seq == seq &&
      c->in_flight.phase == ESY_PROTO_BIN_TX_PHASE_TRANSMITTING) {
    if (result != ESY_OK) {
      fail_in_flight(c, ESY_PROTO_BIN_TP_EVENT_IO_ERROR);
    } else {
      c->in_flight.phase = ESY_PROTO_BIN_TX_PHASE_WAIT_ACK;
      c->in_flight.deadline_ms = esy_hal_system_get_tick() +
          (c->in_flight.retry_requested ? 0u : c->cfg.acknowledgement_timeout_ms);
    }
  } else if (kind == ESY_PROTO_BIN_BORROW_CONTROL && result != ESY_OK) {
    /* A later RX frame may already own control_pending. Keep its ACK as well. */
    c->control_retry_seq = seq;
    c->control_retry_pending = true;
    notify_event(c, ESY_PROTO_BIN_TP_EVENT_IO_ERROR,
        ESY_PROTO_BIN_INVALID_TRANSACTION_ID);
  }
  c->polling = false;
  return ESY_OK;
}

bool esy_proto_bin_session_is_initialized(const esy_proto_bin_session_t *session) {
  const esy_proto_bin_session_ctx_t *c = session == NULL ? NULL :
      (const esy_proto_bin_session_ctx_t *)(const void *)session->priv.bytes;
  return c != NULL && c->initialized;
}

bool esy_proto_bin_session_is_idle(const esy_proto_bin_session_t *session) {
  return bin_is_idle(session_context((esy_proto_bin_session_t *)session));
}

uint8_t *esy_proto_bin_session_get_tx_buffer(esy_proto_bin_session_t *session,
                                             uint32_t *capacity) {
  return bin_get_tx_buffer(session_context(session), capacity);
}

esy_err_t esy_proto_bin_session_commit_tx(esy_proto_bin_session_t *session,
                                          uint32_t length, uint32_t *transaction_id) {
  if (transaction_id != NULL) *transaction_id = ESY_PROTO_BIN_INVALID_TRANSACTION_ID;
  if (session == NULL) return ESY_ERR_INVALID_ARG;
  return bin_commit_tx(session_context(session), length, transaction_id);
}

esy_err_t esy_proto_bin_session_cancel_tx(esy_proto_bin_session_t *session) {
  esy_proto_bin_session_ctx_t *c = session_context(session);
  if (c == NULL) return ESY_ERR_INVALID_ARG;
  if (!c->initialized) return ESY_ERR_STATE;
  return bin_cancel_tx(c);
}

void esy_proto_bin_session_poll(esy_proto_bin_session_t *session) {
  esy_proto_bin_session_ctx_t *c = session_context(session);
  if (c == NULL || !c->initialized || c->polling || c->resetting) return;
  c->polling = true;
  bin_poll(c);
  c->polling = false;
}

void esy_proto_bin_session_reset_rx(esy_proto_bin_session_t *session) {
  esy_proto_bin_session_ctx_t *c = session_context(session);
  if (c == NULL || !c->initialized || c->polling || c->resetting) return;
  esy_proto_bin_parser_reset(&c->parser, &c->rx_frame);
}

void esy_proto_bin_session_reset_session(esy_proto_bin_session_t *session) {
  esy_proto_bin_session_ctx_t *c = session_context(session);
  if (c == NULL || !c->initialized || c->polling || c->resetting) return;
  c->resetting = true;
  esy_proto_bin_parser_reset(&c->parser, &c->rx_frame);
  c->received_seq_valid = false;
  c->control_pending = false;
  c->control_retry_pending = false;
  c->tx_borrow_kind = ESY_PROTO_BIN_BORROW_NONE;
  c->reserved_wblk = NULL;
  c->in_flight.active = false;
  /* Queued frames belong to the old peer; never replay them to a new peer. */
  while (esy_block_ring_get_read_block(&c->tx_ring)) {
    uint8_t *block = esy_block_ring_get_read_block(&c->tx_ring);
    uint32_t id;
    memcpy(&id, block + sizeof(uint32_t), sizeof(id));
    esy_block_ring_commit_read(&c->tx_ring);
    notify_event(c, ESY_PROTO_BIN_TP_EVENT_IO_ERROR, id);
  }
  c->resetting = false;
}

static esy_err_t sender_acquire(void *context, uint8_t **buffer, size_t *capacity) {
  if (buffer != NULL) *buffer = NULL;
  if (capacity != NULL) *capacity = 0u;
  if (context == NULL || buffer == NULL || capacity == NULL) return ESY_ERR_INVALID_ARG;
  esy_proto_bin_session_ctx_t *c = session_context(context);
  if (!c->initialized || c->resetting) return ESY_ERR_STATE;
  if (c->reserved_wblk != NULL) return ESY_ERR_BUSY;
  uint32_t available = 0u;
  uint8_t *payload = bin_get_tx_buffer(c, &available);
  if (payload == NULL) return ESY_ERR_NO_MEM;
  *buffer = payload;
  *capacity = available;
  return ESY_OK;
}

static esy_err_t sender_commit(void *context, const uint8_t *buffer,
                                size_t length, uint32_t *transaction_id) {
  if (transaction_id != NULL) *transaction_id = ESY_PROTO_BIN_INVALID_TRANSACTION_ID;
  if (context == NULL || buffer == NULL || transaction_id == NULL || length > UINT32_MAX)
    return ESY_ERR_INVALID_ARG;
  esy_proto_bin_session_ctx_t *c = session_context(context);
  if (!c->initialized || c->resetting || c->reserved_wblk == NULL) return ESY_ERR_STATE;
  if (buffer != c->reserved_wblk + ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES)
    return ESY_ERR_INVALID_ARG;
  return bin_commit_tx(c, (uint32_t)length, transaction_id);
}

static esy_err_t sender_cancel(void *context) {
  return esy_proto_bin_session_cancel_tx(context);
}

esy_err_t esy_proto_bin_session_get_sender(esy_proto_bin_session_t *session,
                                           esy_proto_bin_sender_t *sender) {
  if (session == NULL || sender == NULL) return ESY_ERR_INVALID_ARG;
  if (!esy_proto_bin_session_is_initialized(session)) return ESY_ERR_STATE;
  *sender = (esy_proto_bin_sender_t){sender_acquire, sender_commit, sender_cancel, session};
  return ESY_OK;
}
