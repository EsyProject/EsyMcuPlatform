/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_dma.h"

#include <string.h>

#define FAKE_DMA_MAX_ROUTES 32u

enum { FAKE_DMA_DONE = 1u, FAKE_DMA_ERROR = 4u };

typedef struct {
  esy_dma_t *owner;
  size_t remaining;
  size_t reload_count;
  uint8_t route_index;
  size_t unit_bytes;
} fake_dma_route_t;

typedef struct {
  fake_dma_route_t routes[FAKE_DMA_MAX_ROUTES];
  esy_err_t prepare_result;
  esy_err_t start_result;
  esy_err_t start_single_result;
  esy_err_t abort_result;
  esy_err_t control_result;
} fake_esy_hal_dma_ctx_t;

/* Test-owned, single-threaded state. Runtime flags/callbacks belong to the
 * caller's handle; this bounded table stores only fake counters and widths. */
static fake_esy_hal_dma_ctx_t s_ctx;

static fake_dma_route_t *find_route(const esy_dma_t *dma, uint32_t dreq,
                                    uint8_t channel) {
  if (dma == NULL || !dma->initialized) return NULL;
  for (size_t i = 0u; i < FAKE_DMA_MAX_ROUTES; ++i) {
    fake_dma_route_t *route = &s_ctx.routes[i];
    if (route->owner != dma) continue;
    const esy_dma_channel_runtime_t *runtime =
        &dma->runtime[route->route_index];
    if (runtime->dreq == dreq && runtime->channel == channel) return route;
  }
  return NULL;
}

static fake_dma_route_t *find_route_index(esy_dma_t *dma, uint8_t index) {
  if (dma == NULL || !dma->initialized || index >= dma->channel_count)
    return NULL;
  return find_route(dma, dma->runtime[index].dreq, dma->runtime[index].channel);
}

static void clear_transfer(esy_dma_channel_runtime_t *runtime) {
  runtime->complete_cb = NULL;
  runtime->complete_arg = NULL;
  runtime->half_cb = NULL;
  runtime->half_arg = NULL;
  runtime->error_cb = NULL;
  runtime->error_arg = NULL;
  runtime->circular = false;
  runtime->prepared = false;
  runtime->started = false;
  atomic_store(&runtime->events, 0u);
}

static esy_err_t trigger_route(esy_dma_channel_runtime_t *runtime,
                                bool trigger_now) {
  if (runtime->started || atomic_load(&runtime->events) != 0u) return ESY_ERR_BUSY;
  if (!runtime->prepared) return ESY_ERR_STATE;
  if (s_ctx.control_result != ESY_OK) return s_ctx.control_result;
  if (trigger_now) {
    runtime->prepared = false;
    runtime->started = true;
  }
  return ESY_OK;
}

esy_err_t esy_hal_dma_init(esy_dma_t *dma,
                          const esy_dma_controller_config_t *config) {
  if (dma == NULL || config == NULL || config->channels == NULL ||
      config->channel_count == 0u ||
      config->channel_count > ESY_HAL_DMA_MAX_TRACKED_CHANNELS)
    return ESY_ERR_INVALID_ARG;
  if (dma->initialized) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < config->channel_count; ++i) {
    for (uint32_t j = 0u; j < i; ++j) {
      if (config->channels[i].dreq == config->channels[j].dreq &&
          config->channels[i].channel == config->channels[j].channel)
        return ESY_ERR_INVALID_ARG;
    }
  }
  size_t free_routes = 0u;
  for (size_t i = 0u; i < FAKE_DMA_MAX_ROUTES; ++i) {
    if (s_ctx.routes[i].owner == dma) return ESY_ERR_STATE;
    if (s_ctx.routes[i].owner == NULL) ++free_routes;
  }
  if (free_routes < config->channel_count) return ESY_ERR_NO_MEM;

  memset(dma, 0, sizeof(*dma));
  dma->channels = config->channels;
  dma->channel_count = config->channel_count;
  dma->transition = config->transition;
  dma->transition_arg = config->transition_arg;
  uint8_t index = 0u;
  for (size_t i = 0u; i < FAKE_DMA_MAX_ROUTES && index < dma->channel_count; ++i) {
    if (s_ctx.routes[i].owner != NULL) continue;
    s_ctx.routes[i] = (fake_dma_route_t){.owner = dma, .route_index = index};
    dma->runtime[index].owner = dma;
    dma->runtime[index].dreq = config->channels[index].dreq;
    dma->runtime[index].channel = config->channels[index].channel;
    dma->runtime[index].resource_id = index;
    atomic_init(&dma->runtime[index].events, 0u);
    ++index;
  }
  dma->initialized = true;
  return ESY_OK;
}

