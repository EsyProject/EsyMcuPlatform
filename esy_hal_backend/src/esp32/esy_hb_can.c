/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_can.h"
#include "esy_hb_esp32_context.h"

#include "driver/twai.h"

#include <string.h>

#define ESP32_CAN_PHYSICAL_INDEX 1u
#define ESP32_CAN_ERROR_WARNING 96u
#define ESP32_CAN_ERROR_PASSIVE 128u
#define ESP32_CAN_BRP_BASE_MAX 128u

typedef struct {
  twai_handle_t native;
  esy_can_t *owner;
  esy_can_filter_t filters[ESY_CAN_ESP32_FILTER_CAPACITY];
  size_t filter_count;
  esy_can_mode_t mode;
  bool auto_retransmit;
  bool stopped;
  bool bus_off_stopped;
  bool faulted;
  bool cleanup_pending;
  bool rx_overrun;
} esy_hb_can_ctx_t;

/* One non-ISR owner serializes every facade call. The SDK ISR owns only its
 * native driver/queue, never this context, filters or caller frame storage. */
static esy_hb_can_ctx_t s_ctx;

static esy_hb_can_ctx_t *can_context(const esy_can_t *can) {
  return can != NULL && can->ctx == &s_ctx && s_ctx.owner == can
      ? &s_ctx : NULL;
}

static bool can_ready(const esy_hb_can_ctx_t *ctx) {
  return ctx != NULL && ctx->native != NULL && !ctx->cleanup_pending &&
         !ctx->faulted;
}

static esy_err_t native_error(esp_err_t error) {
  switch (error) {
    case ESP_OK: return ESY_OK;
    case ESP_ERR_INVALID_ARG: return ESY_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return ESY_ERR_STATE;
    case ESP_ERR_NO_MEM: return ESY_ERR_NO_MEM;
    case ESP_ERR_NOT_SUPPORTED: return ESY_ERR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return ESY_ERR_TIMEOUT;
    default: return ESY_ERR_IO;
  }
}

static esy_err_t validate_config(const esy_can_config_t *config) {
  if (config == NULL) return ESY_ERR_INVALID_ARG;
  if (config->phys_index != ESP32_CAN_PHYSICAL_INDEX || config->fd ||
      config->mode == ESY_CAN_MODE_LOOPBACK) return ESY_ERR_NOT_SUPPORTED;
  if (config->mode != ESY_CAN_MODE_NORMAL &&
      config->mode != ESY_CAN_MODE_LISTEN_ONLY) return ESY_ERR_INVALID_ARG;

  const esy_can_timing_t *timing = &config->nominal;
  if (timing->prescaler < TWAI_BRP_MIN || timing->prescaler > TWAI_BRP_MAX ||
      (timing->prescaler & 1u) != 0u ||
      (timing->prescaler > ESP32_CAN_BRP_BASE_MAX &&
       (timing->prescaler & 3u) != 0u) ||
      timing->segment1 == 0u || timing->segment1 > 16u ||
      timing->segment2 == 0u || timing->segment2 > 8u ||
      timing->sjw == 0u || timing->sjw > 4u ||
      timing->sjw > timing->segment2 || config->poll_limit == 0u ||
      config->poll_limit > ESY_CAN_MAX_POLL_LIMIT ||
      config->port.tx_pin >= GPIO_NUM_MAX ||
      config->port.rx_pin >= GPIO_NUM_MAX ||
      config->port.tx_pin == config->port.rx_pin ||
      config->port.rx_queue_capacity == 0u) return ESY_ERR_INVALID_ARG;
  const gpio_num_t tx_pin = (gpio_num_t)config->port.tx_pin;
  const gpio_num_t rx_pin = (gpio_num_t)config->port.rx_pin;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(tx_pin) || !GPIO_IS_VALID_GPIO(rx_pin))
    return ESY_ERR_INVALID_ARG;
  return ESY_OK;
}

