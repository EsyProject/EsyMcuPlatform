/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_dma.h"
#include "esy_hal/esy_hal_dma_device.h"

#include <string.h>

#include "esy_board/esy_board.h"
#include "esy_hal_backend/esy_hb_dma.h"

static esy_dma_channel_runtime_t *dma_find_runtime(esy_dma_t *dma,
                                                   uint32_t dreq,
                                                   uint8_t channel) {
  if ((dma == NULL) || !dma->initialized) return NULL;
  for (uint32_t i = 0u; i < dma->channel_count; i++) {
    if ((dma->runtime[i].dreq == dreq) &&
        (dma->runtime[i].channel == channel)) {
      return &dma->runtime[i];
    }
  }
  return NULL;
}

static const esy_dma_channel_runtime_t *dma_find_runtime_const(
    const esy_dma_t *dma, uint32_t dreq, uint8_t channel) {
  return dma_find_runtime((esy_dma_t *)dma, dreq, channel);
}

enum { DMA_EVENT_DONE = 1u, DMA_EVENT_HALF = 2u, DMA_EVENT_ERROR = 4u };

static esy_err_t dma_validate_control(esy_dma_channel_runtime_t *runtime) {
  if (runtime->started ||
      atomic_load_explicit(&runtime->events, memory_order_acquire) != 0u ||
      esy_hb_dma_is_busy(runtime->dreq, runtime->channel)) return ESY_ERR_BUSY;
  if (!runtime->prepared) return ESY_ERR_STATE;
  return ESY_OK;
}

static void dma_clear_callbacks(esy_dma_channel_runtime_t *runtime) {
  runtime->complete_cb = NULL;
  runtime->complete_arg = NULL;
  runtime->half_cb = NULL;
  runtime->half_arg = NULL;
  runtime->error_cb = NULL;
  runtime->error_arg = NULL;
  runtime->circular = false;
  runtime->prepared = false;
  runtime->started = false;
  atomic_store_explicit(&runtime->events, 0u, memory_order_release);
}

static void dma_release_runtime(esy_dma_channel_runtime_t *runtime) {
  if ((runtime == NULL) || !runtime->ref_held) return;
  runtime->ref_held = false;
  (void)esy_device_put(&runtime->owner->device);
}

static void dma_complete(void *arg) {
  esy_dma_channel_runtime_t *runtime = arg;
  if (runtime != NULL)
    atomic_fetch_or_explicit(&runtime->events, DMA_EVENT_DONE, memory_order_release);
}

static void dma_half_complete(void *arg) {
  esy_dma_channel_runtime_t *runtime = arg;
  if (runtime != NULL)
    atomic_fetch_or_explicit(&runtime->events, DMA_EVENT_HALF, memory_order_release);
}

static void dma_error(void *arg) {
  esy_dma_channel_runtime_t *runtime = arg;
  if (runtime != NULL)
    atomic_fetch_or_explicit(&runtime->events, DMA_EVENT_ERROR, memory_order_release);
}

esy_err_t esy_hal_dma_init(esy_dma_t *dma,
                                const esy_dma_controller_config_t *config) {
  if ((dma == NULL) || (config == NULL) || (config->channels == NULL) ||
      (config->channel_count == 0u) ||
      (config->channel_count > ESY_HAL_DMA_MAX_TRACKED_CHANNELS)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (dma->initialized) return ESY_ERR_STATE;

  uint8_t resource_ids[ESY_HAL_DMA_MAX_TRACKED_CHANNELS];
  for (uint32_t i = 0u; i < config->channel_count; i++) {
    esy_err_t result = esy_hb_dma_get_resource_id(config->channels[i].dreq,
        config->channels[i].channel, &resource_ids[i]);
    if (result != ESY_OK) return result;
    if (resource_ids[i] >= 32u) return ESY_ERR_NOT_SUPPORTED;
    for (uint32_t j = 0u; j < i; j++) {
      if (resource_ids[i] == resource_ids[j]) {
        return ESY_ERR_INVALID_ARG;
      }
    }
  }

  memset(dma, 0, sizeof(*dma));
  dma->channels = config->channels;
  dma->channel_count = config->channel_count;
  dma->transition = config->transition;
  dma->transition_arg = config->transition_arg;
  for (uint32_t i = 0u; i < dma->channel_count; i++) {
    dma->runtime[i].owner = dma;
    dma->runtime[i].dreq = dma->channels[i].dreq;
    dma->runtime[i].channel = dma->channels[i].channel;
    dma->runtime[i].resource_id = resource_ids[i];
    atomic_init(&dma->runtime[i].events, 0u);
  }
  dma->initialized = true;
  return ESY_OK;
}

esy_err_t esy_hal_dma_deinit(esy_dma_t *dma) {
  if ((dma == NULL) || !dma->initialized) return ESY_ERR_INVALID_ARG;
  if (dma->device.registry != NULL) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < dma->channel_count; i++) {
    if (dma->runtime[i].ref_held || dma->runtime[i].prepared ||
        dma->runtime[i].started ||
        atomic_load_explicit(&dma->runtime[i].events,
                             memory_order_acquire) != 0u) {
      return ESY_ERR_BUSY;
    }
  }
  memset(dma, 0, sizeof(*dma));
  return ESY_OK;
}

