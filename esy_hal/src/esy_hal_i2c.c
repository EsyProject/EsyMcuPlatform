/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_i2c.h"
#include "esy_hal_i2c_internal.h"
#include "esy_hal/esy_hal_i2c_device.h"

#include "esy_hal_backend/esy_hb_i2c.h" /* backend: esy_hb_i2c_xxx */
#include "esy_board/esy_board.h"
#include <string.h>

static esy_err_t i2c_acquire(esy_i2c_t *i2c) {
  return (esy_hal_i2c_runtime(i2c)->device == NULL) ? ESY_OK : esy_device_get(esy_hal_i2c_runtime(i2c)->device);
}

static void i2c_release(esy_i2c_t *i2c) {
  if (esy_hal_i2c_runtime(i2c)->device != NULL) {
    (void)esy_device_put(esy_hal_i2c_runtime(i2c)->device);
  }
}

static esy_i2c_device_binding_t *i2c_binding(esy_i2c_t *i2c) {
  if ((i2c == NULL) || (esy_hal_i2c_runtime(i2c)->device == NULL)) return NULL;
  return (esy_i2c_device_binding_t *)esy_hal_i2c_runtime(i2c)->device->driver_ctx;
}

static void i2c_dma_complete(void *arg) {
  esy_i2c_device_binding_t *binding = arg;
  if ((binding == NULL) || !binding->dma_ref_held) return;
  esy_dma_callback_t callback = binding->dma_complete_cb;
  void *callback_arg = binding->dma_complete_arg;

  binding->dma_complete_cb = NULL;
  binding->dma_complete_arg = NULL;
  binding->dma_error_cb = NULL;
  binding->dma_error_arg = NULL;
  binding->dma_ref_held = false;
  if (callback != NULL) callback(callback_arg);
  (void)esy_device_put_from_isr(&binding->device);
}

static void i2c_dma_error(void *arg) {
  esy_i2c_device_binding_t *binding = arg;
  if ((binding == NULL) || !binding->dma_ref_held) return;
  esy_dma_callback_t callback = binding->dma_error_cb;
  void *callback_arg = binding->dma_error_arg;

  binding->dma_complete_cb = NULL;
  binding->dma_complete_arg = NULL;
  binding->dma_error_cb = NULL;
  binding->dma_error_arg = NULL;
  binding->dma_ref_held = false;
  if (callback != NULL) callback(callback_arg);
  (void)esy_device_put_from_isr(&binding->device);
}

static void i2c_prepare_dma_binding(esy_i2c_device_binding_t *binding,
                                    esy_dma_config_t *dma_cfg) {
  binding->dma_complete_cb = dma_cfg->callback;
  binding->dma_complete_arg = dma_cfg->callback_arg;
  binding->dma_error_cb = dma_cfg->error_callback;
  binding->dma_error_arg = dma_cfg->error_callback_arg;
  binding->dma_ref_held = true;
  dma_cfg->callback = i2c_dma_complete;
  dma_cfg->callback_arg = binding;
  dma_cfg->error_callback = i2c_dma_error;
  dma_cfg->error_callback_arg = binding;
}

static void i2c_cancel_dma_binding(esy_i2c_device_binding_t *binding) {
  binding->dma_complete_cb = NULL;
  binding->dma_complete_arg = NULL;
  binding->dma_error_cb = NULL;
  binding->dma_error_arg = NULL;
  binding->dma_ref_held = false;
}

/* ================= vt_*: sole ops implementation =================
 * Notes:
 * - HAL public APIs call i2c->ops->*, do not perform board or index
 * completion.
 * - vt_* functions perform required board work and HAL backend forwarding.
 */

/* -------- blocking I/O -------- */

static int32_t vt_write_blocking(esy_i2c_t *i2c, uint16_t addr,
                                 const uint8_t *data, size_t len) {
  return esy_hb_i2c_write_blocking(i2c, addr, data, len);
}

static int32_t vt_read_blocking(esy_i2c_t *i2c, uint16_t addr, uint8_t *data,
                                size_t len) {
  return esy_hb_i2c_read_blocking(i2c, addr, data, len);
}

