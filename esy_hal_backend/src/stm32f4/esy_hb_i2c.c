/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_i2c.h"
#include "esy_common/defs/peripheral/esy_i2c_defs.h"

#include <stdint.h>
#include <stdatomic.h>

#include "stm32f4xx_hal.h"

typedef struct {
  I2C_TypeDef *inst;
  uint8_t phys_index;
  esy_i2c_role_t role;
  atomic_bool enabled;
  bool cleanup_pending;
  atomic_bool transfer_active;

  I2C_HandleTypeDef hal;

  uint32_t speed_hz;
  esy_i2c_addr_mode_t addr_mode;

  _Atomic(esy_i2c_t *) bound_i2c;
} stm32f4_i2c_ctx_t;

#define STM32F4_I2C_MAX 4u

typedef struct {
  stm32f4_i2c_ctx_t channel[STM32F4_I2C_MAX];
} esy_hb_i2c_module_ctx_t;

static esy_hb_i2c_module_ctx_t s_ctx = {.channel = {
#if defined(I2C1)
    [1] = {.inst = I2C1,
     .phys_index = 1,
     .role = ESY_I2C_ROLE_MASTER,
     .enabled = false},
#endif
#if defined(I2C2)
    [2] = {.inst = I2C2,
     .phys_index = 2,
     .role = ESY_I2C_ROLE_MASTER,
     .enabled = false},
#endif
#if defined(I2C3)
    [3] = {.inst = I2C3,
     .phys_index = 3,
     .role = ESY_I2C_ROLE_MASTER,
     .enabled = false},
#endif
}};

static stm32f4_i2c_ctx_t *i2c_context(const esy_i2c_t *i2c) {
  if (i2c == NULL || i2c->ctx == NULL) return NULL;
  for (uint8_t i = 1u; i < STM32F4_I2C_MAX; ++i) {
    stm32f4_i2c_ctx_t *c = &s_ctx.channel[i];
    if (i2c->ctx == c && atomic_load_explicit(&c->bound_i2c,
                                             memory_order_acquire) == i2c)
      return c;
  }
  return NULL;
}

esy_err_t esy_hb_i2c_claim(esy_i2c_t *i2c, uint8_t phys_index) {
  if (i2c == NULL || phys_index >= STM32F4_I2C_MAX ||
      s_ctx.channel[phys_index].inst == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_i2c_ctx_t *c = &s_ctx.channel[phys_index];
  if (i2c->ctx != NULL) {
    return i2c_context(i2c) == c && !c->enabled && !c->cleanup_pending
               ? ESY_OK : ESY_ERR_STATE;
  }
  esy_i2c_t *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          &c->bound_i2c, &expected, i2c, memory_order_acq_rel,
          memory_order_acquire)) return ESY_ERR_BUSY;
  i2c->ctx = c;
  return ESY_OK;
}

bool esy_hb_i2c_is_busy(const esy_i2c_t *i2c) {
  stm32f4_i2c_ctx_t *c = i2c_context(i2c);
  if (c == NULL) return false;
  if (c->transfer_active) return true;
  if (!c->enabled) return false;
  HAL_I2C_StateTypeDef state = HAL_I2C_GetState(&c->hal);
  return state != HAL_I2C_STATE_READY && state != HAL_I2C_STATE_LISTEN;
}