static esy_err_t dma_device_pm_action(esy_device_t *device,
                                      esy_device_pm_action_t action,
                                      esy_power_state_t system_state) {
  esy_dma_t *dma;

  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  dma = (esy_dma_t *)device->driver_ctx;
  if ((action == ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND) ||
      (action == ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND)) {
    for (uint32_t i = 0u; i < dma->channel_count; i++) {
      if (dma->runtime[i].prepared || dma->runtime[i].started ||
          esy_hb_dma_is_busy(dma->channels[i].dreq,
                                 dma->channels[i].channel)) {
        return ESY_ERR_BUSY;
      }
    }
  }
  if (dma->transition == NULL) {
    return ESY_OK;
  }
  return dma->transition(dma, action, system_state, dma->transition_arg);
}

static bool dma_device_is_runtime_idle(const esy_device_t *device) {
  const esy_dma_t *dma;

  if ((device == NULL) || (device->driver_ctx == NULL)) return false;
  dma = device->driver_ctx;
  for (uint32_t i = 0u; i < dma->channel_count; i++) {
    if (dma->runtime[i].ref_held || dma->runtime[i].prepared ||
        dma->runtime[i].started ||
        esy_hb_dma_is_busy(dma->channels[i].dreq,
                               dma->channels[i].channel)) {
      return false;
    }
  }
  return true;
}

static const esy_device_ops_t k_dma_device_ops = {
    .pm_action = dma_device_pm_action,
    .is_runtime_idle = dma_device_is_runtime_idle,
};