/* -------- DMA I/O -------- */

static esy_err_t vt_write_dma(esy_i2c_t *i2c, uint16_t addr,
                              esy_dma_config_t *dma_cfg) {
  if (!i2c || !dma_cfg) return ESY_ERR_INVALID_ARG;

  /* Resolve the physical index and complete TX DMA metadata in the vt function
   */
  uint8_t idx;
  esy_err_t r = esy_hb_i2c_get_phys_index(i2c, &idx);
  if (r != ESY_OK) return r;

  if (!esy_board_i2c_get_dma_info(idx, &dma_cfg->inner, true /* is_tx */))
    return ESY_ERR_NOT_SUPPORTED;
  return esy_hb_i2c_write_dma(i2c, addr, dma_cfg);
}

static esy_err_t vt_read_dma(esy_i2c_t *i2c, uint16_t addr,
                             esy_dma_config_t *dma_cfg) {
  if (!i2c || !dma_cfg) return ESY_ERR_INVALID_ARG;

  /* Resolve the physical index and complete RX DMA metadata in the vt function
   */
  uint8_t idx;
  esy_err_t r = esy_hb_i2c_get_phys_index(i2c, &idx);
  if (r != ESY_OK) return r;

  if (!esy_board_i2c_get_dma_info(idx, &dma_cfg->inner, false /* is_tx */))
    return ESY_ERR_NOT_SUPPORTED;
  return esy_hb_i2c_read_dma(i2c, addr, dma_cfg);
}

/* -------- advanced operations -------- */

static int32_t vt_transfer(esy_i2c_t *i2c, const esy_i2c_xfer_t *xfer) {
  return esy_hb_i2c_transfer(i2c, xfer);
}

static esy_err_t vt_set_speed(esy_i2c_t *i2c, uint32_t speed_hz) {
  return esy_hb_i2c_set_speed(i2c, speed_hz);
}

static esy_err_t vt_abort(esy_i2c_t *i2c) { return esy_hb_i2c_abort(i2c); }

static esy_err_t vt_recover_bus(esy_i2c_t *i2c) {
  return esy_hb_i2c_recover_bus(i2c);
}

/* -------- queries -------- */

static esy_err_t vt_get_caps(const esy_i2c_t *i2c, uint32_t *out_caps) {
  return esy_hb_i2c_get_caps(i2c, out_caps);
}

static esy_err_t vt_get_phys_index(const esy_i2c_t *i2c, uint8_t *out_index) {
  return esy_hb_i2c_get_phys_index(i2c, out_index);
}

/* ================ defaults ================ */

extern void esy_hal_i2c_default_cfg(esy_i2c_config_t *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->speed_hz = 100000u;
  cfg->addr_mode = ESY_I2C_ADDR_7BIT;
  cfg->inner.sda_pin = ESY_PIN_UNUSED;
  cfg->inner.scl_pin = ESY_PIN_UNUSED;
}

/* ================= helpers ================= */

/**
 * @brief Bind vt_* implementations into i2c->ops
 */
static const esy_i2c_vtable_t k_i2c_ops = {
    .write_blocking = vt_write_blocking,
    .read_blocking = vt_read_blocking,
    .write_dma = vt_write_dma,
    .read_dma = vt_read_dma,
    .transfer = vt_transfer,
    .set_speed = vt_set_speed,
    .abort = vt_abort,
    .recover_bus = vt_recover_bus,
    .get_caps = vt_get_caps,
    .get_phys_index = vt_get_phys_index,
};

static void vt_bind_ops(esy_i2c_t *i2c) {
  i2c->ops = &k_i2c_ops;
}

static bool i2c_is_busy(esy_i2c_t *i2c) {
  return esy_hb_i2c_is_busy(i2c);
}

