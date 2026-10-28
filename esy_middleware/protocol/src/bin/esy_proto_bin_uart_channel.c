/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_bin_uart_channel.h"
#include "esy_proto_bin_channel_storage_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler.h"

#define ESY_PROTO_BIN_UART_STREAM_EVENT_ERROR (1u << 0)
#define ESY_PROTO_BIN_UART_TX_REQUEST_COUNT 2u

typedef struct {
  esy_uart_t *uart;
  esy_proto_bin_session_t session;
  esy_uart_tx_scheduler_t tx;
  esy_uart_tx_scheduler_tx_request_t tx_requests[ESY_PROTO_BIN_UART_TX_REQUEST_COUNT];
  esy_proto_bin_uart_channel_static_resources_t resources;
  esy_proto_bin_tp_data_cb_t data_cb;
  esy_proto_bin_tp_event_cb_t event_cb;
  void *callback_user_data;
  const uint8_t *rx_span;
  size_t rx_span_len;
  size_t rx_span_offset;
  atomic_uint_fast32_t stream_events;
  bool active;
  bool polling;
  bool rx_borrowed;
  bool tx_initialized;
  bool closing;
  bool tx_enqueued;
  bool tx_completion_pending;
  esy_err_t tx_completion_result;
  esy_err_t tx_result;
} esy_proto_bin_uart_channel_ctx_t;

_Static_assert(ESY_PROTO_BIN_UART_CHANNEL_CTX_BYTES >=
                   sizeof(esy_proto_bin_uart_channel_ctx_t),
               "ESY_PROTO_BIN_UART_CHANNEL_CTX_BYTES too small");

_Static_assert(_Alignof(esy_proto_bin_uart_channel_ctx_t) <= _Alignof(esy_proto_bin_uart_channel_t),
               "Channel storage alignment is insufficient");

static esy_proto_bin_uart_channel_ctx_t *channel_get_ctx(
    esy_proto_bin_uart_channel_t *channel) {
  if (channel == NULL) {
    return NULL;
  }
  return (esy_proto_bin_uart_channel_ctx_t *)(void *)channel->priv.bytes;
}

static bool is_power_of_two(uint32_t value) {
  return value >= 2u && ((value & (value - 1u)) == 0u);
}

static size_t tx_queue_storage_bytes(uint32_t tx_payload_max,
                                     uint32_t block_count) {
  return ((size_t)tx_payload_max +
          (size_t)ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES) *
         (size_t)block_count;
}

static bool resources_are_valid(const esy_proto_bin_uart_channel_t *channel,
                                const esy_proto_bin_uart_channel_config_t *config) {
  const esy_proto_bin_uart_channel_static_resources_t *resources =
      config->static_resources;

  if (!esy_proto_bin_channel_range_is_valid(resources, sizeof(*resources)) ||
      (resources->tx_queue_storage == NULL) ||
      (resources->tx_work_buf == NULL) || (resources->rx_work_buf == NULL) ||
      (resources->rx_stream_storage == NULL)) {
    return false;
  }

  const esy_proto_bin_channel_range_t ranges[] = {
      {channel, sizeof(*channel)},
      {config, sizeof(*config)},
      {resources, sizeof(*resources)},
      {resources->tx_queue_storage, resources->tx_queue_storage_size},
      {resources->tx_work_buf, resources->tx_work_buf_size},
      {resources->rx_work_buf, resources->rx_work_buf_size},
      {resources->rx_stream_storage, resources->rx_stream_storage_size},
  };
  if (!esy_proto_bin_channel_ranges_are_disjoint(ranges, sizeof(ranges) / sizeof(ranges[0])))
    return false;

  if (resources->tx_work_buf_size > UINT32_MAX ||
      resources->rx_work_buf_size > UINT32_MAX)
    return false;
  return resources->tx_queue_storage_size >=
             tx_queue_storage_bytes(config->tx_payload_max,
                                    config->tx_queue_block_count) &&
         resources->tx_work_buf_size >=
             (size_t)config->tx_payload_max + ESY_PROTO_BIN_OVERHEAD_BYTES &&
         resources->rx_work_buf_size >=
             (size_t)config->rx_payload_max + ESY_PROTO_BIN_OVERHEAD_BYTES &&
         resources->rx_stream_storage_size >= 2u;
}