/* SDK stop accepts RUNNING only, while uninstall also accepts BUS_OFF. A
 * bus-off stop remains electrically quiet and forbids start until deinit/init;
 * it never requests recovery or waits for recessive bus bits. */
static esy_err_t stop_native(esy_hb_can_ctx_t *ctx) {
  twai_status_info_t status;
  esp_err_t error = twai_get_status_info_v2(ctx->native, &status);
  if (error != ESP_OK) return native_error(error);
  if (status.state == TWAI_STATE_RUNNING) {
    error = twai_stop_v2(ctx->native);
    if (error != ESP_OK) {
      /* Bus-off may arrive between the native snapshot and stop. Observe once
       * more instead of turning that already quiescent state into a dead end. */
      if (error != ESP_ERR_INVALID_STATE) return native_error(error);
      error = twai_get_status_info_v2(ctx->native, &status);
      if (error != ESP_OK) return native_error(error);
      if (status.state != TWAI_STATE_BUS_OFF &&
          status.state != TWAI_STATE_STOPPED) return ESY_ERR_STATE;
    } else {
      status.state = TWAI_STATE_STOPPED;
    }
  }
  if (status.state != TWAI_STATE_STOPPED && status.state != TWAI_STATE_BUS_OFF)
    return ESY_ERR_STATE;
  ctx->bus_off_stopped = status.state == TWAI_STATE_BUS_OFF;
  ctx->stopped = true;
  ctx->faulted = false;
  return ESY_OK;
}