static esy_err_t i2c_cleanup_hardware(esy_i2c_t *i2c, uint8_t index,
                                      bool retain_reservation) {
  esy_err_t ret = esy_hb_i2c_suspend(i2c);
  if (ret != ESY_OK) return ret;
  /* Keep ownership until board teardown finishes, so a new owner cannot route
   * pins between native cleanup and the old owner's reset. */
  esy_board_i2c_reset_pins(index);
  return retain_reservation ? ESY_OK : esy_hb_i2c_deinit(i2c);
}

static esy_err_t i2c_hardware_init_master(esy_i2c_t *i2c,
                                          const esy_i2c_config_t *cfg,
                                          bool resuming) {
  esy_i2c_config_t local = *cfg;
  esy_err_t ret = esy_hb_i2c_claim(i2c, local.phys_index);
  if (ret != ESY_OK) return ret;
  local.inner.sda_pin = ESY_PIN_UNUSED;
  local.inner.scl_pin = ESY_PIN_UNUSED;
  esy_board_i2c_configure_pins(local.phys_index, ESY_I2C_ROLE_MASTER, &local.inner);
  ret = esy_hb_i2c_init_master(i2c, &local);
  if (ret == ESY_OK) {
    vt_bind_ops(i2c);
  } else {
    esy_err_t cleanup = i2c_cleanup_hardware(i2c, local.phys_index, resuming);
    if (cleanup != ESY_OK) return cleanup;
    if (!resuming) i2c->ops = NULL;
  }
  return ret;
}

static esy_err_t i2c_hardware_init_slave(esy_i2c_t *i2c,
                                         const esy_i2c_slave_config_t *cfg,
                                         bool resuming) {
  esy_i2c_slave_config_t local = *cfg;
  esy_err_t ret = esy_hb_i2c_validate_slave_config(&local);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_i2c_claim(i2c, local.phys_index);
  if (ret != ESY_OK) return ret;
  local.inner.sda_pin = ESY_PIN_UNUSED;
  local.inner.scl_pin = ESY_PIN_UNUSED;
  esy_board_i2c_configure_pins(local.phys_index, ESY_I2C_ROLE_SLAVE, &local.inner);
  ret = esy_hb_i2c_init_slave(i2c, &local);
  if (ret == ESY_OK) {
    vt_bind_ops(i2c);
  } else {
    esy_err_t cleanup = i2c_cleanup_hardware(i2c, local.phys_index, resuming);
    if (cleanup != ESY_OK) return cleanup;
    if (!resuming) i2c->ops = NULL;
  }
  return ret;
}

static esy_err_t i2c_hardware_deinit(esy_i2c_t *i2c) {
  uint8_t index = ESY_PIN_UNUSED;
  if (i2c->ctx == NULL) return ESY_OK;
  esy_err_t ret = esy_hb_i2c_get_phys_index(i2c, &index);
  if (ret != ESY_OK) return ret;
  return i2c_cleanup_hardware(i2c, index, false);
}

static esy_err_t i2c_hardware_suspend(
    esy_i2c_t *i2c, esy_board_controller_pin_state_t pin_state) {
  uint8_t index = ESY_PIN_UNUSED;
  esy_err_t ret = esy_hb_i2c_get_phys_index(i2c, &index);

  if (ret != ESY_OK) return ret;
  ret = esy_board_i2c_select_pin_state(index, pin_state);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_i2c_suspend(i2c);
  if (ret == ESY_OK) return ESY_OK;
  /* Native cleanup can fail after partial teardown. Preserve its reservation
   * and fault the graph even if restoring default pins succeeds. */
  esy_err_t rollback = esy_board_i2c_select_pin_state(
      index, ESY_BOARD_CONTROLLER_PIN_STATE_DEFAULT);
  if (ret == ESY_ERR_BUSY && rollback == ESY_OK) return ESY_ERR_BUSY;
  return ESY_ERR_STATE;
}