esy_err_t esy_hal_dma_register_device(
    esy_dma_t *dma, esy_device_registry_t *registry,
    const esy_dma_device_config_t *config) {
  esy_device_config_t device_config;
  esy_err_t ret;

  if ((dma == NULL) || (registry == NULL) || (config == NULL) ||
      !dma->initialized || (dma->device.registry != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0u; i < dma->channel_count; ++i)
    if (dma->runtime[i].prepared || dma->runtime[i].started ||
        esy_hb_dma_is_busy(dma->runtime[i].dreq, dma->runtime[i].channel))
      return ESY_ERR_BUSY;

  device_config.name = config->device.name;
  device_config.ops = &k_dma_device_ops;
  device_config.driver_ctx = dma;
  device_config.dependencies = config->device.dependencies;
  device_config.dependency_count = config->device.dependency_count;
  device_config.supported_state_mask = config->device.supported_state_mask;
  ret = esy_device_register(registry, &dma->device, &device_config);
  return ret;
}

esy_err_t esy_hal_dma_unregister_device(esy_dma_t *dma) {
  esy_err_t ret;

  if ((dma == NULL) || !dma->initialized) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = dma->device.registry == NULL ? ESY_OK :
      esy_device_unregister(&dma->device);
  return ret;
}

void esy_hal_dma_default_cfg(esy_dma_config_t *config) {
  if (!config) return;

  memset(config, 0, sizeof(*config));
  config->callback = NULL;
  config->callback_arg = NULL;
  config->half_callback = NULL;
  config->half_callback_arg = NULL;
  config->error_callback = NULL;
  config->error_callback_arg = NULL;
  config->circular = false;
  config->transfer_size = ESY_DMA_TRANSFER_SIZE_8;
  config->src = NULL;
  config->dst = NULL;
}

esy_err_t esy_hal_dma_prepare(esy_dma_t *dma,
                                   esy_dma_config_t *config) {
  esy_dma_channel_runtime_t *runtime;
  esy_dma_config_t driver_config;
  esy_err_t ret;

  if ((dma == NULL) || !dma->initialized || (config == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  config->inner.irq_index =
      esy_board_dma_get_irq_index(config->inner.dreq, config->inner.channel);
  config->inner.irq_priority =
      esy_board_dma_get_irq_priority(config->inner.dreq, config->inner.channel);

  runtime = dma_find_runtime(dma, config->inner.dreq, config->inner.channel);
  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
  if (runtime->prepared || runtime->started) return ESY_ERR_BUSY;
  driver_config = *config;
  if (dma->device.registry != NULL) {
    if (runtime->ref_held) return ESY_ERR_BUSY;
    ret = esy_device_get(&dma->device);
    if (ret != ESY_OK) return ret;
    runtime->ref_held = true;
  }
  runtime->complete_cb = config->callback;
  runtime->complete_arg = config->callback_arg;
  runtime->half_cb = config->half_callback;
  runtime->half_arg = config->half_callback_arg;
  runtime->error_cb = config->error_callback;
  runtime->error_arg = config->error_callback_arg;
  runtime->circular = config->circular;
  atomic_store_explicit(&runtime->events, 0u, memory_order_release);
  driver_config.callback = dma_complete;
  driver_config.callback_arg = runtime;
  driver_config.half_callback = dma_half_complete;
  driver_config.half_callback_arg = runtime;
  driver_config.error_callback = dma_error;
  driver_config.error_callback_arg = runtime;

  ret = esy_hb_dma_prepare(dma, &driver_config);
  if (ret == ESY_OK) {
    runtime->prepared = true;
  } else {
    dma_clear_callbacks(runtime);
    dma_release_runtime(runtime);
  }
  return ret;
}

esy_err_t esy_hal_dma_start(esy_dma_t *dma, uint32_t route_mask) {
  uint32_t known_mask = 0u;
  uint32_t resource_mask = 0u;
  esy_err_t ret;

  if ((dma == NULL) || !dma->initialized || (route_mask == 0u)) {
    return ESY_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0u; i < dma->channel_count; i++) {
    uint32_t bit = 1u << i;

    known_mask |= bit;
    if (((route_mask & bit) != 0u) &&
        (!dma->runtime[i].prepared || dma->runtime[i].started)) {
      return ESY_ERR_STATE;
    }
    if ((route_mask & bit) != 0u)
      resource_mask |= 1u << dma->runtime[i].resource_id;
  }
  if ((route_mask & ~known_mask) != 0u) return ESY_ERR_INVALID_ARG;

  for (uint32_t i = 0u; i < dma->channel_count; i++) {
    if ((route_mask & (1u << i)) == 0u) continue;
    dma->runtime[i].prepared = false;
    dma->runtime[i].started = true;
  }
  ret = esy_hb_dma_start(dma, resource_mask);
  if (ret != ESY_OK) {
    for (uint32_t i = 0u; i < dma->channel_count; i++) {
      if ((route_mask & (1u << i)) != 0u) {
        dma->runtime[i].started = false;
        dma->runtime[i].prepared = true;
      }
    }
  }
  return ret;
}

esy_err_t esy_hal_dma_start_single(esy_dma_t *dma,
                                        esy_dma_config_t *config) {
  if (config == NULL) return ESY_ERR_INVALID_ARG;
  esy_dma_channel_runtime_t *runtime =
      dma_find_runtime(dma, config->inner.dreq, config->inner.channel);
  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t ret = esy_hal_dma_prepare(dma, config);
  if (ret != ESY_OK) return ret;
  uint32_t route_index = (uint32_t)(runtime - dma->runtime);
  return esy_hal_dma_start(dma, 1u << route_index);
}

esy_err_t esy_hal_dma_abort(esy_dma_t *dma, uint32_t dreq,
                                 uint8_t channel) {
  esy_dma_channel_runtime_t *runtime = dma_find_runtime(dma, dreq, channel);
  esy_err_t ret;

  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
  if (!runtime->prepared && !runtime->started &&
      atomic_load_explicit(&runtime->events, memory_order_acquire) == 0u) {
    return ESY_OK;
  }
  ret = esy_hb_dma_abort(dma, dreq, channel);
  if (ret == ESY_OK) {
    dma_clear_callbacks(runtime);
    dma_release_runtime(runtime);
  }
  return ret;
}

bool esy_hal_dma_is_busy(const esy_dma_t *dma, uint32_t dreq,
                              uint8_t channel) {
  const esy_dma_channel_runtime_t *runtime = dma_find_runtime_const(dma, dreq, channel);
  if (runtime == NULL) return false;
  return runtime->started ||
      atomic_load_explicit(&runtime->events, memory_order_acquire) != 0u ||
      esy_hb_dma_is_busy(dreq, channel);
}

esy_err_t esy_hal_dma_poll(esy_dma_t *dma) {
  if (dma == NULL || !dma->initialized) return ESY_ERR_INVALID_ARG;
  esy_err_t first_error = ESY_OK;
  for (uint32_t i = 0u; i < dma->channel_count; ++i) {
    esy_dma_channel_runtime_t *runtime = &dma->runtime[i];
    unsigned events = atomic_load_explicit(&runtime->events, memory_order_acquire);
    if ((events & DMA_EVENT_ERROR) ||
        (!runtime->circular && (events & DMA_EVENT_DONE))) {
      esy_err_t result = esy_hb_dma_abort(dma, runtime->dreq, runtime->channel);
      if (result != ESY_OK) {
        if (first_error == ESY_OK) first_error = result;
        continue;
      }
      events = atomic_load_explicit(&runtime->events, memory_order_acquire);
      esy_dma_callback_t callback = events & DMA_EVENT_ERROR
          ? runtime->error_cb : runtime->complete_cb;
      void *arg = events & DMA_EVENT_ERROR
          ? runtime->error_arg : runtime->complete_arg;
      dma_clear_callbacks(runtime);
      if (callback != NULL) callback(arg);
      dma_release_runtime(runtime);
      continue;
    }
    const unsigned notifications = DMA_EVENT_HALF |
        (runtime->circular ? DMA_EVENT_DONE : 0u);
    events = atomic_fetch_and_explicit(&runtime->events, ~notifications,
                                       memory_order_acq_rel);
    if (events & DMA_EVENT_ERROR) continue;
    if ((events & DMA_EVENT_HALF) && runtime->half_cb != NULL)
      runtime->half_cb(runtime->half_arg);
    if (runtime->circular && (events & DMA_EVENT_DONE) &&
        runtime->complete_cb != NULL) runtime->complete_cb(runtime->complete_arg);
  }
  return first_error;
}

esy_err_t esy_hal_dma_bind_irq_to_core(esy_dma_t *dma, uint8_t irq_index,
                                            uint8_t core_id) {
  if ((dma == NULL) || !dma->initialized) return ESY_ERR_INVALID_ARG;
  return esy_hb_dma_bind_irq_to_core(irq_index, core_id);
}

esy_err_t esy_hal_dma_set_read_addr(esy_dma_t *dma, uint32_t dreq,
                                         uint8_t channel, const void *addr,
                                         bool trigger_now) {
  esy_dma_channel_runtime_t *runtime = dma_find_runtime(dma, dreq, channel);
  esy_err_t ret;

  if ((runtime == NULL) || (addr == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = dma_validate_control(runtime);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_dma_set_read_addr(dma, dreq, channel, addr, trigger_now);
  if (ret == ESY_OK && trigger_now) {
    runtime->prepared = false;
    runtime->started = true;
  }
  return ret;
}

esy_err_t esy_hal_dma_set_write_addr(esy_dma_t *dma, uint32_t dreq,
                                          uint8_t channel, void *addr,
                                          bool trigger_now) {
  esy_dma_channel_runtime_t *runtime = dma_find_runtime(dma, dreq, channel);
  esy_err_t ret;

  if ((runtime == NULL) || (addr == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = dma_validate_control(runtime);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_dma_set_write_addr(dma, dreq, channel, addr, trigger_now);
  if (ret == ESY_OK && trigger_now) {
    runtime->prepared = false;
    runtime->started = true;
  }
  return ret;
}

esy_err_t esy_hal_dma_set_trans_count(esy_dma_t *dma, uint32_t dreq,
                                           uint8_t channel, size_t count,
                                           bool trigger_now) {
  esy_dma_channel_runtime_t *runtime = dma_find_runtime(dma, dreq, channel);
  esy_err_t ret;

  if ((runtime == NULL) || (count == 0u)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = dma_validate_control(runtime);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_dma_set_trans_count(dma, dreq, channel, count, trigger_now);
  if (ret == ESY_OK && trigger_now) {
    runtime->prepared = false;
    runtime->started = true;
  }
  return ret;
}

esy_err_t esy_hal_dma_get_trans_count_remaining(
    const esy_dma_t *dma, uint32_t dreq, uint8_t channel,
    size_t *out_remaining) {
  if ((dma_find_runtime_const(dma, dreq, channel) == NULL) ||
      (out_remaining == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  return esy_hb_dma_get_trans_count_remaining(dreq, channel, out_remaining);
}

esy_err_t esy_hal_dma_chain_to(esy_dma_t *dma, uint32_t dreq,
                                    uint8_t channel, uint8_t next_route_index) {
  if ((dma_find_runtime(dma, dreq, channel) == NULL) ||
      (next_route_index >= dma->channel_count)) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_err_t result = dma_validate_control(dma_find_runtime(dma, dreq, channel));
  if (result != ESY_OK) return result;
  result = dma_validate_control(&dma->runtime[next_route_index]);
  if (result != ESY_OK) return result;
  /* Native chaining needs transaction-wide ownership of the destination.
   * Until that lifecycle exists, reject it without arming hardware. */
  return ESY_ERR_NOT_SUPPORTED;
}

void esy_hal_dma_handle_irq(uint32_t dreq, uint8_t channel) {
  esy_hb_dma_handle_irq(dreq, channel);
}