esy_err_t esy_hb_can_init(esy_can_t *can, const esy_can_config_t *config) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  if (can->ctx != NULL || can->ops != NULL) return ESY_ERR_STATE;
  esy_err_t result = validate_config(config);
  if (result != ESY_OK) return result;
  if (esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;
  if (s_ctx.owner != NULL) return ESY_ERR_BUSY;

  twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT_V2(
      0, (gpio_num_t)config->port.tx_pin, (gpio_num_t)config->port.rx_pin,
      config->mode == ESY_CAN_MODE_LISTEN_ONLY
          ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
  general.tx_queue_len = 0u;
  general.rx_queue_len = config->port.rx_queue_capacity;
  general.alerts_enabled = TWAI_ALERT_NONE;
#if CONFIG_TWAI_ISR_IN_IRAM
  general.intr_flags |= ESP_INTR_FLAG_IRAM;
#endif
  const twai_timing_config_t timing = {
      .clk_src = TWAI_CLK_SRC_APB,
      .brp = config->nominal.prescaler,
      .tseg_1 = config->nominal.segment1,
      .tseg_2 = config->nominal.segment2,
      .sjw = config->nominal.sjw,
      .triple_sampling = false,
  };
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  twai_handle_t native = NULL;
  const esp_err_t error = twai_driver_install_v2(&general, &timing, &filter,
                                                &native);
  /* The pinned SDK frees all resources on a reported install failure, and
   * publishes the returned handle only after installation has completed. */
  if (error != ESP_OK)
    return error == ESP_ERR_INVALID_STATE ? ESY_ERR_BUSY : native_error(error);
  s_ctx = (esy_hb_can_ctx_t){
      .native = native,
      .owner = can,
      .mode = config->mode,
      .auto_retransmit = config->auto_retransmit,
      .stopped = true,
  };
  can->ctx = &s_ctx;
  return ESY_OK;
}

esy_err_t esy_hb_can_start(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  esy_hb_can_ctx_t *ctx = can_context(can);
  if (!can_ready(ctx) || esy_hb_esp32_is_in_isr() || !ctx->stopped ||
      ctx->bus_off_stopped) return ESY_ERR_STATE;
  const esp_err_t error = twai_start_v2(ctx->native);
  if (error != ESP_OK) {
    ctx->faulted = true;
    return native_error(error);
  }
  ctx->stopped = false;
  return ESY_OK;
}

esy_err_t esy_hb_can_stop(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  esy_hb_can_ctx_t *ctx = can_context(can);
  if (ctx == NULL || esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;
  const esy_err_t result = stop_native(ctx);
  if (result != ESY_OK) ctx->faulted = true;
  return result;
}

esy_err_t esy_hb_can_deinit(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  if (esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;
  esy_hb_can_ctx_t *ctx = can_context(can);
  if (ctx == NULL)
    return can->ctx == NULL && can->ops == NULL ? ESY_OK : ESY_ERR_STATE;
  ctx->cleanup_pending = true;
  esy_err_t result = stop_native(ctx);
  if (result != ESY_OK) return result;
  const esp_err_t error = twai_driver_uninstall_v2(ctx->native);
  if (error != ESP_OK) return native_error(error);
  memset(ctx, 0, sizeof(*ctx));
  can->ctx = NULL;
  return ESY_OK;
}

esy_err_t esy_hb_can_send(esy_can_t *can, const esy_can_frame_t *frame) {
  if (can == NULL || frame == NULL) return ESY_ERR_INVALID_ARG;
  if (frame->fd || frame->brs) return ESY_ERR_NOT_SUPPORTED;
  if (frame->length > ESY_CAN_CLASSIC_MAX_DATA || frame->esi ||
      frame->id > (frame->extended ? ESY_CAN_EXT_ID_MAX : ESY_CAN_STD_ID_MAX))
    return ESY_ERR_INVALID_ARG;
  esy_hb_can_ctx_t *ctx = can_context(can);
  if (!can_ready(ctx) || esy_hb_esp32_is_in_isr() || ctx->stopped ||
      ctx->mode == ESY_CAN_MODE_LISTEN_ONLY) return ESY_ERR_STATE;
  twai_message_t message = {
      .identifier = frame->id,
      .data_length_code = frame->length,
      .extd = frame->extended,
      .rtr = frame->rtr,
      .ss = !ctx->auto_retransmit,
  };
  if (!frame->rtr) memcpy(message.data, frame->data, frame->length);
  const esp_err_t error = twai_transmit_v2(ctx->native, &message, 0);
  /* With no native TX queue, ESP_FAIL specifically means its hardware buffer
   * is occupied. Acceptance never borrows the caller's frame storage. */
  if (error == ESP_FAIL || error == ESP_ERR_TIMEOUT) return ESY_ERR_BUSY;
  return native_error(error);
}

static bool frame_matches(const esy_hb_can_ctx_t *ctx,
                           const esy_can_frame_t *frame) {
  for (size_t i = 0u; i < ctx->filter_count; ++i) {
    const esy_can_filter_t *filter = &ctx->filters[i];
    if (filter->extended == frame->extended &&
        (!filter->match_rtr || filter->rtr == frame->rtr) &&
        (frame->id & filter->mask) == (filter->id & filter->mask)) return true;
  }
  return false;
}

esy_err_t esy_hb_can_receive(esy_can_t *can, esy_can_frame_t *frame) {
  if (can == NULL || frame == NULL) return ESY_ERR_INVALID_ARG;
  esy_hb_can_ctx_t *ctx = can_context(can);
  if (!can_ready(ctx) || esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;
  twai_message_t message;
  const esp_err_t error = twai_receive_v2(ctx->native, &message, 0);
  if (error == ESP_ERR_TIMEOUT) return ESY_ERR_BUSY;
  if (error != ESP_OK) return native_error(error);
  esy_can_frame_t received = {
      .id = message.identifier,
      .length = message.data_length_code > ESY_CAN_CLASSIC_MAX_DATA
          ? ESY_CAN_CLASSIC_MAX_DATA : message.data_length_code,
      .extended = message.extd,
      .rtr = message.rtr,
  };
  if (received.id > (received.extended ? ESY_CAN_EXT_ID_MAX : ESY_CAN_STD_ID_MAX))
    return ESY_ERR_IO;
  if (!received.rtr) memcpy(received.data, message.data, received.length);
  /* Exactly one native frame is retired, even if rejected. A later call may
   * find an accepted frame behind it; BUSY is not proof that the raw queue is empty. */
  if (!frame_matches(ctx, &received)) return ESY_ERR_BUSY;
  *frame = received;
  return ESY_OK;
}

esy_err_t esy_hb_can_set_filters(esy_can_t *can,
                                const esy_can_filter_t *filters, size_t count) {
  if (can == NULL || count > ESY_CAN_ESP32_FILTER_CAPACITY ||
      (filters == NULL && count != 0u)) return ESY_ERR_INVALID_ARG;
  esy_hb_can_ctx_t *ctx = can_context(can);
  if (!can_ready(ctx) || esy_hb_esp32_is_in_isr() || !ctx->stopped)
    return ESY_ERR_STATE;
  for (size_t i = 0u; i < count; ++i) {
    const uint32_t limit = filters[i].extended
        ? ESY_CAN_EXT_ID_MAX : ESY_CAN_STD_ID_MAX;
    if (filters[i].id > limit || filters[i].mask > limit)
      return ESY_ERR_INVALID_ARG;
  }
  if (count != 0u) memcpy(ctx->filters, filters, count * sizeof(*filters));
  ctx->filter_count = count;
  return ESY_OK;
}

esy_err_t esy_hb_can_get_status(esy_can_t *can, esy_can_status_t *status) {
  if (can == NULL || status == NULL) return ESY_ERR_INVALID_ARG;
  esy_hb_can_ctx_t *ctx = can_context(can);
  if (ctx == NULL || esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;
  twai_status_info_t native;
  const esp_err_t error = twai_get_status_info_v2(ctx->native, &native);
  if (error != ESP_OK) return native_error(error);
  ctx->rx_overrun |= native.rx_missed_count != 0u || native.rx_overrun_count != 0u;
  esy_can_state_t state = ESY_CAN_STATE_FAULT;
  if (!ctx->faulted && !ctx->cleanup_pending) {
    if (ctx->stopped) state = ESY_CAN_STATE_STOPPED;
    else if (native.state == TWAI_STATE_BUS_OFF) state = ESY_CAN_STATE_BUS_OFF;
    else if (native.state == TWAI_STATE_RUNNING)
      state = native.tx_error_counter >= ESP32_CAN_ERROR_PASSIVE ||
              native.rx_error_counter >= ESP32_CAN_ERROR_PASSIVE
          ? ESY_CAN_STATE_ERROR_PASSIVE : ESY_CAN_STATE_ERROR_ACTIVE;
  }
  const esy_can_status_t sampled = {
      .state = state,
      .tx_error_count = (uint16_t)native.tx_error_counter,
      .rx_error_count = (uint16_t)native.rx_error_counter,
      .warning = native.tx_error_counter >= ESP32_CAN_ERROR_WARNING ||
                 native.rx_error_counter >= ESP32_CAN_ERROR_WARNING,
      .rx_overrun = ctx->rx_overrun,
      .tx_pending = ctx->stopped ? 0u : (uint8_t)native.msgs_to_tx,
      .rx_pending = (uint8_t)native.msgs_to_rx,
  };
  *status = sampled;
  return ESY_OK;
}

esy_err_t esy_hb_can_recover(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  if (!can_ready(can_context(can)) || esy_hb_esp32_is_in_isr())
    return ESY_ERR_STATE;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_can_get_caps(const esy_can_t *can, uint32_t *caps) {
  if (can == NULL || caps == NULL) return ESY_ERR_INVALID_ARG;
  if (!can_ready(can_context(can)) || esy_hb_esp32_is_in_isr())
    return ESY_ERR_STATE;
  *caps = ESY_CAN_CAP_LISTEN_ONLY | ESY_CAN_CAP_FILTER;
  return ESY_OK;
}