static esy_err_t i2c_device_pm_action(esy_device_t *device,
                                      esy_device_pm_action_t action,
                                      esy_power_state_t system_state) {
  esy_i2c_device_binding_t *binding;
  esy_err_t ret;

  (void)system_state;
  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_i2c_device_binding_t *)device->driver_ctx;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (binding->suspended) return ESY_ERR_STATE;
      if (i2c_is_busy(binding->i2c)) return ESY_ERR_BUSY;
      ret = i2c_hardware_suspend(
          binding->i2c,
          action == ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND
              ? ESY_BOARD_CONTROLLER_PIN_STATE_IDLE
              : ESY_BOARD_CONTROLLER_PIN_STATE_SLEEP);
      if (ret != ESY_OK) return ret;
      binding->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!binding->suspended) return ESY_ERR_STATE;
      ret = (binding->role == ESY_I2C_DEVICE_ROLE_MASTER)
                ? i2c_hardware_init_master(binding->i2c,
                                           &binding->resume_config.master, true)
                : i2c_hardware_init_slave(binding->i2c,
                                          &binding->resume_config.slave, true);
      if (ret != ESY_OK) return ret;
      binding->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static bool i2c_device_is_runtime_idle(const esy_device_t *device) {
  const esy_i2c_device_binding_t *binding;

  if ((device == NULL) || (device->driver_ctx == NULL)) return false;
  binding = device->driver_ctx;
  return (binding->role == ESY_I2C_DEVICE_ROLE_MASTER) &&
         !i2c_is_busy(binding->i2c);
}

static const esy_device_ops_t k_i2c_device_ops = {
    .pm_action = i2c_device_pm_action,
    .is_runtime_idle = i2c_device_is_runtime_idle,
};

static esy_err_t i2c_register_device(
    esy_i2c_device_binding_t *binding, esy_i2c_t *i2c,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config) {
  esy_device_config_t config;
  esy_err_t ret;

  config.name = device_config->name;
  config.ops = &k_i2c_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->dependencies;
  config.dependency_count = device_config->dependency_count;
  config.supported_state_mask = device_config->supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    esy_hal_i2c_runtime(i2c)->device = &binding->device;
  }
  return ret;
}

/* ================ lifecycle ================ */

extern esy_err_t esy_hal_i2c_init(esy_i2c_t *i2c,
                                       const esy_i2c_config_t *cfg) {
  if (!i2c || !cfg) return ESY_ERR_INVALID_ARG;
  if (esy_hal_i2c_runtime(i2c)->device != NULL || i2c->ctx != NULL ||
      (i2c->ops != NULL && i2c->ops->write_blocking != NULL))
    return ESY_ERR_STATE;
  return i2c_hardware_init_master(i2c, cfg, false);
}

extern esy_err_t esy_hal_i2c_slave_init(
    esy_i2c_t *i2c, const esy_i2c_slave_config_t *cfg) {
  if (!i2c || !cfg) return ESY_ERR_INVALID_ARG;
  if (esy_hal_i2c_runtime(i2c)->device != NULL || i2c->ctx != NULL ||
      (i2c->ops != NULL && i2c->ops->write_blocking != NULL))
    return ESY_ERR_STATE;
  return i2c_hardware_init_slave(i2c, cfg, false);
}

extern esy_err_t esy_hal_i2c_deinit(esy_i2c_t *i2c) {
  if (!i2c) return ESY_ERR_INVALID_ARG;
  if (esy_hal_i2c_runtime(i2c)->device != NULL) return ESY_ERR_STATE;
  if (i2c_is_busy(i2c)) return ESY_ERR_BUSY;
  esy_err_t ret = i2c_hardware_deinit(i2c);
  if (ret != ESY_OK) return ret;
  memset(i2c, 0, sizeof(*i2c));
  return ESY_OK;
}

