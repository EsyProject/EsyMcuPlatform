/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_adc.h"
#include "esy_hal/esy_hal_adc_device.h"
#include "esy_hal_adc_internal.h"

#include <string.h>

#include "esy_hal_backend/esy_hb_adc.h" /* backend: esy_hb_adc_xxx */
#include "esy_board/esy_board.h"

static esy_err_t adc_acquire(esy_adc_t *adc) {
  esy_hal_adc_runtime_t *runtime = esy_hal_adc_runtime(adc);
  return (runtime->device == NULL) ? ESY_OK : esy_device_get(runtime->device);
}

static void adc_release(esy_adc_t *adc) {
  esy_hal_adc_runtime_t *runtime = esy_hal_adc_runtime(adc);
  if (runtime->device != NULL) {
    (void)esy_device_put(runtime->device);
  }
}

static esy_adc_device_binding_t *adc_binding(esy_adc_t *adc) {
  esy_hal_adc_runtime_t *runtime = esy_hal_adc_runtime(adc);
  if ((adc == NULL) || (runtime->device == NULL)) return NULL;
  return (esy_adc_device_binding_t *)runtime->device->driver_ctx;
}

static bool adc_claim_dma_reference(esy_adc_device_binding_t *binding) {
  return atomic_exchange_explicit(&binding->dma_ref_held, 0u,
                                   memory_order_acq_rel) != 0u;
}

static void adc_dma_complete(void *arg) {
  esy_adc_device_binding_t *binding = arg;
  if ((binding == NULL) || !binding->dma_ref_held) return;
  esy_dma_callback_t callback = binding->complete_cb;
  void *callback_arg = binding->complete_arg;

  if (binding->dma_circular) {
    if (callback != NULL) callback(callback_arg);
    return;
  }
  if (!adc_claim_dma_reference(binding)) return;

  binding->complete_cb = NULL;
  binding->complete_arg = NULL;
  binding->error_cb = NULL;
  binding->error_arg = NULL;
  binding->dma_ref_held = false;
  binding->dma_circular = false;
  if (callback != NULL) callback(callback_arg);
  (void)esy_device_put_from_isr(&binding->device);
}

static void adc_dma_error(void *arg) {
  esy_adc_device_binding_t *binding = arg;
  if ((binding == NULL) || !binding->dma_ref_held) return;
  if (!adc_claim_dma_reference(binding)) return;
  esy_dma_callback_t callback = binding->error_cb;
  void *callback_arg = binding->error_arg;

  binding->complete_cb = NULL;
  binding->complete_arg = NULL;
  binding->error_cb = NULL;
  binding->error_arg = NULL;
  binding->dma_ref_held = false;
  binding->dma_circular = false;
  if (callback != NULL) callback(callback_arg);
  (void)esy_device_put_from_isr(&binding->device);
}

static void adc_prepare_dma_binding(esy_adc_device_binding_t *binding,
                                    esy_dma_config_t *dma_cfg) {
  binding->complete_cb = dma_cfg->callback;
  binding->complete_arg = dma_cfg->callback_arg;
  binding->error_cb = dma_cfg->error_callback;
  binding->error_arg = dma_cfg->error_callback_arg;
  binding->dma_circular = dma_cfg->circular;
  atomic_store_explicit(&binding->dma_ref_held, 1u, memory_order_release);
  dma_cfg->callback = adc_dma_complete;
  dma_cfg->callback_arg = binding;
  dma_cfg->error_callback = adc_dma_error;
  dma_cfg->error_callback_arg = binding;
}

static void adc_cancel_dma_binding(esy_adc_device_binding_t *binding) {
  binding->complete_cb = NULL;
  binding->complete_arg = NULL;
  binding->error_cb = NULL;
  binding->error_arg = NULL;
  binding->dma_ref_held = false;
  binding->dma_circular = false;
}

/* ================= vt_*: sole ops implementation =================
 * Notes:
 * - HAL public APIs call the shared ADC operations table; board and index
 *   completion stay in the vt_* functions.
 * - vt_* functions perform required board work, DMA metadata completion, and
 *   HAL backend forwarding.
 */

