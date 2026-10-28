/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_I2C_DEVICE_H
#define ESY_HAL_I2C_DEVICE_H

#include "esy_common/device/esy_device.h"
#include "esy_hal/esy_hal_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief I2C controller role restored after a PM transition. */
typedef enum {
  ESY_I2C_DEVICE_ROLE_MASTER = 0,
  ESY_I2C_DEVICE_ROLE_SLAVE
} esy_i2c_device_role_t;

/**
 * @brief Caller-owned adapter that makes one I2C controller a graph device.
 *
 * @note Initialize this binding with {0} before its first registration. Keep it
 * alive and unchanged until successful unregister, including after registry
 * deinit. Reusing an attached binding for another handle returns ESY_ERR_STATE
 * without changing its original handle or graph node. Registration failures
 * preserve an existing binding; never clear it to bypass unregister.
 */
typedef struct {
  esy_device_t device;
  esy_i2c_t *i2c;
  esy_i2c_device_role_t role;
  union {
    esy_i2c_config_t master;
    esy_i2c_slave_config_t slave;
  } resume_config;
  esy_dma_callback_t dma_complete_cb;
  void *dma_complete_arg;
  esy_dma_callback_t dma_error_cb;
  void *dma_error_arg;
  bool suspended;
  bool dma_ref_held;
} esy_i2c_device_binding_t;

/** @brief Register an initialized master I2C controller. */
esy_err_t esy_hal_i2c_register_master_device(
    esy_i2c_device_binding_t *binding, esy_i2c_t *i2c,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_i2c_config_t *resume_config);

/** @brief Register an initialized slave I2C controller. */
esy_err_t esy_hal_i2c_register_slave_device(
    esy_i2c_device_binding_t *binding, esy_i2c_t *i2c,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_i2c_slave_config_t *resume_config);

/**
 * @brief Unbind an idle I2C adapter before graph start or after registry deinit.
 * Stop DMA and serialize with I/O/lifecycle calls. Registry deinit leaves this
 * binding attached; unbind successfully before hardware deinit or reuse.
 * Failure retains ownership and permits retry after its cause is resolved.
 */
esy_err_t esy_hal_i2c_unregister_device(esy_i2c_device_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_I2C_DEVICE_H */