extern esy_err_t esy_hb_i2c_init_master(esy_i2c_t *i2c,
                                            const esy_i2c_config_t *cfg) {
  if (!i2c || !cfg) return ESY_ERR_INVALID_ARG;
  if (cfg->phys_index >= STM32F4_I2C_MAX) return ESY_ERR_INVALID_ARG;
  if (cfg->addr_mode != ESY_I2C_ADDR_7BIT &&
      cfg->addr_mode != ESY_I2C_ADDR_10BIT) return ESY_ERR_INVALID_ARG;
  if (cfg->addr_mode == ESY_I2C_ADDR_10BIT) return ESY_ERR_NOT_SUPPORTED;
  esy_err_t ret = esy_hb_i2c_claim(i2c, cfg->phys_index);
  if (ret != ESY_OK) return ret;
  stm32f4_i2c_ctx_t *c = &s_ctx.channel[cfg->phys_index];

  c->role = ESY_I2C_ROLE_MASTER;
  c->cleanup_pending = true;
  c->speed_hz = cfg->speed_hz ? cfg->speed_hz : 100000u;
  c->addr_mode = cfg->addr_mode;

  I2C_HandleTypeDef *hi2c = &c->hal;
  hi2c->Instance = c->inst;
  hi2c->Init.ClockSpeed = c->speed_hz;
  hi2c->Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c->Init.OwnAddress1 = 0;
  hi2c->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c->Init.OwnAddress2 = 0;
  hi2c->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

  if (HAL_I2C_Init(hi2c) != HAL_OK) {
    c->enabled = false;
    return ESY_ERR_FAIL;
  }

  c->enabled = true;
  c->cleanup_pending = false;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_validate_slave_config(
    const esy_i2c_slave_config_t *cfg) {
  if (cfg == NULL) return ESY_ERR_INVALID_ARG;
  /* No complete EV/ER/address/owned-buffer dispatcher is implemented. */
  return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_i2c_init_slave(esy_i2c_t *i2c,
                                           const esy_i2c_slave_config_t *cfg) {
  if (i2c == NULL) return ESY_ERR_INVALID_ARG;
  return esy_hb_i2c_validate_slave_config(cfg);
}

esy_err_t esy_hb_i2c_suspend(esy_i2c_t *i2c) {
  if (i2c == NULL) return ESY_ERR_INVALID_ARG;
  stm32f4_i2c_ctx_t *c = i2c_context(i2c);
  if (c == NULL) return ESY_ERR_STATE;
  if (esy_hb_i2c_is_busy(i2c)) return ESY_ERR_BUSY;
  c->cleanup_pending = true;
  c->enabled = false;
  if (c->hal.Instance != NULL && HAL_I2C_DeInit(&c->hal) != HAL_OK)
    return ESY_ERR_FAIL;

  c->enabled = false;
  c->hal.Instance = NULL;
  c->cleanup_pending = false;
  c->transfer_active = false;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_deinit(esy_i2c_t *i2c) {
  if (i2c == NULL) return ESY_ERR_INVALID_ARG;
  if (i2c->ctx == NULL) return ESY_OK;
  stm32f4_i2c_ctx_t *c = i2c_context(i2c);
  if (c == NULL) return ESY_ERR_STATE;
  esy_err_t ret = esy_hb_i2c_suspend(i2c);
  if (ret != ESY_OK) return ret;
  i2c->ctx = NULL;
  atomic_store_explicit(&c->bound_i2c, NULL, memory_order_release);
  return ESY_OK;
}

extern int32_t esy_hb_i2c_write_blocking(esy_i2c_t *i2c, uint16_t addr,
                                             const uint8_t *data, size_t len) {
  const esy_i2c_xfer_t transfer = {.addr = addr, .tx = data, .tx_len = len};
  return esy_hb_i2c_transfer(i2c, &transfer);
}

extern int32_t esy_hb_i2c_read_blocking(esy_i2c_t *i2c, uint16_t addr,
                                            uint8_t *data, size_t len) {
  const esy_i2c_xfer_t transfer = {.addr = addr, .rx = data, .rx_len = len};
  return esy_hb_i2c_transfer(i2c, &transfer);
}

extern esy_err_t esy_hb_i2c_write_dma(esy_i2c_t *i2c, uint16_t addr,
                                          esy_dma_config_t *dma_cfg) {
  (void)addr;
  if (!i2c || !i2c->ctx || !dma_cfg || !dma_cfg->src || dma_cfg->length == 0 ||
      dma_cfg->length > UINT16_MAX)
    return ESY_ERR_INVALID_ARG;
  const stm32f4_i2c_ctx_t *c = (const stm32f4_i2c_ctx_t *)i2c->ctx;
  if (c->role != ESY_I2C_ROLE_MASTER) return ESY_ERR_INVALID_ARG;

  /* Native I2C DMA lacks a shared stream-owner and callback-retirement contract.
   * Reject before borrowing storage, claiming a stream or starting the bus. */
  return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_i2c_read_dma(esy_i2c_t *i2c, uint16_t addr,
                                         esy_dma_config_t *dma_cfg) {
  (void)addr;
  if (!i2c || !i2c->ctx || !dma_cfg || !dma_cfg->dst || dma_cfg->length == 0 ||
      dma_cfg->length > UINT16_MAX)
    return ESY_ERR_INVALID_ARG;
  const stm32f4_i2c_ctx_t *c = (const stm32f4_i2c_ctx_t *)i2c->ctx;
  if (c->role != ESY_I2C_ROLE_MASTER) return ESY_ERR_INVALID_ARG;

  /* A payload stream alone cannot establish native transaction retirement.
   * No buffer, callback or DMA resource is retained by this rejection. */
  return ESY_ERR_NOT_SUPPORTED;
}

extern int32_t esy_hb_i2c_transfer(esy_i2c_t *i2c,
                                       const esy_i2c_xfer_t *xfer) {
  if (i2c == NULL || xfer == NULL) return ESY_ERR_INVALID_ARG;
  if ((xfer->tx == NULL && xfer->tx_len != 0u) ||
      (xfer->rx == NULL && xfer->rx_len != 0u) ||
      (xfer->tx_len == 0u && xfer->rx_len == 0u) ||
      xfer->tx_len > UINT16_MAX || xfer->rx_len > UINT16_MAX ||
      xfer->addr > ESY_I2C_ADDR_7BIT_MAX || xfer->timeout_ms > INT32_MAX)
    return ESY_ERR_INVALID_ARG;
  stm32f4_i2c_ctx_t *c = i2c_context(i2c);
  if (c == NULL || !c->enabled) return ESY_ERR_STATE;
  if (c->role != ESY_I2C_ROLE_MASTER) return ESY_ERR_INVALID_ARG;
  if (c->addr_mode == ESY_I2C_ADDR_10BIT) return ESY_ERR_NOT_SUPPORTED;
  /* Reject an unsupported compound transaction before its write phase has
   * externally visible effects. No STOP-free transaction is available here. */
  if (xfer->repeated_start && xfer->tx_len != 0u && xfer->rx_len != 0u)
    return ESY_ERR_NOT_SUPPORTED;
  if (esy_hb_i2c_is_busy(i2c)) return ESY_ERR_BUSY;
  /* Avoid the native fixed bus-busy wait consuming an unrelated timeout. */
  if (__HAL_I2C_GET_FLAG(&c->hal, I2C_FLAG_BUSY) != RESET) return ESY_ERR_BUSY;
  const uint32_t budget_ms = xfer->timeout_ms == 0u
                                 ? ESY_I2C_DEFAULT_TIMEOUT_MS : xfer->timeout_ms;
  const uint32_t started_ms = HAL_GetTick();
  HAL_StatusTypeDef status = HAL_OK;
  c->transfer_active = true;
  if (xfer->tx && xfer->tx_len) {
    status = HAL_I2C_Master_Transmit(
        &c->hal, (uint16_t)(xfer->addr << 1u), (uint8_t *)xfer->tx,
        (uint16_t)xfer->tx_len, budget_ms);
  }

  uint32_t elapsed_ms = HAL_GetTick() - started_ms;
  if (status == HAL_OK && elapsed_ms < budget_ms && xfer->rx && xfer->rx_len) {
    status = HAL_I2C_Master_Receive(
        &c->hal, (uint16_t)(xfer->addr << 1u), xfer->rx,
        (uint16_t)xfer->rx_len, budget_ms - elapsed_ms);
  }
  elapsed_ms = HAL_GetTick() - started_ms;
  c->transfer_active = false;
  (void)xfer->prefer_dma;
  if (status == HAL_TIMEOUT ||
      (HAL_I2C_GetError(&c->hal) & HAL_I2C_ERROR_TIMEOUT) != 0u ||
      elapsed_ms >= budget_ms) return ESY_ERR_TIMEOUT;
  if (status == HAL_BUSY) return ESY_ERR_BUSY;
  return status == HAL_OK ? (int32_t)(xfer->tx_len + xfer->rx_len) : ESY_ERR_IO;
}

extern esy_err_t esy_hb_i2c_set_speed(esy_i2c_t *i2c, uint32_t speed_hz) {
  if (!i2c || !i2c->ctx || !speed_hz) return ESY_ERR_INVALID_ARG;
  stm32f4_i2c_ctx_t *c = (stm32f4_i2c_ctx_t *)i2c->ctx;
  if (c->role != ESY_I2C_ROLE_MASTER) return ESY_ERR_INVALID_ARG;

  if (!c->enabled) return ESY_ERR_STATE;
  if (esy_hb_i2c_is_busy(i2c)) return ESY_ERR_BUSY;
  c->enabled = false;
  c->cleanup_pending = true;
  if (HAL_I2C_DeInit(&c->hal) != HAL_OK) return ESY_ERR_FAIL;
  c->hal.Init.ClockSpeed = speed_hz;
  if (HAL_I2C_Init(&c->hal) != HAL_OK) return ESY_ERR_FAIL;
  c->speed_hz = speed_hz;
  c->cleanup_pending = false;
  c->enabled = true;
  return ESY_OK;
}

extern esy_err_t esy_hb_i2c_abort(esy_i2c_t *i2c) {
  (void)i2c;
  return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_i2c_recover_bus(esy_i2c_t *i2c) {
  (void)i2c;
  return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_i2c_get_caps(const esy_i2c_t *i2c,
                                         uint32_t *out_caps) {
  if (!i2c || !i2c->ctx || !out_caps) return ESY_ERR_INVALID_ARG;
  const stm32f4_i2c_ctx_t *c = (const stm32f4_i2c_ctx_t *)i2c->ctx;
  *out_caps = c->role == ESY_I2C_ROLE_MASTER
                  ? ESY_I2C_CAP_MASTER_TRANSFER
                  : 0u;
  return ESY_OK;
}

extern esy_err_t esy_hb_i2c_get_phys_index(const esy_i2c_t *i2c,
                                               uint8_t *out_index) {
  if (!i2c || !i2c->ctx || !out_index) return ESY_ERR_INVALID_ARG;
  const stm32f4_i2c_ctx_t *c = (const stm32f4_i2c_ctx_t *)i2c->ctx;
  *out_index = c->phys_index;
  return ESY_OK;
}