static void channel_rx_stream_callback(esy_uart_t *uart,
                                       esy_uart_rx_stream_event_t event,
                                       void *user_data) {
  esy_proto_bin_uart_channel_ctx_t *ctx = (esy_proto_bin_uart_channel_ctx_t *)user_data;

  (void)uart;
  if ((ctx != NULL) && (event != ESY_UART_RX_STREAM_EVENT_DATA)) {
    atomic_fetch_or_explicit(&ctx->stream_events,
                             ESY_PROTO_BIN_UART_STREAM_EVENT_ERROR,
                             memory_order_release);
  }
}

static bool channel_is_tx_busy(void *user_data) {
  esy_proto_bin_uart_channel_ctx_t *ctx = (esy_proto_bin_uart_channel_ctx_t *)user_data;

  return (ctx != NULL) && ctx->active && ctx->tx_initialized &&
         esy_uart_tx_scheduler_is_busy(&ctx->tx);
}

static void channel_tx_complete(const uint8_t *data, size_t length,
    esy_uart_tx_scheduler_tx_status_t status, esy_err_t error, void *arg) {
  (void)data;
  (void)length;
  esy_proto_bin_uart_channel_ctx_t *ctx = arg;
  if (ctx->closing) return;
  ctx->tx_completion_result = status == ESY_UART_TX_SCHEDULER_TX_OK ? ESY_OK :
      (error != ESY_OK ? error : ESY_ERR_IO);
  ctx->tx_completion_pending = true;
}

static esy_err_t channel_process_tx(esy_proto_bin_uart_channel_ctx_t *ctx) {
  esy_err_t processed = esy_uart_tx_scheduler_process(&ctx->tx);
  if (!ctx->tx_completion_pending) return processed;
  const esy_err_t result = ctx->tx_completion_result;
  if (ctx->tx_result == ESY_OK && result != ESY_OK) ctx->tx_result = result;
  /* The scheduler has returned and released its immutable frame borrow before
   * protocol completion can invoke application callbacks or reuse work storage. */
  esy_err_t notified = esy_proto_bin_session_on_tx_complete(&ctx->session, result);
  if (notified == ESY_OK) ctx->tx_completion_pending = false;
  return processed != ESY_OK ? processed : notified;
}

static void channel_on_data(const esy_proto_bin_frame_t *frame,
                            void *user_data) {
  esy_proto_bin_uart_channel_ctx_t *ctx = (esy_proto_bin_uart_channel_ctx_t *)user_data;

  if ((ctx != NULL) && (ctx->data_cb != NULL)) {
    ctx->data_cb(frame, ctx->callback_user_data);
  }
}

static void channel_on_event(esy_proto_bin_tp_event_t event,
                             uint32_t transaction_id, void *user_data) {
  esy_proto_bin_uart_channel_ctx_t *ctx = (esy_proto_bin_uart_channel_ctx_t *)user_data;

  if ((ctx != NULL) && (ctx->event_cb != NULL)) {
    ctx->event_cb(event, transaction_id, ctx->callback_user_data);
  }
}

static bool channel_is_rx_ready(void *user_data) {
  esy_proto_bin_uart_channel_ctx_t *ctx = (esy_proto_bin_uart_channel_ctx_t *)user_data;
  esy_err_t ret;

  if ((ctx == NULL) || !ctx->active || ctx->closing) {
    return false;
  }
  if (ctx->rx_span_offset < ctx->rx_span_len) {
    return true;
  }

  ctx->rx_span = NULL;
  ctx->rx_span_len = 0u;
  ctx->rx_span_offset = 0u;
  ret = esy_hal_uart_rx_stream_peek(ctx->uart, &ctx->rx_span,
                                         &ctx->rx_span_len);
  if (ret != ESY_OK) {
    ctx->rx_span = NULL;
    ctx->rx_span_len = 0u;
    return false;
  }
  return ctx->rx_span_len > 0u;
}