esy_err_t esy_hal_dma_deinit(esy_dma_t *dma) {
  if (dma == NULL || !dma->initialized) return ESY_ERR_INVALID_ARG;
  if (dma->device.registry != NULL) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < dma->channel_count; ++i) {
    if (dma->runtime[i].prepared || dma->runtime[i].started)
      return ESY_ERR_BUSY;
  }
  for (size_t i = 0u; i < FAKE_DMA_MAX_ROUTES; ++i) {
    if (s_ctx.routes[i].owner == dma)
      memset(&s_ctx.routes[i], 0, sizeof(s_ctx.routes[i]));
  }
  memset(dma, 0, sizeof(*dma));
  return ESY_OK;
}

void fake_esy_hal_dma_reset(void) { memset(&s_ctx, 0, sizeof(s_ctx)); }

void fake_esy_hal_dma_set_prepare_result(esy_err_t ret) {
  s_ctx.prepare_result = ret;
}

void fake_esy_hal_dma_set_start_result(esy_err_t ret) {
  s_ctx.start_result = ret;
}

void fake_esy_hal_dma_set_start_single_result(esy_err_t ret) {
  s_ctx.start_single_result = ret;
}

esy_err_t fake_esy_hal_dma_set_remaining(esy_dma_t *dma, uint32_t dreq,
                                         uint8_t channel, size_t remaining) {
  fake_dma_route_t *route = find_route(dma, dreq, channel);
  if (route == NULL) return ESY_ERR_INVALID_ARG;
  route->remaining = remaining;
  return ESY_OK;
}

void esy_hal_dma_default_cfg(esy_dma_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->transfer_size = ESY_DMA_TRANSFER_SIZE_8;
}

esy_err_t esy_hal_dma_prepare(esy_dma_t *dma, esy_dma_config_t *config) {
  if (config == NULL) return ESY_ERR_INVALID_ARG;
  fake_dma_route_t *route = find_route(dma, config->inner.dreq,
                                      config->inner.channel);
  if (route == NULL) return ESY_ERR_INVALID_ARG;
  size_t unit_bytes;
  switch (config->transfer_size) {
    case ESY_DMA_TRANSFER_SIZE_8: unit_bytes = sizeof(uint8_t); break;
    case ESY_DMA_TRANSFER_SIZE_16: unit_bytes = sizeof(uint16_t); break;
    case ESY_DMA_TRANSFER_SIZE_32: unit_bytes = sizeof(uint32_t); break;
    default: return ESY_ERR_INVALID_ARG;
  }
  if (config->length == 0u || config->length % unit_bytes != 0u)
    return ESY_ERR_INVALID_ARG;
  esy_dma_channel_runtime_t *runtime = &dma->runtime[route->route_index];
  if (runtime->prepared || runtime->started) return ESY_ERR_BUSY;
  if (s_ctx.prepare_result != ESY_OK) return s_ctx.prepare_result;
  route->remaining = config->length / unit_bytes;
  route->reload_count = route->remaining;
  route->unit_bytes = unit_bytes;
  runtime->complete_cb = config->callback;
  runtime->complete_arg = config->callback_arg;
  runtime->half_cb = config->half_callback;
  runtime->half_arg = config->half_callback_arg;
  runtime->error_cb = config->error_callback;
  runtime->error_arg = config->error_callback_arg;
  runtime->circular = config->circular;
  runtime->prepared = true;
  return ESY_OK;
}

