/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_i2c.h"
#include "esy_common/defs/peripheral/esy_i2c_defs.h"
#include <stdatomic.h>

#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "pico/i2c_slave.h"

#define RP2040_I2C_MAX 2

typedef struct {
  i2c_inst_t *inst;
  uint8_t phys_index;
  esy_i2c_role_t role;
  atomic_bool enabled;
  atomic_bool transfer_active;
  atomic_bool callback_active;

  uint32_t speed_hz;
  esy_i2c_addr_mode_t addr_mode;

  uint16_t slave_addr;
  esy_i2c_addr_mode_t slave_addr_mode;
  esy_i2c_slave_receive_cb receive_cb;
  esy_i2c_slave_request_cb request_cb;
  esy_i2c_slave_finish_cb finish_cb;

  _Atomic(esy_i2c_t *) bound_i2c;
} rp2040_i2c_ctx_t;

typedef struct {
  rp2040_i2c_ctx_t channel[RP2040_I2C_MAX];
} esy_hb_i2c_module_ctx_t;

static esy_hb_i2c_module_ctx_t s_ctx = {
    .channel = {
        {.inst = i2c0, .phys_index = 0, .role = ESY_I2C_ROLE_MASTER},
        {.inst = i2c1, .phys_index = 1, .role = ESY_I2C_ROLE_MASTER},
    },
};

static void rp2040_i2c_slave_isr(i2c_inst_t *i2c, i2c_slave_event_t event);

static rp2040_i2c_ctx_t *i2c_context(const esy_i2c_t *i2c) {
  if (i2c == NULL || i2c->ctx == NULL) return NULL;
  for (uint8_t i = 0u; i < RP2040_I2C_MAX; ++i) {
    rp2040_i2c_ctx_t *c = &s_ctx.channel[i];
    if (i2c->ctx == c && atomic_load_explicit(&c->bound_i2c,
                                             memory_order_acquire) == i2c)
      return c;
  }
  return NULL;
}

esy_err_t esy_hb_i2c_claim(esy_i2c_t *i2c, uint8_t phys_index) {
  if (i2c == NULL || phys_index >= RP2040_I2C_MAX)
    return ESY_ERR_INVALID_ARG;
  rp2040_i2c_ctx_t *c = &s_ctx.channel[phys_index];
  if (i2c->ctx != NULL) {
    return i2c_context(i2c) == c && !c->enabled ? ESY_OK : ESY_ERR_STATE;
  }
  esy_i2c_t *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          &c->bound_i2c, &expected, i2c, memory_order_acq_rel,
          memory_order_acquire)) return ESY_ERR_BUSY;
  i2c->ctx = c;
  return ESY_OK;
}

bool esy_hb_i2c_is_busy(const esy_i2c_t *i2c) {
  const rp2040_i2c_ctx_t *c = i2c_context(i2c);
  return c != NULL &&
         (c->transfer_active || c->callback_active ||
          (c->enabled &&
           (i2c_get_hw(c->inst)->status & I2C_IC_STATUS_ACTIVITY_BITS) != 0u));
}