/* ---- basic sampling ---- */
static int32_t vt_sample_once(esy_adc_t *adc) {
  return esy_hb_adc_sample_once(adc);
}

/* ---- single-channel DMA one-shot---- */
static esy_err_t vt_transfer_dma(esy_adc_t *adc, esy_dma_config_t *dma_cfg) {
  if (!adc || !dma_cfg) return ESY_ERR_INVALID_ARG;

  uint8_t idx;
  esy_err_t r = esy_hb_adc_get_phys_index(adc, &idx);
  if (r != ESY_OK) return r;

  dma_cfg->transfer_size = ESY_DMA_TRANSFER_SIZE_16;
  if (!esy_board_adc_get_dma_info(idx, &dma_cfg->inner))
    return ESY_ERR_NOT_SUPPORTED;

  return esy_hb_adc_transfer_dma(adc, dma_cfg);
}

/* ---- continuous DMA ---- */
static esy_err_t vt_start_continuous_dma(esy_adc_t *adc,
                                         esy_dma_config_t *dma_cfg) {
  if (!adc || !dma_cfg) return ESY_ERR_INVALID_ARG;

  uint8_t idx;
  esy_err_t r = esy_hb_adc_get_phys_index(adc, &idx);
  if (r != ESY_OK) return r;

  dma_cfg->transfer_size = ESY_DMA_TRANSFER_SIZE_16;
  if (!esy_board_adc_get_dma_info(idx, &dma_cfg->inner))
    return ESY_ERR_NOT_SUPPORTED;

  return esy_hb_adc_start_continuous_dma(adc, dma_cfg);
}

/* ---- multi-channel DMA scan one-shot---- */
static esy_err_t vt_transfer_dma_multi_channel(esy_adc_t *adc,
                                               esy_dma_config_t *dma_cfg,
                                               const uint8_t *index_list,
                                               uint8_t num_indexes) {
  if (!adc || !dma_cfg || !index_list || num_indexes == 0)
    return ESY_ERR_INVALID_ARG;

  uint8_t idx;
  esy_err_t r = esy_hb_adc_get_phys_index(adc, &idx);
  if (r != ESY_OK) return r;

  dma_cfg->transfer_size = ESY_DMA_TRANSFER_SIZE_16;
  if (!esy_board_adc_get_dma_info(idx, &dma_cfg->inner))
    return ESY_ERR_NOT_SUPPORTED;

  return esy_hb_adc_transfer_dma_multi_channel(adc, dma_cfg, index_list,
                                                   num_indexes);
}

/* ---- DMA busy query ---- */
static bool vt_is_dma_busy(const esy_adc_t *adc) {
  return esy_hb_adc_is_dma_busy(adc);
}

static esy_err_t vt_abort_dma(esy_adc_t *adc) {
  return esy_hb_adc_abort_dma(adc);
}

/* ---- trigger source setup ---- */
static esy_err_t vt_set_trigger_source(esy_adc_t *adc, esy_adc_trigger_edge_t edge,
                                       esy_adc_trigger_source_t source) {
  return esy_hb_adc_set_trigger_source(adc, edge, source);
}

/* ---- queries ---- */
static esy_err_t vt_get_caps(const esy_adc_t *adc, uint32_t *out_caps) {
  return esy_hb_adc_get_caps(adc, out_caps);
}

static esy_err_t vt_get_phys_index(const esy_adc_t *adc, uint8_t *out_index) {
  return esy_hb_adc_get_phys_index(adc, out_index);
}

/* ================= helpers ================= */

static const esy_adc_vtable_t k_adc_ops = {
    .sample_once = vt_sample_once,
    .transfer_dma = vt_transfer_dma,
    .is_dma_busy = vt_is_dma_busy,
    .transfer_dma_multi_channel = vt_transfer_dma_multi_channel,
    .start_continuous_dma = vt_start_continuous_dma,
    .abort_dma = vt_abort_dma,
    .set_trigger_source = vt_set_trigger_source,
    .get_caps = vt_get_caps,
    .get_phys_index = vt_get_phys_index,
};

