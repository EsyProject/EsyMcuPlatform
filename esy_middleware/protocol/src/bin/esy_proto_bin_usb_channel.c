/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_bin_usb_channel.h"
#include "esy_proto_bin_channel_storage_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  esy_usb_cdc_t *cdc;
  esy_proto_bin_session_t session;
  esy_proto_bin_usb_channel_static_resources_t resources;
  esy_proto_bin_tp_data_cb_t data_cb;
  esy_proto_bin_tp_event_cb_t event_cb;
  void *callback_user_data;
  uint32_t max_frame_bytes;
  uint32_t tx_available_before;
  uint32_t session_id;
  bool tx_pending;
  bool active;
  bool polling;
} esy_proto_bin_usb_channel_ctx_t;

_Static_assert(ESY_PROTO_BIN_USB_CHANNEL_CTX_BYTES >=
                   sizeof(esy_proto_bin_usb_channel_ctx_t),
               "ESY_PROTO_BIN_USB_CHANNEL_CTX_BYTES too small");

_Static_assert(_Alignof(esy_proto_bin_usb_channel_ctx_t) <= _Alignof(esy_proto_bin_usb_channel_t),
               "Channel storage alignment is insufficient");

static esy_proto_bin_usb_channel_ctx_t *channel_get_ctx(
    esy_proto_bin_usb_channel_t *channel) {
  if (channel == NULL) {
    return NULL;
  }
  return (esy_proto_bin_usb_channel_ctx_t *)(void *)channel->priv.bytes;
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

static bool resources_are_valid(const esy_proto_bin_usb_channel_t *channel,
                                const esy_proto_bin_usb_channel_config_t *config) {
  const esy_proto_bin_usb_channel_static_resources_t *resources =
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

static bool channel_is_tx_busy(void *user_data) {
  esy_proto_bin_usb_channel_ctx_t *ctx =
      (esy_proto_bin_usb_channel_ctx_t *)user_data;
  uint32_t available;

  if ((ctx == NULL) || !ctx->active) return false;
  if (!esy_usb_cdc_connected(ctx->cdc) ||
      ctx->session_id != esy_usb_cdc_get_session_id(ctx->cdc)) {
    return true;
  }
  available = esy_usb_cdc_write_available(ctx->cdc);
  if (ctx->tx_pending && (available >= ctx->tx_available_before)) {
    ctx->tx_pending = false;
  }
  return ctx->tx_pending || (available < ctx->max_frame_bytes);
}

static bool channel_is_rx_ready(void *user_data) {
  esy_proto_bin_usb_channel_ctx_t *ctx =
      (esy_proto_bin_usb_channel_ctx_t *)user_data;

  return (ctx != NULL) && ctx->active &&
         esy_usb_cdc_connected(ctx->cdc) &&
         ctx->session_id == esy_usb_cdc_get_session_id(ctx->cdc) &&
         (esy_usb_cdc_available(ctx->cdc) > 0u);
}

static esy_err_t channel_send(void *user_data, const uint8_t *data,
                              uint32_t length) {
  esy_proto_bin_usb_channel_ctx_t *ctx =
      (esy_proto_bin_usb_channel_ctx_t *)user_data;
  uint32_t available;

  if ((ctx == NULL) || !ctx->active || (data == NULL) || (length == 0u)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!esy_usb_cdc_connected(ctx->cdc) ||
      ctx->session_id != esy_usb_cdc_get_session_id(ctx->cdc)) {
    return ESY_ERR_STATE;
  }
  if (ctx->tx_pending) {
    return ESY_ERR_BUSY;
  }
  available = esy_usb_cdc_write_available(ctx->cdc);
  if (available < length) {
    return ESY_ERR_BUSY;
  }
  if (esy_usb_cdc_write(ctx->cdc, data, length) != length) {
    return ESY_ERR_IO;
  }
  ctx->tx_available_before = available;
  ctx->tx_pending = true;
  esy_usb_cdc_flush(ctx->cdc);
  return ESY_OK;
}

static bool channel_receive(void *user_data, uint8_t *byte) {
  esy_proto_bin_usb_channel_ctx_t *ctx =
      (esy_proto_bin_usb_channel_ctx_t *)user_data;

  return (ctx != NULL) && ctx->active && (byte != NULL) &&
         esy_usb_cdc_connected(ctx->cdc) &&
         ctx->session_id == esy_usb_cdc_get_session_id(ctx->cdc) &&
         (esy_usb_cdc_read(ctx->cdc, byte, 1u) == 1u);
}

static void channel_observe_session(esy_proto_bin_usb_channel_ctx_t *ctx) {
  const uint32_t session_id = esy_usb_cdc_get_session_id(ctx->cdc);
  if (session_id == ctx->session_id) return;
  ctx->session_id = session_id;
  ctx->tx_pending = false;
  ctx->tx_available_before = 0u;
  esy_proto_bin_session_reset_session(&ctx->session);
}

static void channel_on_data(const esy_proto_bin_frame_t *frame,
                            void *user_data) {
  esy_proto_bin_usb_channel_ctx_t *ctx =
      (esy_proto_bin_usb_channel_ctx_t *)user_data;

  if ((ctx != NULL) && (ctx->data_cb != NULL)) {
    ctx->data_cb(frame, ctx->callback_user_data);
  }
}

static void channel_on_event(esy_proto_bin_tp_event_t event,
                             uint32_t transaction_id, void *user_data) {
  esy_proto_bin_usb_channel_ctx_t *ctx =
      (esy_proto_bin_usb_channel_ctx_t *)user_data;

  if ((ctx != NULL) && (ctx->event_cb != NULL)) {
    ctx->event_cb(event, transaction_id, ctx->callback_user_data);
  }
}

void esy_proto_bin_usb_channel_default_cfg(
    esy_proto_bin_usb_channel_config_t *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

esy_err_t esy_proto_bin_usb_channel_init(
    esy_proto_bin_usb_channel_t *channel,
    const esy_proto_bin_usb_channel_config_t *config) {
  esy_proto_bin_usb_channel_ctx_t *ctx;
  esy_err_t ret;

  if (!esy_proto_bin_channel_range_is_valid(channel, sizeof(*channel)) ||
      !esy_proto_bin_channel_range_is_valid(config, sizeof(*config)) ||
      (config->cdc == NULL) ||
      (config->tx_payload_max == 0u) || (config->rx_payload_max == 0u) ||
      (config->tx_payload_max > UINT32_MAX - ESY_PROTO_BIN_OVERHEAD_BYTES) ||
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
  if (ctx->active) return ESY_ERR_STATE;
  if (esy_usb_cdc_get_session_id(config->cdc) == 0u) return ESY_ERR_INVALID_ARG;
  memset(ctx, 0, sizeof(*ctx));
  ctx->cdc = config->cdc;
  ctx->session_id = esy_usb_cdc_get_session_id(config->cdc);
  ctx->resources = *config->static_resources;
  ctx->data_cb = config->data_cb;
  ctx->event_cb = config->event_cb;
  ctx->callback_user_data = config->callback_user_data;
  ctx->max_frame_bytes =
      config->tx_payload_max + ESY_PROTO_BIN_OVERHEAD_BYTES;

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

  ctx->active = true;
  return ESY_OK;
}

esy_err_t esy_proto_bin_usb_channel_poll(esy_proto_bin_usb_channel_t *channel) {
  esy_proto_bin_usb_channel_ctx_t *ctx = channel_get_ctx(channel);

  if ((ctx == NULL) || !ctx->active) {
    return ESY_ERR_STATE;
  }

  if (ctx->polling) return ESY_ERR_BUSY;
  ctx->polling = true;
  esy_usb_cdc_task();
  channel_observe_session(ctx);
  if (esy_usb_cdc_connected(ctx->cdc)) {
    esy_proto_bin_session_poll(&ctx->session);
    channel_observe_session(ctx);
  }
  ctx->polling = false;
  return ESY_OK;
}

esy_err_t esy_proto_bin_usb_channel_deinit(esy_proto_bin_usb_channel_t *channel) {
  esy_proto_bin_usb_channel_ctx_t *ctx = channel_get_ctx(channel);
  if (ctx == NULL || !ctx->active) return ESY_OK;
  if (ctx->polling) return ESY_ERR_BUSY;
  /* CDC FIFO writes copy immediately. Pending FIFO retirement is not a borrow
   * of session workspace; stop this borrower without inventing completion. */
  ctx->active = false;
  esy_err_t result = esy_proto_bin_session_deinit(&ctx->session);
  if (result != ESY_OK) {
    ctx->active = true;
    return result;
  }
  memset(channel, 0, sizeof(*channel));
  return ESY_OK;
}

esy_proto_bin_session_t *esy_proto_bin_usb_channel_get_session(
    esy_proto_bin_usb_channel_t *channel) {
  esy_proto_bin_usb_channel_ctx_t *ctx = channel_get_ctx(channel);

  if ((ctx == NULL) || !ctx->active) {
    return NULL;
  }
  return &ctx->session;
}
