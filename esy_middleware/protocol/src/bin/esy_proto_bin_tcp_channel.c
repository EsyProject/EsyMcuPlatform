/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_bin_tcp_channel.h"
#include "esy_proto_bin_channel_storage_internal.h"

#include <limits.h>
#include <string.h>

typedef struct {
  esy_network_connection_t connection;
  esy_proto_bin_session_t session;
  esy_proto_bin_tcp_channel_static_resources_t resources;
  esy_proto_bin_tp_data_cb_t data_cb;
  esy_proto_bin_tp_event_cb_t event_cb;
  void *callback_user_data;
  uint64_t session_id;
  bool active;
  bool session_active;
  bool polling;
  bool send_blocked;
  bool peer_drained;
} esy_proto_bin_tcp_channel_ctx_t;

_Static_assert(sizeof(esy_proto_bin_tcp_channel_ctx_t) <=
                   ESY_PROTO_BIN_TCP_CHANNEL_CTX_BYTES,
               "ESY_PROTO_BIN_TCP_CHANNEL_CTX_BYTES is too small");

_Static_assert(_Alignof(esy_proto_bin_tcp_channel_ctx_t) <= _Alignof(esy_proto_bin_tcp_channel_t),
               "Channel storage alignment is insufficient");

static esy_proto_bin_tcp_channel_ctx_t *channel_get_ctx(esy_proto_bin_tcp_channel_t *channel) {
  return channel == NULL ? NULL :
      (esy_proto_bin_tcp_channel_ctx_t *)(void *)channel->priv.bytes;
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

static bool resources_are_valid(
    const esy_proto_bin_tcp_channel_t *channel,
    const esy_proto_bin_tcp_channel_config_t *config) {
  const esy_proto_bin_tcp_channel_static_resources_t *resources =
      config->static_resources;

  if (!esy_proto_bin_channel_range_is_valid(resources, sizeof(*resources)) ||
      (resources->tx_queue_storage == NULL) ||
      (resources->tx_work_buf == NULL) || (resources->rx_work_buf == NULL)) {
    return false;
  }

  const esy_proto_bin_channel_range_t ranges[] = {
      {channel, sizeof(*channel)},
      {config, sizeof(*config)},
      {resources, sizeof(*resources)},
      {resources->tx_queue_storage, resources->tx_queue_storage_size},
      {resources->tx_work_buf, resources->tx_work_buf_size},
      {resources->rx_work_buf, resources->rx_work_buf_size},
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
             (size_t)config->rx_payload_max + ESY_PROTO_BIN_OVERHEAD_BYTES;
}

static void channel_on_data(const esy_proto_bin_frame_t *frame, void *argument) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = argument;
  if (ctx->data_cb != NULL) ctx->data_cb(frame, ctx->callback_user_data);
}

static void channel_on_event(esy_proto_bin_tp_event_t event,
                             uint32_t transaction_id, void *argument) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = argument;
  if (ctx->event_cb != NULL) {
    ctx->event_cb(event, transaction_id, ctx->callback_user_data);
  }
}

static bool channel_is_tx_busy(void *argument) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = argument;
  if (!ctx->active) return false;
  esy_network_connection_status_t status;
  esy_err_t ret = ctx->connection.get_status(ctx->connection.stream.context, &status);
  if (ret != ESY_OK) {
    ctx->send_blocked = true;
    return true;
  }
  return (status.state != ESY_NETWORK_CONNECTION_OPEN && status.state != ESY_NETWORK_CONNECTION_EOF) ||
      status.session_id != ctx->session_id || status.unacked_bytes != 0u;
}

static bool channel_is_rx_ready(void *argument) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = argument;
  esy_network_connection_status_t status;
  return ctx->connection.get_status(ctx->connection.stream.context, &status) == ESY_OK &&
      (status.state == ESY_NETWORK_CONNECTION_OPEN || status.state == ESY_NETWORK_CONNECTION_EOF) &&
      status.session_id == ctx->session_id && status.rx_available != 0u;
}

static esy_err_t channel_send(void *argument, const uint8_t *data,
                              uint32_t length) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = argument;
  if (channel_is_tx_busy(ctx)) return ESY_ERR_BUSY;
  int32_t sent = ctx->connection.send_all(ctx->connection.stream.context, data, length);
  if (sent < 0) {
    if (sent == ESY_ERR_BUSY) ctx->send_blocked = true;
    return (esy_err_t)sent;
  }
  if (sent == 0) {
    ctx->send_blocked = true;
    return ESY_ERR_BUSY;
  }
  return (uint32_t)sent == length ? ESY_OK : ESY_ERR_IO;
}

static bool channel_receive(void *argument, uint8_t *byte) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = argument;
  return ctx->connection.stream.recv(ctx->connection.stream.context, byte, 1u) == 1;
}

void esy_proto_bin_tcp_channel_default_cfg(esy_proto_bin_tcp_channel_config_t *config) {
  if (config != NULL) memset(config, 0, sizeof(*config));
}