esy_err_t esy_hal_dma_start(esy_dma_t *dma, uint32_t route_mask) {
  if (dma == NULL || !dma->initialized || route_mask == 0u ||
      (route_mask >> dma->channel_count) != 0u) return ESY_ERR_INVALID_ARG;
  for (uint8_t i = 0u; i < dma->channel_count; ++i) {
    if ((route_mask & (1u << i)) == 0u) continue;
    if (find_route_index(dma, i) == NULL) return ESY_ERR_STATE;
    if (!dma->runtime[i].prepared || dma->runtime[i].started)
      return ESY_ERR_STATE;
  }
  for (uint8_t i = 0u; i < dma->channel_count; ++i) {
    if ((route_mask & (1u << i)) == 0u) continue;
    if (s_ctx.start_result == ESY_OK) {
      dma->runtime[i].prepared = false;
      dma->runtime[i].started = true;
    }
  }
  return s_ctx.start_result;
}

esy_err_t esy_hal_dma_start_single(esy_dma_t *dma,
                                  esy_dma_config_t *transfer) {
  if (transfer == NULL) return ESY_ERR_INVALID_ARG;
  fake_dma_route_t *route = find_route(dma, transfer->inner.dreq,
                                      transfer->inner.channel);
  if (route == NULL) return ESY_ERR_INVALID_ARG;
  if (s_ctx.start_single_result != ESY_OK) return s_ctx.start_single_result;
  esy_err_t result = esy_hal_dma_prepare(dma, transfer);
  if (result != ESY_OK) return result;
  return esy_hal_dma_start(dma, 1u << route->route_index);
}

esy_err_t esy_hal_dma_abort(esy_dma_t *dma, uint32_t dreq, uint8_t channel) {
  fake_dma_route_t *route = find_route(dma, dreq, channel);
  if (route == NULL) return ESY_ERR_INVALID_ARG;
  if (s_ctx.abort_result != ESY_OK) return s_ctx.abort_result;
  clear_transfer(&dma->runtime[route->route_index]);
  return ESY_OK;
}

bool esy_hal_dma_is_busy(const esy_dma_t *dma, uint32_t dreq, uint8_t channel) {
  fake_dma_route_t *route = find_route(dma, dreq, channel);
  return route != NULL && dma->runtime[route->route_index].started;
}

esy_err_t esy_hal_dma_bind_irq_to_core(esy_dma_t *dma, uint8_t irq_index,
                                     uint8_t core_id) {
  if (dma == NULL || !dma->initialized) return ESY_ERR_INVALID_ARG;
  (void)irq_index;
  (void)core_id;
  return ESY_OK;
}

esy_err_t esy_hal_dma_set_read_addr(esy_dma_t *dma, uint32_t dreq,
                                  uint8_t channel, const void *addr,
                                  bool trigger_now) {
  fake_dma_route_t *route = find_route(dma, dreq, channel);
  if (route == NULL || addr == NULL ||
      (route->unit_bytes != 0u && (uintptr_t)addr % route->unit_bytes != 0u))
    return ESY_ERR_INVALID_ARG;
  return trigger_route(&dma->runtime[route->route_index], trigger_now);
}

esy_err_t esy_hal_dma_set_write_addr(esy_dma_t *dma, uint32_t dreq,
                                   uint8_t channel, void *addr,
                                   bool trigger_now) {
  fake_dma_route_t *route = find_route(dma, dreq, channel);
  if (route == NULL || addr == NULL ||
      (route->unit_bytes != 0u && (uintptr_t)addr % route->unit_bytes != 0u))
    return ESY_ERR_INVALID_ARG;
  return trigger_route(&dma->runtime[route->route_index], trigger_now);
}