esy_err_t esy_hal_i2c_register_master_device(
    esy_i2c_device_binding_t *binding, esy_i2c_t *i2c,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_i2c_config_t *resume_config) {
  esy_err_t ret;

  if ((binding == NULL) || (i2c == NULL) || (registry == NULL) ||
      (device_config == NULL) || (resume_config == NULL) ||
      (esy_hal_i2c_runtime(i2c)->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->i2c != NULL) return ESY_ERR_STATE;
  memset(binding, 0, sizeof(*binding));
  binding->i2c = i2c;
  binding->role = ESY_I2C_DEVICE_ROLE_MASTER;
  binding->resume_config.master = *resume_config;
  ret = i2c_register_device(binding, i2c, registry, device_config);
  if (ret != ESY_OK) memset(binding, 0, sizeof(*binding));
  return ret;
}

esy_err_t esy_hal_i2c_register_slave_device(
    esy_i2c_device_binding_t *binding, esy_i2c_t *i2c,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_i2c_slave_config_t *resume_config) {
  esy_err_t ret;

  if ((binding == NULL) || (i2c == NULL) || (registry == NULL) ||
      (device_config == NULL) || (resume_config == NULL) ||
      (esy_hal_i2c_runtime(i2c)->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->i2c != NULL) return ESY_ERR_STATE;
  memset(binding, 0, sizeof(*binding));
  binding->i2c = i2c;
  binding->role = ESY_I2C_DEVICE_ROLE_SLAVE;
  binding->resume_config.slave = *resume_config;
  ret = i2c_register_device(binding, i2c, registry, device_config);
  if (ret != ESY_OK) memset(binding, 0, sizeof(*binding));
  return ret;
}

esy_err_t esy_hal_i2c_unregister_device(
    esy_i2c_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->i2c == NULL) ||
      (esy_hal_i2c_runtime(binding->i2c)->device != &binding->device) || binding->dma_ref_held ||
      i2c_is_busy(binding->i2c)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    esy_hal_i2c_runtime(binding->i2c)->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

/* ================ blocking I/O ================ */

extern int32_t esy_hal_i2c_write_blocking(esy_i2c_t *i2c, uint16_t addr,
                                               const uint8_t *data,
                                               size_t len) {
  int32_t ret;

  if (!i2c || !data || !len || addr > ESY_I2C_ADDR_10BIT_MAX)
    return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->write_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = i2c_acquire(i2c);
  if (ret != ESY_OK) return ret;
  ret = i2c->ops->write_blocking(i2c, addr, data, len);
  i2c_release(i2c);
  return ret;
}

extern int32_t esy_hal_i2c_read_blocking(esy_i2c_t *i2c, uint16_t addr,
                                              uint8_t *data, size_t len) {
  int32_t ret;

  if (!i2c || !data || !len || addr > ESY_I2C_ADDR_10BIT_MAX)
    return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->read_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = i2c_acquire(i2c);
  if (ret != ESY_OK) return ret;
  ret = i2c->ops->read_blocking(i2c, addr, data, len);
  i2c_release(i2c);
  return ret;
}

/* ================ DMA I/O ================ */

extern esy_err_t esy_hal_i2c_write_dma(esy_i2c_t *i2c, uint16_t addr,
                                            esy_dma_config_t *dma_cfg) {
  esy_i2c_device_binding_t *binding;
  esy_err_t ret;

  if (!i2c || !dma_cfg || !dma_cfg->src || dma_cfg->length == 0 ||
      addr > ESY_I2C_ADDR_10BIT_MAX)
    return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->write_dma == NULL) return ESY_ERR_NOT_SUPPORTED;
  if (dma_cfg->circular) return ESY_ERR_NOT_SUPPORTED;
  esy_dma_config_t transfer = *dma_cfg;
  dma_cfg = &transfer;
  binding = i2c_binding(i2c);
  if ((binding != NULL) && binding->dma_ref_held) return ESY_ERR_BUSY;
  ret = i2c_acquire(i2c);
  if (ret != ESY_OK) return ret;
  if (binding != NULL) i2c_prepare_dma_binding(binding, dma_cfg);
  ret = i2c->ops->write_dma(i2c, addr, dma_cfg);
  if (binding == NULL) {
    i2c_release(i2c);
  } else if ((ret != ESY_OK) && binding->dma_ref_held) {
    i2c_cancel_dma_binding(binding);
    i2c_release(i2c);
  }
  return ret;
}

extern esy_err_t esy_hal_i2c_read_dma(esy_i2c_t *i2c, uint16_t addr,
                                           esy_dma_config_t *dma_cfg) {
  esy_i2c_device_binding_t *binding;
  esy_err_t ret;

  if (!i2c || !dma_cfg || !dma_cfg->dst || dma_cfg->length == 0 ||
      addr > ESY_I2C_ADDR_10BIT_MAX)
    return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->read_dma == NULL) return ESY_ERR_NOT_SUPPORTED;
  if (dma_cfg->circular) return ESY_ERR_NOT_SUPPORTED;
  esy_dma_config_t transfer = *dma_cfg;
  dma_cfg = &transfer;
  binding = i2c_binding(i2c);
  if ((binding != NULL) && binding->dma_ref_held) return ESY_ERR_BUSY;
  ret = i2c_acquire(i2c);
  if (ret != ESY_OK) return ret;
  if (binding != NULL) i2c_prepare_dma_binding(binding, dma_cfg);
  ret = i2c->ops->read_dma(i2c, addr, dma_cfg);
  if (binding == NULL) {
    i2c_release(i2c);
  } else if ((ret != ESY_OK) && binding->dma_ref_held) {
    i2c_cancel_dma_binding(binding);
    i2c_release(i2c);
  }
  return ret;
}