esy_err_t esy_proto_bin_tcp_channel_init(esy_proto_bin_tcp_channel_t *channel,
                                    const esy_proto_bin_tcp_channel_config_t *config) {
  if (!esy_proto_bin_channel_range_is_valid(channel, sizeof(*channel)) ||
      !esy_proto_bin_channel_range_is_valid(config, sizeof(*config)) ||
      config->connection.send_all == NULL ||
      config->connection.stream.recv == NULL || config->connection.get_status == NULL ||
      config->tx_payload_max == 0u || config->rx_payload_max == 0u ||
      config->tx_payload_max > UINT16_MAX - ESY_PROTO_BIN_OVERHEAD_BYTES ||
      config->rx_payload_max > UINT16_MAX ||
      !is_power_of_two(config->tx_queue_block_count) ||
      config->tx_queue_block_count > SIZE_MAX /
          ((size_t)config->tx_payload_max + ESY_PROTO_BIN_TX_BLOCK_OVERHEAD_BYTES) ||
      !resources_are_valid(channel, config)) return ESY_ERR_INVALID_ARG;
  esy_proto_bin_tcp_channel_ctx_t *ctx = channel_get_ctx(channel);
  if (ctx->active) return ESY_ERR_STATE;
  esy_network_connection_status_t status;
  esy_err_t ret = config->connection.get_status(config->connection.stream.context, &status);
  if (ret != ESY_OK) return ret;
  memset(ctx, 0, sizeof(*ctx));
  ctx->connection = config->connection;
  ctx->resources = *config->static_resources;
  ctx->data_cb = config->data_cb;
  ctx->event_cb = config->event_cb;
  ctx->callback_user_data = config->callback_user_data;
  ctx->session_id = status.session_id;
  ctx->session_active = status.state == ESY_NETWORK_CONNECTION_OPEN ||
      status.state == ESY_NETWORK_CONNECTION_EOF;
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
    memset(ctx, 0, sizeof(*ctx));
    return ret;
  }
  ctx->active = true;
  return ESY_OK;
}

esy_err_t esy_proto_bin_tcp_channel_poll(esy_proto_bin_tcp_channel_t *channel) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = channel_get_ctx(channel);
  if (ctx == NULL || !ctx->active) return ESY_ERR_STATE;
  if (ctx->polling) return ESY_ERR_BUSY;
  ctx->polling = true;
  esy_err_t ret = ESY_OK;
  if (ctx->connection.stream.progress != NULL) ret = ctx->connection.stream.progress(ctx->connection.stream.context);
  esy_network_connection_status_t status;
  if (ret == ESY_OK) ret = ctx->connection.get_status(ctx->connection.stream.context, &status);
  if (ret != ESY_OK) {
    if (ret != ESY_ERR_BUSY) {
      if (ctx->session_active) esy_proto_bin_session_reset_session(&ctx->session);
      ctx->session_active = false;
      ctx->peer_drained = false;
    }
    ctx->polling = false;
    return ret;
  }
  if (status.session_id != ctx->session_id || status.state != ESY_NETWORK_CONNECTION_EOF) {
    ctx->peer_drained = false;
  }
  if (ctx->session_active &&
      ((status.state != ESY_NETWORK_CONNECTION_OPEN && status.state != ESY_NETWORK_CONNECTION_EOF) ||
       status.session_id != ctx->session_id)) {
    esy_proto_bin_session_reset_session(&ctx->session);
    ctx->session_active = false;
  }
  if (status.state == ESY_NETWORK_CONNECTION_OPEN ||
      (status.state == ESY_NETWORK_CONNECTION_EOF && !ctx->peer_drained)) {
    ctx->session_id = status.session_id;
    ctx->session_active = true;
    ctx->send_blocked = false;
    if (status.state == ESY_NETWORK_CONNECTION_OPEN) ctx->peer_drained = false;
    esy_proto_bin_session_poll(&ctx->session);
    if (status.state == ESY_NETWORK_CONNECTION_EOF) {
      ret = ctx->connection.get_status(ctx->connection.stream.context, &status);
      if (ret == ESY_OK && status.state == ESY_NETWORK_CONNECTION_EOF &&
          status.session_id == ctx->session_id && status.rx_available == 0u &&
          status.unacked_bytes == 0u && !ctx->send_blocked) {
        esy_proto_bin_session_reset_session(&ctx->session);
        ctx->session_active = false;
        ctx->peer_drained = true;
      } else if ((ret != ESY_OK && ret != ESY_ERR_BUSY) ||
                 (ret == ESY_OK && (status.session_id != ctx->session_id ||
                  (status.state != ESY_NETWORK_CONNECTION_OPEN &&
                   status.state != ESY_NETWORK_CONNECTION_EOF)))) {
        esy_proto_bin_session_reset_session(&ctx->session);
        ctx->session_active = false;
        ctx->peer_drained = false;
      }
    }
  }
  ctx->polling = false;
  return ret;
}

bool esy_proto_bin_tcp_channel_is_peer_drained(const esy_proto_bin_tcp_channel_t *channel) {
  const esy_proto_bin_tcp_channel_ctx_t *ctx = channel == NULL ? NULL :
      (const esy_proto_bin_tcp_channel_ctx_t *)(const void *)channel->priv.bytes;
  return ctx != NULL && ctx->active && ctx->peer_drained;
}

esy_err_t esy_proto_bin_tcp_channel_deinit(esy_proto_bin_tcp_channel_t *channel) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = channel_get_ctx(channel);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  if (!ctx->active) return ESY_ERR_STATE;
  if (ctx->polling) return ESY_ERR_BUSY;
  /* Network sends copied their bytes before return. Stop the borrower without
   * treating pending peer ACKs as a workspace borrow or reporting protocol ACK. */
  ctx->active = false;
  esy_err_t result = esy_proto_bin_session_deinit(&ctx->session);
  if (result != ESY_OK) {
    ctx->active = true;
    return result;
  }
  memset(channel, 0, sizeof(*channel));
  return ESY_OK;
}

esy_proto_bin_session_t *esy_proto_bin_tcp_channel_get_session(esy_proto_bin_tcp_channel_t *channel) {
  esy_proto_bin_tcp_channel_ctx_t *ctx = channel_get_ctx(channel);
  return ctx == NULL || !ctx->active ? NULL : &ctx->session;
}
