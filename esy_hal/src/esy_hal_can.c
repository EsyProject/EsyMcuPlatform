/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_can.h"
#include "esy_hal_backend/esy_hb_can.h"

#include <string.h>

static const esy_can_vtable_t k_can_ops = {
    .start = esy_hb_can_start,
    .stop = esy_hb_can_stop,
    .send = esy_hb_can_send,
    .receive = esy_hb_can_receive,
    .set_filters = esy_hb_can_set_filters,
    .get_status = esy_hb_can_get_status,
    .recover = esy_hb_can_recover,
    .get_caps = esy_hb_can_get_caps,
};

static bool timing_valid(const esy_can_timing_t *timing) {
  return timing->prescaler != 0u && timing->segment1 != 0u &&
         timing->segment2 != 0u && timing->sjw != 0u &&
         timing->sjw <= timing->segment2;
}

static bool identifier_valid(uint32_t id, bool extended) {
  return id <= (extended ? ESY_CAN_EXT_ID_MAX : ESY_CAN_STD_ID_MAX);
}

static bool frame_valid(const esy_can_frame_t *frame) {
  if (!identifier_valid(frame->id, frame->extended) || frame->esi ||
      (frame->brs && !frame->fd) || (frame->rtr && frame->fd)) {
    return false;
  }
  if (!frame->fd) return frame->length <= ESY_CAN_CLASSIC_MAX_DATA;
  return frame->length <= ESY_CAN_CLASSIC_MAX_DATA || frame->length == 12u ||
         frame->length == 16u || frame->length == 20u || frame->length == 24u ||
         frame->length == 32u || frame->length == 48u ||
         frame->length == ESY_CAN_FD_MAX_DATA;
}

static esy_err_t can_ready(const esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  if (can->ctx == NULL || can->ops == NULL) return ESY_ERR_STATE;
  return ESY_OK;
}

void esy_hal_can_default_cfg(esy_can_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->nominal.prescaler = 1u;
  config->nominal.segment1 = 13u;
  config->nominal.segment2 = 2u;
  config->nominal.sjw = 1u;
  config->auto_retransmit = true;
  config->poll_limit = ESY_CAN_DEFAULT_POLL_LIMIT;
}

esy_err_t esy_hal_can_init(esy_can_t *can, const esy_can_config_t *config) {
  if (can == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
  if (can->ctx != NULL || can->ops != NULL) return ESY_ERR_STATE;
  if (!timing_valid(&config->nominal) ||
      (config->fd && !timing_valid(&config->data)) ||
      (config->mode != ESY_CAN_MODE_NORMAL &&
       config->mode != ESY_CAN_MODE_LOOPBACK &&
       config->mode != ESY_CAN_MODE_LISTEN_ONLY) ||
      config->poll_limit == 0u || config->poll_limit > ESY_CAN_MAX_POLL_LIMIT) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_err_t result = esy_hb_can_init(can, config);
  if (result == ESY_OK) can->ops = &k_can_ops;
  return result;
}

esy_err_t esy_hal_can_deinit(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_can_deinit(can);
  if (result == ESY_OK) memset(can, 0, sizeof(*can));
  return result;
}

esy_err_t esy_hal_can_start(esy_can_t *can) {
  esy_err_t result = can_ready(can);
  if (result != ESY_OK) return result;
  if (can->ops->start == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return can->ops->start(can);
}

esy_err_t esy_hal_can_stop(esy_can_t *can) {
  esy_err_t result = can_ready(can);
  if (result != ESY_OK) return result;
  if (can->ops->stop == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return can->ops->stop(can);
}

esy_err_t esy_hal_can_send(esy_can_t *can, const esy_can_frame_t *frame) {
  if (frame == NULL || !frame_valid(frame)) return ESY_ERR_INVALID_ARG;
  esy_err_t result = can_ready(can);
  if (result != ESY_OK) return result;
  if (can->ops->send == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  if (frame->fd) {
    uint32_t caps;
    result = esy_hal_can_get_caps(can, &caps);
    if (result != ESY_OK) return result;
    if ((caps & ESY_CAN_CAP_FD) == 0u ||
        (frame->brs && (caps & ESY_CAN_CAP_BRS) == 0u)) {
      return ESY_ERR_NOT_SUPPORTED;
    }
  }
  return can->ops->send(can, frame);
}

esy_err_t esy_hal_can_receive(esy_can_t *can, esy_can_frame_t *frame) {
  if (frame == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = can_ready(can);
  if (result != ESY_OK) return result;
  if (can->ops->receive == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  esy_can_frame_t received = {0};
  result = can->ops->receive(can, &received);
  if (result == ESY_OK) *frame = received;
  return result;
}

esy_err_t esy_hal_can_set_filters(esy_can_t *can,
                                 const esy_can_filter_t *filters, size_t count) {
  if (count != 0u && filters == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = can_ready(can);
  if (result != ESY_OK) return result;
  if (can->ops->set_filters == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  for (size_t i = 0u; i < count; ++i) {
    if (!identifier_valid(filters[i].id, filters[i].extended) ||
        !identifier_valid(filters[i].mask, filters[i].extended)) {
      return ESY_ERR_INVALID_ARG;
    }
  }
  return can->ops->set_filters(can, filters, count);
}

esy_err_t esy_hal_can_get_status(esy_can_t *can, esy_can_status_t *status) {
  if (status == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = can_ready(can);
  if (result != ESY_OK) return result;
  if (can->ops->get_status == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  esy_can_status_t sampled = {0};
  result = can->ops->get_status(can, &sampled);
  if (result == ESY_OK) *status = sampled;
  return result;
}

esy_err_t esy_hal_can_recover(esy_can_t *can) {
  esy_err_t result = can_ready(can);
  if (result != ESY_OK) return result;
  if (can->ops->recover == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return can->ops->recover(can);
}

esy_err_t esy_hal_can_get_caps(const esy_can_t *can, uint32_t *caps) {
  if (caps == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = can_ready(can);
  if (result != ESY_OK) return result;
  if (can->ops->get_caps == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  uint32_t sampled = 0u;
  result = can->ops->get_caps(can, &sampled);
  if (result == ESY_OK) *caps = sampled;
  return result;
}