esy_err_t esy_hal_dma_set_trans_count(esy_dma_t *dma, uint32_t dreq,
                                    uint8_t channel, size_t count,
                                    bool trigger_now) {
  fake_dma_route_t *route = find_route(dma, dreq, channel);
  if (route == NULL || count == 0u) return ESY_ERR_INVALID_ARG;
  esy_err_t result = trigger_route(&dma->runtime[route->route_index], trigger_now);
  if (result == ESY_OK) {
    route->remaining = count;
    route->reload_count = count;
  }
  return result;
}

esy_err_t esy_hal_dma_get_trans_count_remaining(const esy_dma_t *dma,
    uint32_t dreq, uint8_t channel, size_t *out_remaining) {
  fake_dma_route_t *route = find_route(dma, dreq, channel);
  if (route == NULL || out_remaining == NULL) return ESY_ERR_INVALID_ARG;
  *out_remaining = route->remaining;
  return ESY_OK;
}

esy_err_t esy_hal_dma_chain_to(esy_dma_t *dma, uint32_t dreq, uint8_t channel,
                             uint8_t next_route_index) {
  fake_dma_route_t *source = find_route(dma, dreq, channel);
  fake_dma_route_t *target = find_route_index(dma, next_route_index);
  if (source == NULL || target == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = trigger_route(&dma->runtime[source->route_index], false);
  if (result != ESY_OK) return result;
  result = trigger_route(&dma->runtime[target->route_index], false);
  return result == ESY_OK ? ESY_ERR_NOT_SUPPORTED : result;
}

esy_err_t esy_hal_dma_poll(esy_dma_t *dma) {
  if (dma == NULL || !dma->initialized) return ESY_ERR_INVALID_ARG;
  esy_err_t first_error = ESY_OK;
  for (uint8_t i = 0u; i < dma->channel_count; ++i) {
    esy_dma_channel_runtime_t *runtime = &dma->runtime[i];
    uint32_t events = atomic_load(&runtime->events);
    if (events == 0u) continue;
    bool terminal = (events & FAKE_DMA_ERROR) || !runtime->circular;
    if (terminal && s_ctx.abort_result != ESY_OK) {
      first_error = s_ctx.abort_result;
      continue;
    }
    esy_dma_callback_t callback = events & FAKE_DMA_ERROR
        ? runtime->error_cb : runtime->complete_cb;
    void *arg = events & FAKE_DMA_ERROR
        ? runtime->error_arg : runtime->complete_arg;
    if (terminal) clear_transfer(runtime);
    else atomic_store(&runtime->events, 0u);
    if (callback != NULL) callback(arg);
  }
  return first_error;
}

void fake_esy_hal_dma_set_abort_result(esy_err_t result) { s_ctx.abort_result = result; }
void fake_esy_hal_dma_set_control_result(esy_err_t result) { s_ctx.control_result = result; }

esy_err_t fake_esy_hal_dma_signal_error(esy_dma_t *dma, uint32_t dreq,
                                       uint8_t channel) {
  fake_dma_route_t *route = find_route(dma, dreq, channel);
  if (route == NULL) return ESY_ERR_INVALID_ARG;
  esy_dma_channel_runtime_t *runtime = &dma->runtime[route->route_index];
  if (!runtime->started) return ESY_ERR_STATE;
  atomic_fetch_or(&runtime->events, FAKE_DMA_ERROR);
  return ESY_OK;
}

void esy_hal_dma_handle_irq(uint32_t dreq, uint8_t channel) {
  for (size_t i = 0u; i < FAKE_DMA_MAX_ROUTES; ++i) {
    fake_dma_route_t *route = &s_ctx.routes[i];
    if (route->owner == NULL) continue;
    esy_dma_t *dma = route->owner;
    esy_dma_channel_runtime_t *runtime = &dma->runtime[route->route_index];
    if (!runtime->started || runtime->dreq != dreq || runtime->channel != channel)
      continue;
    route->remaining = runtime->circular ? route->reload_count : 0u;
    atomic_fetch_or(&runtime->events, FAKE_DMA_DONE);
  }
}