/* ================ advanced ================= */

extern int32_t esy_hal_i2c_transfer(esy_i2c_t *i2c,
                                         const esy_i2c_xfer_t *xfer) {
  int32_t ret;

  if (!i2c || !xfer || xfer->addr > ESY_I2C_ADDR_10BIT_MAX ||
      xfer->timeout_ms > INT32_MAX) return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->transfer == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  ret = i2c_acquire(i2c);
  if (ret != ESY_OK) return ret;
  ret = i2c->ops->transfer(i2c, xfer);
  i2c_release(i2c);
  return ret;
}

extern esy_err_t esy_hal_i2c_set_speed(esy_i2c_t *i2c, uint32_t speed_hz) {
  esy_err_t ret;

  if (!i2c || !speed_hz) return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->set_speed == NULL) return ESY_ERR_NOT_SUPPORTED;
  ret = i2c_acquire(i2c);
  if (ret != ESY_OK) return ret;
  ret = i2c->ops->set_speed(i2c, speed_hz);
  esy_i2c_device_binding_t *binding = i2c_binding(i2c);
  if (ret == ESY_OK && binding != NULL &&
      binding->role == ESY_I2C_DEVICE_ROLE_MASTER)
    binding->resume_config.master.speed_hz = speed_hz;
  i2c_release(i2c);
  return ret;
}

extern esy_err_t esy_hal_i2c_abort(esy_i2c_t *i2c) {
  esy_err_t ret;

  if (!i2c) return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->abort == NULL) return ESY_ERR_NOT_SUPPORTED;
  ret = i2c_acquire(i2c);
  if (ret != ESY_OK) return ret;
  ret = i2c->ops->abort(i2c);
  i2c_release(i2c);
  return ret;
}

extern esy_err_t esy_hal_i2c_recover_bus(esy_i2c_t *i2c) {
  esy_err_t ret;

  if (!i2c) return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->recover_bus == NULL) return ESY_ERR_NOT_SUPPORTED;
  ret = i2c_acquire(i2c);
  if (ret != ESY_OK) return ret;
  ret = i2c->ops->recover_bus(i2c);
  i2c_release(i2c);
  return ret;
}

/* ================ queries ================ */

extern esy_err_t esy_hal_i2c_get_caps(const esy_i2c_t *i2c,
                                           uint32_t *out_caps) {
  if (!i2c || !out_caps) return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->get_caps == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return i2c->ops->get_caps(i2c, out_caps);
}

extern esy_err_t esy_hal_i2c_get_phys_index(const esy_i2c_t *i2c,
                                                 uint8_t *out_index) {
  if (!i2c || !out_index) return ESY_ERR_INVALID_ARG;
  if (i2c->ops == NULL || i2c->ops->get_phys_index == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return i2c->ops->get_phys_index(i2c, out_index);
}