esy_err_t esy_hb_i2c_init_master(esy_i2c_t *i2c,
                                     const esy_i2c_config_t *cfg) {
  if (!i2c || !cfg) return ESY_ERR_INVALID_ARG;
  if (cfg->phys_index >= RP2040_I2C_MAX) return ESY_ERR_INVALID_ARG;
  if (cfg->addr_mode != ESY_I2C_ADDR_7BIT &&
      cfg->addr_mode != ESY_I2C_ADDR_10BIT) return ESY_ERR_INVALID_ARG;
  if (cfg->addr_mode == ESY_I2C_ADDR_10BIT) return ESY_ERR_NOT_SUPPORTED;
  esy_err_t ret = esy_hb_i2c_claim(i2c, cfg->phys_index);
  if (ret != ESY_OK) return ret;
  rp2040_i2c_ctx_t *c = &s_ctx.channel[cfg->phys_index];
  c->role = ESY_I2C_ROLE_MASTER;
  c->speed_hz = cfg->speed_hz ? cfg->speed_hz : 100000u;
  c->addr_mode = cfg->addr_mode;

  i2c_init(c->inst, c->speed_hz);

  c->enabled = true;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_validate_slave_config(
    const esy_i2c_slave_config_t *cfg) {
  if (cfg == NULL || cfg->phys_index >= RP2040_I2C_MAX ||
      (cfg->addr_mode != ESY_I2C_ADDR_7BIT &&
       cfg->addr_mode != ESY_I2C_ADDR_10BIT)) return ESY_ERR_INVALID_ARG;
  if (cfg->addr_mode == ESY_I2C_ADDR_10BIT) return ESY_ERR_NOT_SUPPORTED;
  if (cfg->address > ESY_I2C_ADDR_7BIT_MAX) return ESY_ERR_INVALID_ARG;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_init_slave(esy_i2c_t *i2c,
                                    const esy_i2c_slave_config_t *cfg) {
  if (!i2c || !cfg) return ESY_ERR_INVALID_ARG;
  esy_err_t ret = esy_hb_i2c_validate_slave_config(cfg);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_i2c_claim(i2c, cfg->phys_index);
  if (ret != ESY_OK) return ret;
  rp2040_i2c_ctx_t *c = &s_ctx.channel[cfg->phys_index];

  c->role = ESY_I2C_ROLE_SLAVE;
  c->slave_addr = cfg->address;
  c->slave_addr_mode = cfg->addr_mode;
  c->receive_cb = cfg->receive_cb;
  c->request_cb = cfg->request_cb;
  c->finish_cb = cfg->finish_cb;

  /* Slave setup does not release the reset asserted by SDK master deinit.
   * Establish the controller clock/reset baseline before installing its IRQ. */
  i2c_init(c->inst, 100000u);
  c->enabled = true;
  i2c_slave_init(c->inst, (uint8_t)c->slave_addr, rp2040_i2c_slave_isr);

  uint irq_num = (uint)(I2C0_IRQ + c->phys_index);
  irq_set_priority(irq_num, cfg->inner.irq_priority);

  return ESY_OK;
}

esy_err_t esy_hb_i2c_suspend(esy_i2c_t *i2c) {
  if (i2c == NULL) return ESY_ERR_INVALID_ARG;
  rp2040_i2c_ctx_t *c = i2c_context(i2c);
  if (c == NULL) return ESY_ERR_STATE;
  if (esy_hb_i2c_is_busy(i2c)) return ESY_ERR_BUSY;
  if (!c->enabled) return ESY_OK;

  if (c->role == ESY_I2C_ROLE_MASTER) {
    i2c_deinit(c->inst);
  } else {
    i2c_slave_deinit(c->inst);
    c->receive_cb = NULL;
    c->request_cb = NULL;
    c->finish_cb = NULL;
    c->slave_addr = 0;
  }
  c->enabled = false;
  c->transfer_active = false;
  c->callback_active = false;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_deinit(esy_i2c_t *i2c) {
  if (i2c == NULL) return ESY_ERR_INVALID_ARG;
  if (i2c->ctx == NULL) return ESY_OK;
  rp2040_i2c_ctx_t *c = i2c_context(i2c);
  if (c == NULL) return ESY_ERR_STATE;
  esy_err_t ret = esy_hb_i2c_suspend(i2c);
  if (ret != ESY_OK) return ret;
  i2c->ctx = NULL;
  atomic_store_explicit(&c->bound_i2c, NULL, memory_order_release);
  return ESY_OK;
}

int32_t esy_hb_i2c_write_blocking(esy_i2c_t *i2c, uint16_t addr,
                                      const uint8_t *data, size_t len) {
  const esy_i2c_xfer_t transfer = {.addr = addr, .tx = data, .tx_len = len};
  return esy_hb_i2c_transfer(i2c, &transfer);
}

int32_t esy_hb_i2c_read_blocking(esy_i2c_t *i2c, uint16_t addr,
                                     uint8_t *data, size_t len) {
  const esy_i2c_xfer_t transfer = {.addr = addr, .rx = data, .rx_len = len};
  return esy_hb_i2c_transfer(i2c, &transfer);
}

esy_err_t esy_hb_i2c_write_dma(esy_i2c_t *i2c, uint16_t addr,
                                   esy_dma_config_t *dma_cfg) {
  (void)addr;
  if (!i2c || !i2c->ctx || !dma_cfg) return ESY_ERR_INVALID_ARG;
  const rp2040_i2c_ctx_t *c = (const rp2040_i2c_ctx_t *)i2c->ctx;
  if (c->role != ESY_I2C_ROLE_MASTER) return ESY_ERR_INVALID_ARG;

  /* Raw payload DMA cannot encode a complete I2C transaction and retire STOP,
   * NACK and pending callbacks. Reject before touching hardware or storage. */
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_i2c_read_dma(esy_i2c_t *i2c, uint16_t addr,
                                  esy_dma_config_t *dma_cfg) {
  (void)addr;
  if (!i2c || !i2c->ctx || !dma_cfg) return ESY_ERR_INVALID_ARG;
  const rp2040_i2c_ctx_t *c = (const rp2040_i2c_ctx_t *)i2c->ctx;
  if (c->role != ESY_I2C_ROLE_MASTER) return ESY_ERR_INVALID_ARG;

  /* Read commands need bounded FIFO servicing and an explicit final STOP.
   * No DMA buffer or callback is borrowed by this unsupported operation. */
  return ESY_ERR_NOT_SUPPORTED;
}

int32_t esy_hb_i2c_transfer(esy_i2c_t *i2c,
                                const esy_i2c_xfer_t *xfer) {
  if (i2c == NULL || xfer == NULL ||
      (xfer->tx_len == 0u && xfer->rx_len == 0u) ||
      (xfer->tx_len > 0u && xfer->tx == NULL) ||
      (xfer->rx_len > 0u && xfer->rx == NULL) ||
      xfer->tx_len > INT32_MAX ||
      xfer->rx_len > (size_t)INT32_MAX - xfer->tx_len ||
      xfer->timeout_ms > INT32_MAX) return ESY_ERR_INVALID_ARG;
  rp2040_i2c_ctx_t *c = i2c_context(i2c);
  if (c == NULL) return ESY_ERR_STATE;
  if (c->role != ESY_I2C_ROLE_MASTER) return ESY_ERR_INVALID_ARG;
  if (c->addr_mode == ESY_I2C_ADDR_10BIT) return ESY_ERR_NOT_SUPPORTED;
  if (xfer->addr > ESY_I2C_ADDR_7BIT_MAX) return ESY_ERR_INVALID_ARG;
  if (!c->enabled) return ESY_ERR_STATE;
  if (c->transfer_active) return ESY_ERR_BUSY;

  uint8_t addr7 = (uint8_t)xfer->addr;
  int32_t written = 0;
  int32_t read = 0;
  const absolute_time_t deadline = make_timeout_time_ms(
      xfer->timeout_ms == 0u ? ESY_I2C_DEFAULT_TIMEOUT_MS : xfer->timeout_ms);
  c->transfer_active = true;

  if (xfer->tx && xfer->tx_len) {
    bool nostop = (xfer->repeated_start && xfer->rx && xfer->rx_len);
    int32_t r = (int32_t)i2c_write_blocking_until(
        c->inst, addr7, xfer->tx, xfer->tx_len, nostop, deadline);
    if (r < 0) {
      c->transfer_active = false;
      return r == PICO_ERROR_TIMEOUT ? ESY_ERR_TIMEOUT : ESY_ERR_IO;
    }
    written = r;
  }

  if (xfer->rx && xfer->rx_len) {
    /* Keep the original deadline for the read phase, including when a
     * repeated-START write used the remaining budget. Timeout bus-state
     * recovery remains native/board-specific. */
    int32_t r = (int32_t)i2c_read_blocking_until(
        c->inst, addr7, xfer->rx, xfer->rx_len, false, deadline);
    if (r < 0) {
      c->transfer_active = false;
      return r == PICO_ERROR_TIMEOUT ? ESY_ERR_TIMEOUT : ESY_ERR_IO;
    }
    read = r;
  }

  (void)xfer->prefer_dma;

  c->transfer_active = false;
  return time_reached(deadline) ? ESY_ERR_TIMEOUT : written + read;
}

esy_err_t esy_hb_i2c_set_speed(esy_i2c_t *i2c, uint32_t speed_hz) {
  if (!i2c || !i2c->ctx || !speed_hz) return ESY_ERR_INVALID_ARG;
  rp2040_i2c_ctx_t *c = (rp2040_i2c_ctx_t *)i2c->ctx;
  if (c->role != ESY_I2C_ROLE_MASTER) return ESY_ERR_INVALID_ARG;
  if (!c->enabled) return ESY_ERR_STATE;
  if (esy_hb_i2c_is_busy(i2c)) return ESY_ERR_BUSY;
  i2c_set_baudrate(c->inst, speed_hz);
  c->speed_hz = speed_hz;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_abort(esy_i2c_t *i2c) {
  (void)i2c;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_i2c_recover_bus(esy_i2c_t *i2c) {
  (void)i2c;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_i2c_get_caps(const esy_i2c_t *i2c,
                                  uint32_t *out_caps) {
  if (!i2c || !i2c->ctx || !out_caps) return ESY_ERR_INVALID_ARG;
  const rp2040_i2c_ctx_t *c = (const rp2040_i2c_ctx_t *)i2c->ctx;
  *out_caps = c->role == ESY_I2C_ROLE_MASTER
                  ? (ESY_I2C_CAP_MASTER_TRANSFER | ESY_I2C_CAP_REPEATED_START)
                  : 0u;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_get_phys_index(const esy_i2c_t *i2c,
                                        uint8_t *out_index) {
  if (!i2c || !i2c->ctx || !out_index) return ESY_ERR_INVALID_ARG;
  const rp2040_i2c_ctx_t *c = (const rp2040_i2c_ctx_t *)i2c->ctx;
  *out_index = c->phys_index;
  return ESY_OK;
}

static void rp2040_i2c_slave_isr(i2c_inst_t *i2c, i2c_slave_event_t event) {
  for (uint8_t i = 0; i < RP2040_I2C_MAX; ++i) {
    rp2040_i2c_ctx_t *c = &s_ctx.channel[i];
    if (!c->enabled || c->inst != i2c || c->role != ESY_I2C_ROLE_SLAVE) {
      continue;
    }

    esy_i2c_t *h = c->bound_i2c;
    c->callback_active = true;
    switch (event) {
      case I2C_SLAVE_RECEIVE: {
        if (c->receive_cb && h) {
          uint8_t b = i2c_read_byte_raw(i2c);
          c->receive_cb(h, &b, 1);
        }
      } break;
      case I2C_SLAVE_REQUEST: {
        if (c->request_cb && h) {
          uint8_t b = c->request_cb(h);
          i2c_write_byte_raw(i2c, b);
        }
      } break;
      case I2C_SLAVE_FINISH: {
        if (c->finish_cb && h) {
          c->finish_cb(h);
        }
      } break;
      default:
        break;
    }
    c->callback_active = false;
  }
}