static esy_err_t channel_send(void *user_data, const uint8_t *data,
                              uint32_t length) {
  esy_proto_bin_uart_channel_ctx_t *ctx = (esy_proto_bin_uart_channel_ctx_t *)user_data;

  if ((ctx == NULL) || !ctx->active || ctx->closing ||
      (data == NULL) || (length == 0u)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (ctx->tx_completion_pending) return ESY_ERR_BUSY;

  const esy_uart_tx_scheduler_tx_request_t request = {
      .data = data, .length = length,
      .on_complete = channel_tx_complete, .user_data = ctx};
  esy_err_t result = esy_uart_tx_scheduler_submit(&ctx->tx, &request);
  if (result == ESY_OK) ctx->tx_enqueued = true;
  return result == ESY_ERR_NO_MEM ? ESY_ERR_BUSY : result;
}

static bool channel_receive(void *user_data, uint8_t *byte) {
  esy_proto_bin_uart_channel_ctx_t *ctx = (esy_proto_bin_uart_channel_ctx_t *)user_data;
  esy_err_t ret;

  if ((ctx == NULL) || !ctx->active || (byte == NULL) ||
      (ctx->rx_span_offset >= ctx->rx_span_len)) {
    return false;
  }

  *byte = ctx->rx_span[ctx->rx_span_offset++];
  if (ctx->rx_span_offset < ctx->rx_span_len) {
    return true;
  }

  ret = esy_hal_uart_rx_stream_consume(ctx->uart, ctx->rx_span_len);
  ctx->rx_span = NULL;
  ctx->rx_span_len = 0u;
  ctx->rx_span_offset = 0u;
  return ret == ESY_OK;
}

static esy_err_t channel_restart_rx_stream(esy_proto_bin_uart_channel_ctx_t *ctx) {
  esy_err_t ret;

  ret = esy_hal_uart_rx_stream_stop(ctx->uart);
  if (ret != ESY_OK) {
    return ret;
  }

  esy_proto_bin_session_reset_rx(&ctx->session);
  ctx->rx_span = NULL;
  ctx->rx_span_len = 0u;
  ctx->rx_span_offset = 0u;
  ctx->rx_borrowed = true;
  ret = esy_hal_uart_rx_stream_start(
      ctx->uart, ctx->resources.rx_stream_storage,
      ctx->resources.rx_stream_storage_size, channel_rx_stream_callback, ctx);
  if (ret == ESY_ERR_INVALID_ARG || ret == ESY_ERR_NOT_SUPPORTED ||
      ret == ESY_ERR_NOT_IMPLEMENTED) ctx->rx_borrowed = false;
  return ret;
}

void esy_proto_bin_uart_channel_default_cfg(
    esy_proto_bin_uart_channel_config_t *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

esy_err_t esy_proto_bin_uart_channel_init(
    esy_proto_bin_uart_channel_t *channel,
    const esy_proto_bin_uart_channel_config_t *config) {
  esy_proto_bin_uart_channel_ctx_t *ctx;
  esy_err_t ret;

  if (!esy_proto_bin_channel_range_is_valid(channel, sizeof(*channel)) ||
      !esy_proto_bin_channel_range_is_valid(config, sizeof(*config)) ||
      (config->uart == NULL) ||
      (config->tx_payload_max == 0u) || (config->rx_payload_max == 0u) ||
      !is_power_of_two(config->tx_queue_block_count) ||
      (config->tx_payload_max > UINT16_MAX) ||
      (config->rx_payload_max > UINT16_MAX) ||
      (config->tx_queue_block_count >
       SIZE_MAX / ((size_t)config->tx_payload_max +
                   ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES)) ||
      !resources_are_valid(channel, config)) {
    return ESY_ERR_INVALID_ARG;
  }

  ctx = channel_get_ctx(channel);
  if (ctx->active || ctx->rx_borrowed || ctx->tx_initialized) return ESY_ERR_STATE;
  const uint8_t *existing_span = NULL;
  size_t existing_length = 0u;
  ret = esy_hal_uart_rx_stream_peek(config->uart, &existing_span, &existing_length);
  if (ret != ESY_ERR_STATE) return ret == ESY_OK ? ESY_ERR_STATE : ret;
  memset(ctx, 0, sizeof(*ctx));
  ctx->uart = config->uart;
  ctx->resources = *config->static_resources;
  ctx->data_cb = config->data_cb;
  ctx->event_cb = config->event_cb;
  ctx->callback_user_data = config->callback_user_data;
  atomic_init(&ctx->stream_events, 0u);

  const esy_proto_bin_session_static_resources_t session_resources = {
      .tx_queue_buf = ctx->resources.tx_queue_storage,
      .tx_queue_buf_size = ctx->resources.tx_queue_storage_size,
      .tx_work_buf = ctx->resources.tx_work_buf,
      .tx_work_buf_size = ctx->resources.tx_work_buf_size,
      .rx_work_buf = ctx->resources.rx_work_buf,
      .rx_work_buf_size = ctx->resources.rx_work_buf_size,
  };
  esy_proto_bin_session_config_t session_cfg;
  esy_proto_bin_session_default_cfg(&session_cfg);
  session_cfg.static_resources = &session_resources;
  session_cfg.tx_queue_block_count = config->tx_queue_block_count;
  session_cfg.tx_payload_max = config->tx_payload_max;
  session_cfg.rx_payload_max = config->rx_payload_max;
  session_cfg.tx_completion_timeout_ms =
      config->tx_completion_timeout_ms;
  session_cfg.acknowledgement_timeout_ms =
      config->acknowledgement_timeout_ms;
  session_cfg.retries = config->retries;
  session_cfg.data_cb = channel_on_data;
  session_cfg.event_cb = channel_on_event;
  session_cfg.io.is_tx_busy = channel_is_tx_busy;
  session_cfg.io.is_rx_ready = channel_is_rx_ready;
  session_cfg.io.send = channel_send;
  session_cfg.io.receive = channel_receive;
  session_cfg.user_data = ctx;

  ret = esy_proto_bin_session_init(&ctx->session, &session_cfg);
  if (ret != ESY_OK) {
    return ret;
  }

  const esy_uart_tx_scheduler_static_resources_t tx_resources = {
      .requests = ctx->tx_requests, .request_count = ESY_PROTO_BIN_UART_TX_REQUEST_COUNT};
  ret = esy_uart_tx_scheduler_init(&ctx->tx, ctx->uart, &tx_resources);
  if (ret != ESY_OK) {
    (void)esy_proto_bin_session_deinit(&ctx->session);
    return ret;
  }
  ctx->tx_initialized = true;

  ctx->active = true;
  ctx->rx_borrowed = true;
  ret = esy_hal_uart_rx_stream_start(
      ctx->uart, ctx->resources.rx_stream_storage,
      ctx->resources.rx_stream_storage_size, channel_rx_stream_callback, ctx);
  if (ret != ESY_OK) {
    ctx->active = false;
    if (ret == ESY_ERR_INVALID_ARG || ret == ESY_ERR_NOT_SUPPORTED ||
        ret == ESY_ERR_NOT_IMPLEMENTED) ctx->rx_borrowed = false;
    esy_err_t cleanup = esy_proto_bin_uart_channel_deinit(channel);
    return cleanup == ESY_OK ? ret : cleanup;
  }
  return ESY_OK;
}

static esy_err_t channel_finish_close(esy_proto_bin_uart_channel_ctx_t *ctx) {
  esy_err_t ret = esy_uart_tx_scheduler_process(&ctx->tx);
  if (ret != ESY_OK) return ret;
  if (esy_uart_tx_scheduler_is_busy(&ctx->tx)) return ESY_ERR_BUSY;
  if (ctx->rx_borrowed) {
    ret = esy_hal_uart_rx_stream_stop(ctx->uart);
    if (ret != ESY_OK) return ret;
    ctx->rx_borrowed = false;
  }
  ret = esy_proto_bin_session_deinit(&ctx->session);
  if (ret != ESY_OK) return ret;
  ret = esy_uart_tx_scheduler_deinit(&ctx->tx);
  if (ret != ESY_OK) return ret;
  ctx->tx_initialized = false;
  ctx->active = false;
  ctx->closing = false;
  ctx->tx_completion_pending = false;
  ctx->rx_span = NULL;
  ctx->rx_span_len = 0u;
  ctx->rx_span_offset = 0u;
  return ESY_OK;
}

esy_err_t esy_proto_bin_uart_channel_poll(esy_proto_bin_uart_channel_t *channel) {
  esy_proto_bin_uart_channel_ctx_t *ctx = channel_get_ctx(channel);
  if (ctx == NULL || !ctx->active) return ESY_ERR_STATE;
  if (ctx->polling) return ESY_ERR_BUSY;
  ctx->polling = true;
  ctx->tx_result = ESY_OK;
  ctx->tx_enqueued = false;
  esy_err_t ret;
  if (ctx->closing) {
    ret = channel_finish_close(ctx);
    goto done;
  }
  ret = channel_process_tx(ctx);
  if (ret != ESY_OK || ctx->tx_result != ESY_OK) goto done;
  const uint32_t events =
      atomic_exchange_explicit(&ctx->stream_events, 0u, memory_order_acquire);
  if ((events & ESY_PROTO_BIN_UART_STREAM_EVENT_ERROR) != 0u) {
    ret = channel_restart_rx_stream(ctx);
    if (ret != ESY_OK) {
      atomic_fetch_or_explicit(&ctx->stream_events,
                               ESY_PROTO_BIN_UART_STREAM_EVENT_ERROR,
                               memory_order_release);
      goto done;
    }
  }
  esy_proto_bin_session_poll(&ctx->session);
  /* Only a freshly queued frame needs the second bounded slice. Existing
   * retained/failed work is never aborted twice or resent within one poll. */
  if (ctx->tx_enqueued) ret = channel_process_tx(ctx);
done:
  if (ctx->tx_result != ESY_OK) ret = ctx->tx_result;
  ctx->polling = false;
  return ret;
}

esy_err_t esy_proto_bin_uart_channel_deinit(esy_proto_bin_uart_channel_t *channel) {
  esy_proto_bin_uart_channel_ctx_t *ctx = channel_get_ctx(channel);
  if (ctx == NULL || (!ctx->active && !ctx->rx_borrowed && !ctx->tx_initialized))
    return ESY_OK;
  if (ctx->polling) return ESY_ERR_BUSY;
  ctx->polling = true;
  ctx->closing = true;
  ctx->tx_completion_pending = false;
  esy_err_t ret = esy_uart_tx_scheduler_request_abort(&ctx->tx);
  if (ret == ESY_OK) ret = channel_finish_close(ctx);
  ctx->polling = false;
  return ret;
}

esy_proto_bin_session_t *esy_proto_bin_uart_channel_get_session(
    esy_proto_bin_uart_channel_t *channel) {
  esy_proto_bin_uart_channel_ctx_t *ctx = channel_get_ctx(channel);

  if ((ctx == NULL) || !ctx->active || ctx->closing) {
    return NULL;
  }
  return &ctx->session;
}