static esy_err_t adc_hardware_init(esy_adc_t *adc,
                                   const esy_adc_config_t *cfg) {
  esy_hal_adc_runtime_t *runtime = esy_hal_adc_runtime(adc);
  esy_adc_config_t local = *cfg;
  esy_err_t ret = esy_hb_adc_check_available(local.phys_index);
  if (ret != ESY_OK) return ret;
  esy_board_adc_configure_pin(local.phys_index, &local.inner);
  adc->ops = &k_adc_ops;
  ret = esy_hb_adc_init(adc, &local);
  if (ret == ESY_OK)
    ret = esy_hb_adc_set_trigger_source(adc, runtime->trigger_edge,
                                        runtime->trigger_source);
  if (ret != ESY_OK) {
    esy_err_t cleanup = esy_hb_adc_deinit(adc);
    if (cleanup != ESY_OK) return cleanup;
    if (ret != ESY_ERR_BUSY) esy_board_adc_reset_pin(local.phys_index);
    adc->ops = NULL;
  }
  return ret;
}

static esy_err_t adc_hardware_deinit(esy_adc_t *adc) {
  uint8_t index = ESY_PIN_UNUSED;
  bool have_index = esy_hb_adc_get_phys_index(adc, &index) == ESY_OK;
  esy_err_t result = esy_hb_adc_deinit(adc);
  if (result == ESY_OK && have_index) esy_board_adc_reset_pin(index);
  return result;
}

static esy_err_t adc_device_pm_action(esy_device_t *device,
                                      esy_device_pm_action_t action,
                                      esy_power_state_t system_state) {
  esy_adc_device_binding_t *binding;

  (void)system_state;
  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_adc_device_binding_t *)device->driver_ctx;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (binding->suspended) return ESY_ERR_STATE;
      if (vt_is_dma_busy(binding->adc)) return ESY_ERR_BUSY;
      /* A failed stop/deinit may have changed the hardware already. The graph
       * must fault this unknown state instead of treating it as a rollback. */
      if (adc_hardware_deinit(binding->adc) != ESY_OK) return ESY_ERR_STATE;
      binding->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!binding->suspended) return ESY_ERR_STATE;
      if (adc_hardware_init(binding->adc, &binding->resume_config) != ESY_OK) {
        return ESY_ERR_FAIL;
      }
      binding->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static const esy_device_ops_t k_adc_device_ops = {
    .pm_action = adc_device_pm_action,
};

/* ================= default configuration ================= */

extern void esy_hal_adc_default_cfg(esy_adc_config_t *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->sample_time = ESY_ADC_SAMPLETIME_15CYCLES;
}

/* ================= lifecycle ================= */

extern esy_err_t esy_hal_adc_init(esy_adc_t *adc,
                                       const esy_adc_config_t *cfg) {
  esy_hal_adc_runtime_t *runtime = esy_hal_adc_runtime(adc);
  if (!adc || !cfg) return ESY_ERR_INVALID_ARG;
  if (runtime->device != NULL || adc->ops != NULL)
    return ESY_ERR_STATE;
  return adc_hardware_init(adc, cfg);
}

extern esy_err_t esy_hal_adc_deinit(esy_adc_t *adc) {
  esy_hal_adc_runtime_t *runtime = esy_hal_adc_runtime(adc);
  if (adc == NULL) return ESY_ERR_INVALID_ARG;
  if (runtime->device != NULL) return ESY_ERR_STATE;
  if (adc->ctx == NULL && adc->ops == NULL) return ESY_OK;
  esy_err_t result = esy_hal_adc_abort_dma(adc);
  if (result != ESY_OK) return result;
  result = adc_hardware_deinit(adc);
  if (result != ESY_OK) return result;
  memset(adc, 0, sizeof(*adc));
  return ESY_OK;
}

esy_err_t esy_hal_adc_abort_dma(esy_adc_t *adc) {
  if (adc == NULL) return ESY_ERR_INVALID_ARG;
  if (adc->ops == NULL || adc->ops->abort_dma == NULL)
    return adc->ctx == NULL ? ESY_OK : ESY_ERR_NOT_SUPPORTED;
  esy_err_t result = adc->ops->abort_dma(adc);
  esy_adc_device_binding_t *binding = adc_binding(adc);
  if (result == ESY_OK && binding != NULL && adc_claim_dma_reference(binding)) {
    adc_cancel_dma_binding(binding);
    adc_release(adc);
  }
  return result;
}

