/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_can_provider.h"
#include "esy_hal_backend/esy_hb_can.h"

typedef struct {
  fake_esy_hal_can_provider_t provider;
} fake_esy_hal_can_module_ctx_t;

static fake_esy_hal_can_module_ctx_t s_ctx;

static bool owns(const esy_can_t *can) {
  return can != NULL && can->ctx == &s_ctx.provider &&
         s_ctx.provider.owner == can;
}

static esy_err_t control_result(fake_esy_hal_can_hook_t hook) {
  esy_err_t result = s_ctx.provider.results[hook];
  if (result == ESY_ERR_TIMEOUT)
    s_ctx.provider.status.state = ESY_CAN_STATE_FAULT;
  return result;
}

void fake_esy_hal_can_provider_reset(void) {
  s_ctx = (fake_esy_hal_can_module_ctx_t){0};
  s_ctx.provider.caps = ESY_CAN_CAP_LOOPBACK | ESY_CAN_CAP_LISTEN_ONLY |
                        ESY_CAN_CAP_FILTER | ESY_CAN_CAP_RECOVERY;
  s_ctx.provider.results[FAKE_ESY_CAN_RECEIVE] = ESY_ERR_BUSY;
}

fake_esy_hal_can_provider_t *fake_esy_hal_can_provider_state(void) {
  return &s_ctx.provider;
}

esy_err_t esy_hb_can_init(esy_can_t *can, const esy_can_config_t *config) {
  if (can == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
  fake_esy_hal_can_provider_t *provider = &s_ctx.provider;
  ++provider->calls[FAKE_ESY_CAN_INIT];
  if (provider->owner != NULL) return ESY_ERR_BUSY;
  provider->config = *config;
  esy_err_t result = provider->results[FAKE_ESY_CAN_INIT];
  if (result == ESY_OK || provider->retain_failed_init) {
    provider->owner = can;
    can->ctx = provider;
    provider->status.state = result == ESY_OK ? ESY_CAN_STATE_STOPPED
                                              : ESY_CAN_STATE_FAULT;
  }
  return result;
}

esy_err_t esy_hb_can_deinit(esy_can_t *can) {
  if (can == NULL) return ESY_ERR_INVALID_ARG;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_DEINIT];
  if (!owns(can)) return can->ctx == NULL ? ESY_OK : ESY_ERR_STATE;
  esy_err_t result = control_result(FAKE_ESY_CAN_DEINIT);
  if (result == ESY_OK) {
    s_ctx.provider.owner = NULL;
    can->ctx = NULL;
  }
  return result;
}

esy_err_t esy_hb_can_start(esy_can_t *can) {
  if (!owns(can)) return ESY_ERR_STATE;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_START];
  if (s_ctx.provider.status.state != ESY_CAN_STATE_STOPPED) return ESY_ERR_STATE;
  esy_err_t result = control_result(FAKE_ESY_CAN_START);
  if (result != ESY_OK) return result;
  s_ctx.provider.status.state = ESY_CAN_STATE_ERROR_ACTIVE;
  return ESY_OK;
}

esy_err_t esy_hb_can_stop(esy_can_t *can) {
  if (!owns(can)) return ESY_ERR_STATE;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_STOP];
  if (s_ctx.provider.status.state == ESY_CAN_STATE_STOPPED) return ESY_OK;
  esy_err_t result = control_result(FAKE_ESY_CAN_STOP);
  if (result == ESY_OK) {
    s_ctx.provider.status.state = ESY_CAN_STATE_STOPPED;
    s_ctx.provider.status.tx_pending = 0u;
  }
  return result;
}

esy_err_t esy_hb_can_send(esy_can_t *can, const esy_can_frame_t *frame) {
  if (frame == NULL) return ESY_ERR_INVALID_ARG;
  if (!owns(can)) return ESY_ERR_STATE;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_SEND];
  if (s_ctx.provider.status.state != ESY_CAN_STATE_ERROR_ACTIVE &&
      s_ctx.provider.status.state != ESY_CAN_STATE_ERROR_PASSIVE)
    return ESY_ERR_STATE;
  if (s_ctx.provider.config.mode == ESY_CAN_MODE_LISTEN_ONLY)
    return ESY_ERR_NOT_SUPPORTED;
  if (frame->fd && !s_ctx.provider.config.fd) return ESY_ERR_NOT_SUPPORTED;
  esy_err_t result = s_ctx.provider.results[FAKE_ESY_CAN_SEND];
  if (result != ESY_OK) return result;
  s_ctx.provider.tx_frame = *frame;
  return ESY_OK;
}

esy_err_t esy_hb_can_receive(esy_can_t *can, esy_can_frame_t *frame) {
  if (frame == NULL) return ESY_ERR_INVALID_ARG;
  if (!owns(can) || s_ctx.provider.status.state == ESY_CAN_STATE_FAULT)
    return ESY_ERR_STATE;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_RECEIVE];
  *frame = s_ctx.provider.rx_frame;
  return s_ctx.provider.results[FAKE_ESY_CAN_RECEIVE];
}

esy_err_t esy_hb_can_set_filters(esy_can_t *can,
                                const esy_can_filter_t *filters, size_t count) {
  if (count > FAKE_ESY_CAN_FILTER_COUNT || (count != 0u && filters == NULL))
    return ESY_ERR_INVALID_ARG;
  if (!owns(can)) return ESY_ERR_STATE;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_FILTERS];
  if (s_ctx.provider.status.state != ESY_CAN_STATE_STOPPED) return ESY_ERR_STATE;
  esy_err_t result = s_ctx.provider.results[FAKE_ESY_CAN_FILTERS];
  if (result != ESY_OK) return result;
  for (size_t i = 0u; i < count; ++i) s_ctx.provider.filters[i] = filters[i];
  s_ctx.provider.filter_count = count;
  return ESY_OK;
}

esy_err_t esy_hb_can_get_status(esy_can_t *can, esy_can_status_t *status) {
  if (status == NULL) return ESY_ERR_INVALID_ARG;
  if (!owns(can)) return ESY_ERR_STATE;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_STATUS];
  *status = s_ctx.provider.status;
  return s_ctx.provider.results[FAKE_ESY_CAN_STATUS];
}

esy_err_t esy_hb_can_recover(esy_can_t *can) {
  if (!owns(can)) return ESY_ERR_STATE;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_RECOVER];
  if (s_ctx.provider.status.state == ESY_CAN_STATE_RECOVERING) return ESY_ERR_BUSY;
  if (s_ctx.provider.status.state != ESY_CAN_STATE_BUS_OFF) return ESY_ERR_STATE;
  esy_err_t result = control_result(FAKE_ESY_CAN_RECOVER);
  if (result != ESY_OK) return result;
  s_ctx.provider.status.state = ESY_CAN_STATE_RECOVERING;
  return ESY_OK;
}

esy_err_t esy_hb_can_get_caps(const esy_can_t *can, uint32_t *caps) {
  if (caps == NULL) return ESY_ERR_INVALID_ARG;
  if (!owns(can)) return ESY_ERR_STATE;
  ++s_ctx.provider.calls[FAKE_ESY_CAN_CAPS];
  *caps = s_ctx.provider.caps;
  return s_ctx.provider.results[FAKE_ESY_CAN_CAPS];
}