esy_err_t esy_hal_adc_register_device(
    esy_adc_device_binding_t *binding, esy_adc_t *adc,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_adc_config_t *resume_config) {
  esy_hal_adc_runtime_t *runtime = esy_hal_adc_runtime(adc);
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (adc == NULL) || (registry == NULL) ||
      (device_config == NULL) || (resume_config == NULL) ||
      (runtime->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->adc != NULL) return ESY_ERR_STATE;
  if (vt_is_dma_busy(adc)) return ESY_ERR_BUSY;
  memset(binding, 0, sizeof(*binding));
  binding->adc = adc;
  binding->resume_config = *resume_config;
  config.name = device_config->name;
  config.ops = &k_adc_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->dependencies;
  config.dependency_count = device_config->dependency_count;
  config.supported_state_mask = device_config->supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    runtime->device = &binding->device;
  } else {
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

esy_err_t esy_hal_adc_unregister_device(
    esy_adc_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->adc == NULL) ||
      (esy_hal_adc_runtime(binding->adc)->device != &binding->device) ||
      binding->dma_ref_held ||
      vt_is_dma_busy(binding->adc)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    esy_hal_adc_runtime(binding->adc)->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

/* ================= sampling wrappers ================= */

extern int32_t esy_hal_adc_sample_once(esy_adc_t *adc) {
  int32_t ret;

  if (!adc) return ESY_ERR_INVALID_ARG;
  if (adc->ops == NULL || adc->ops->sample_once == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = adc_acquire(adc);
  if (ret != ESY_OK) return ret;
  ret = adc->ops->sample_once(adc);
  adc_release(adc);
  return ret;
}

/* ================= DMA one-shot single-channel wrappers ================= */

extern esy_err_t esy_hal_adc_start_dma(esy_adc_t *adc,
                                            esy_dma_config_t *dma_cfg) {
  esy_adc_device_binding_t *binding;
  esy_err_t ret;

  if (!adc || !dma_cfg || dma_cfg->dst == NULL || dma_cfg->length == 0u ||
      dma_cfg->length % sizeof(uint16_t) != 0u) return ESY_ERR_INVALID_ARG;
  if (adc->ops == NULL || adc->ops->transfer_dma == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  if (dma_cfg->circular) return ESY_ERR_NOT_SUPPORTED;
  esy_dma_config_t transfer = *dma_cfg;
  dma_cfg = &transfer;
  binding = adc_binding(adc);
  if ((binding != NULL) && binding->dma_ref_held) return ESY_ERR_BUSY;
  ret = adc_acquire(adc);
  if (ret != ESY_OK) return ret;
  if (binding != NULL) adc_prepare_dma_binding(binding, dma_cfg);
  ret = adc->ops->transfer_dma(adc, dma_cfg);
  if (binding == NULL) {
    adc_release(adc);
  } else if ((ret != ESY_OK) && !vt_is_dma_busy(adc) &&
             adc_claim_dma_reference(binding)) {
    adc_cancel_dma_binding(binding);
    adc_release(adc);
  }
  return ret;
}

/* ================= DMA continuous wrappers ================= */

extern esy_err_t esy_hal_adc_start_continuous_dma(
    esy_adc_t *adc, esy_dma_config_t *dma_cfg) {
  if (!adc || !dma_cfg || dma_cfg->dst == NULL || dma_cfg->length == 0u ||
      dma_cfg->length % sizeof(uint16_t) != 0u) return ESY_ERR_INVALID_ARG;
  if (adc->ops == NULL || adc->ops->start_continuous_dma == NULL)
    return ESY_ERR_NOT_IMPLEMENTED;
  esy_dma_config_t transfer = *dma_cfg;
  dma_cfg = &transfer;
  dma_cfg->circular = true;
  esy_adc_device_binding_t *binding = adc_binding(adc);
  if (binding != NULL && binding->dma_ref_held) return ESY_ERR_BUSY;
  esy_err_t ret = adc_acquire(adc);
  if (ret != ESY_OK) return ret;
  if (binding != NULL) adc_prepare_dma_binding(binding, dma_cfg);
  ret = adc->ops->start_continuous_dma(adc, dma_cfg);
  if (binding == NULL) {
    adc_release(adc);
  } else if (ret != ESY_OK && !vt_is_dma_busy(adc) &&
             adc_claim_dma_reference(binding)) {
    adc_cancel_dma_binding(binding);
    adc_release(adc);
  }
  return ret;
}

/* ================= DMA multi-channel scan wrappers ================= */

extern esy_err_t esy_hal_adc_start_multi_channel_dma(
    esy_adc_t *adc, const uint8_t *index_list, uint8_t num_indexes,
    esy_dma_config_t *dma_cfg) {
  esy_adc_device_binding_t *binding;
  esy_err_t ret;

  if (!adc || !index_list || !dma_cfg || num_indexes == 0 ||
      dma_cfg->dst == NULL || dma_cfg->length == 0u ||
      dma_cfg->length % (sizeof(uint16_t) * num_indexes) != 0u)
    return ESY_ERR_INVALID_ARG;
  if (adc->ops == NULL || adc->ops->transfer_dma_multi_channel == NULL)
    return ESY_ERR_NOT_IMPLEMENTED;
  if (dma_cfg->circular) return ESY_ERR_NOT_SUPPORTED;
  esy_dma_config_t transfer = *dma_cfg;
  dma_cfg = &transfer;
  binding = adc_binding(adc);
  if ((binding != NULL) && binding->dma_ref_held) return ESY_ERR_BUSY;
  ret = adc_acquire(adc);
  if (ret != ESY_OK) return ret;
  if (binding != NULL) adc_prepare_dma_binding(binding, dma_cfg);
  ret = adc->ops->transfer_dma_multi_channel(adc, dma_cfg, index_list,
                                            num_indexes);
  if (binding == NULL) {
    adc_release(adc);
  } else if ((ret != ESY_OK) && !vt_is_dma_busy(adc) &&
             adc_claim_dma_reference(binding)) {
    adc_cancel_dma_binding(binding);
    adc_release(adc);
  }
  return ret;
}

/* ================= status query wrappers ================= */

extern bool esy_hal_adc_is_busy(const esy_adc_t *adc) {
  if (!adc) return false;
  if (adc->ops == NULL || adc->ops->is_dma_busy == NULL) return false;
  return adc->ops->is_dma_busy(adc);
}

/* ================= trigger configuration wrappers ================= */

extern esy_err_t esy_hal_adc_set_trigger(esy_adc_t *adc,
                                              esy_adc_trigger_edge_t edge,
                                              esy_adc_trigger_source_t source) {
  esy_hal_adc_runtime_t *runtime = esy_hal_adc_runtime(adc);
  esy_err_t ret;

  if (!adc) return ESY_ERR_INVALID_ARG;
  if (adc->ops == NULL || adc->ops->set_trigger_source == NULL)
    return ESY_ERR_NOT_IMPLEMENTED;
  ret = adc_acquire(adc);
  if (ret != ESY_OK) return ret;
  ret = adc->ops->set_trigger_source(adc, edge, source);
  if (ret == ESY_OK) {
    runtime->trigger_edge = edge;
    runtime->trigger_source = source;
  }
  adc_release(adc);
  return ret;
}

/* ================= query wrappers ================= */

extern esy_err_t esy_hal_adc_get_caps(const esy_adc_t *adc,
                                           uint32_t *out_caps) {
  if (!adc || !out_caps) return ESY_ERR_INVALID_ARG;
  if (adc->ops == NULL || adc->ops->get_caps == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return adc->ops->get_caps(adc, out_caps);
}

extern esy_err_t esy_hal_adc_get_phys_index(const esy_adc_t *adc,
                                                 uint8_t *out_index) {
  if (!adc || !out_index) return ESY_ERR_INVALID_ARG;
  if (adc->ops == NULL || adc->ops->get_phys_index == NULL)
    return ESY_ERR_NOT_IMPLEMENTED;
  return adc->ops->get_phys_index(adc, out_index);
}
