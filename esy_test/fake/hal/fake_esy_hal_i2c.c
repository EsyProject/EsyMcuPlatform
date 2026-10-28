/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_i2c.h"
#include <limits.h>
#include <string.h>

#define FAKE_I2C_INSTANCES 4u
#define FAKE_I2C_EXPECTATIONS 128u
#define FAKE_I2C_BYTES 64u

typedef struct {
  esy_i2c_t *handle;
  esy_i2c_config_t config;
} fake_i2c_slot_t;

typedef struct {
  uint16_t addr;
  uint8_t tx[FAKE_I2C_BYTES];
  uint8_t rx[FAKE_I2C_BYTES];
  size_t tx_len;
  size_t rx_len;
  int32_t result;
  uint32_t timeout_ms;
  bool check_timeout;
  bool repeated_start;
  bool check_repeated_start;
} fake_i2c_expectation_t;

typedef struct {
  fake_i2c_slot_t slots[FAKE_I2C_INSTANCES];
  fake_i2c_expectation_t expected[FAKE_I2C_EXPECTATIONS];
  size_t queued;
  size_t consumed;
  size_t mismatches;
  size_t transfers;
  uint32_t last_timeout_ms;
} fake_esy_hal_i2c_ctx_t;

static fake_esy_hal_i2c_ctx_t s_ctx;

static fake_i2c_slot_t *find_slot(const esy_i2c_t *handle) {
  for (size_t index = 0u; index < FAKE_I2C_INSTANCES; ++index) {
    if (s_ctx.slots[index].handle == handle) return &s_ctx.slots[index];
  }
  return NULL;
}

static const esy_i2c_vtable_t k_fake_i2c_ops = {
    .write_blocking = esy_hal_i2c_write_blocking,
    .read_blocking = esy_hal_i2c_read_blocking,
    .transfer = esy_hal_i2c_transfer,
    .get_caps = esy_hal_i2c_get_caps,
};

void fake_esy_hal_i2c_reset(void) { memset(&s_ctx, 0, sizeof(s_ctx)); }

esy_err_t fake_esy_hal_i2c_expect_transfer(uint16_t addr,
    const uint8_t *tx, size_t tx_len, const uint8_t *rx, size_t rx_len,
    int32_t result) {
  if (addr > 0x3ffu || tx_len > FAKE_I2C_BYTES || rx_len > FAKE_I2C_BYTES ||
      (tx_len != 0u && tx == NULL) || (rx_len != 0u && rx == NULL) ||
      (tx_len == 0u && rx_len == 0u)) return ESY_ERR_INVALID_ARG;
  if (s_ctx.queued == FAKE_I2C_EXPECTATIONS) return ESY_ERR_NO_MEM;
  fake_i2c_expectation_t *expected = &s_ctx.expected[s_ctx.queued++];
  memset(expected, 0, sizeof(*expected));
  expected->addr = addr;
  expected->tx_len = tx_len;
  expected->rx_len = rx_len;
  expected->result = result;
  if (tx_len != 0u) memcpy(expected->tx, tx, tx_len);
  if (rx_len != 0u) memcpy(expected->rx, rx, rx_len);
  return ESY_OK;
}

void fake_esy_hal_i2c_expect_timeout(uint32_t timeout_ms) {
  if (s_ctx.queued > s_ctx.consumed) {
    s_ctx.expected[s_ctx.queued - 1u].timeout_ms = timeout_ms;
    s_ctx.expected[s_ctx.queued - 1u].check_timeout = true;
  }
}

void fake_esy_hal_i2c_expect_repeated_start(bool repeated_start) {
  if (s_ctx.queued > s_ctx.consumed) {
    s_ctx.expected[s_ctx.queued - 1u].repeated_start = repeated_start;
    s_ctx.expected[s_ctx.queued - 1u].check_repeated_start = true;
  }
}

size_t fake_esy_hal_i2c_pending_count(void) { return s_ctx.queued - s_ctx.consumed; }
size_t fake_esy_hal_i2c_mismatch_count(void) { return s_ctx.mismatches; }
size_t fake_esy_hal_i2c_transfer_count(void) { return s_ctx.transfers; }
uint32_t fake_esy_hal_i2c_last_timeout(void) { return s_ctx.last_timeout_ms; }

void esy_hal_i2c_default_cfg(esy_i2c_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->speed_hz = 100000u;
  config->addr_mode = ESY_I2C_ADDR_7BIT;
}

esy_err_t esy_hal_i2c_init(esy_i2c_t *i2c, const esy_i2c_config_t *config) {
  if (i2c == NULL || config == NULL || config->speed_hz == 0u ||
      (config->addr_mode != ESY_I2C_ADDR_7BIT &&
       config->addr_mode != ESY_I2C_ADDR_10BIT)) return ESY_ERR_INVALID_ARG;
  if (i2c->ops != NULL || find_slot(i2c) != NULL) return ESY_ERR_STATE;
  for (size_t index = 0u; index < FAKE_I2C_INSTANCES; ++index) {
    if (s_ctx.slots[index].handle == NULL) {
      s_ctx.slots[index].handle = i2c;
      s_ctx.slots[index].config = *config;
      i2c->ops = &k_fake_i2c_ops;
      return ESY_OK;
    }
  }
  return ESY_ERR_NO_MEM;
}

esy_err_t esy_hal_i2c_deinit(esy_i2c_t *i2c) {
  if (i2c == NULL) return ESY_ERR_INVALID_ARG;
  fake_i2c_slot_t *slot = find_slot(i2c);
  if (slot != NULL) memset(slot, 0, sizeof(*slot));
  memset(i2c, 0, sizeof(*i2c));
  return ESY_OK;
}

esy_err_t esy_hal_i2c_get_caps(const esy_i2c_t *i2c, uint32_t *caps) {
  if (i2c == NULL || caps == NULL) return ESY_ERR_INVALID_ARG;
  if (find_slot(i2c) == NULL) return ESY_ERR_STATE;
  *caps = ESY_I2C_CAP_MASTER_TRANSFER | ESY_I2C_CAP_REPEATED_START;
  return ESY_OK;
}

int32_t esy_hal_i2c_transfer(esy_i2c_t *i2c, const esy_i2c_xfer_t *xfer) {
  if (i2c == NULL || xfer == NULL ||
      (xfer->tx_len != 0u && xfer->tx == NULL) ||
      (xfer->rx_len != 0u && xfer->rx == NULL) ||
      (xfer->tx_len == 0u && xfer->rx_len == 0u) ||
      xfer->timeout_ms > (uint32_t)INT32_MAX) return ESY_ERR_INVALID_ARG;
  fake_i2c_slot_t *slot = find_slot(i2c);
  if (slot == NULL) return ESY_ERR_STATE;
  uint16_t maximum = slot->config.addr_mode == ESY_I2C_ADDR_10BIT ? 0x3ffu : 0x7fu;
  if (xfer->addr > maximum) return ESY_ERR_INVALID_ARG;
  ++s_ctx.transfers;
  s_ctx.last_timeout_ms = xfer->timeout_ms;
  if (s_ctx.consumed == s_ctx.queued) {
    ++s_ctx.mismatches;
    return ESY_ERR_IO;
  }
  const fake_i2c_expectation_t *expected = &s_ctx.expected[s_ctx.consumed++];
  if (xfer->addr != expected->addr || xfer->tx_len != expected->tx_len ||
      xfer->rx_len != expected->rx_len ||
      (xfer->tx_len != 0u && memcmp(xfer->tx, expected->tx, xfer->tx_len) != 0) ||
      (expected->check_timeout && xfer->timeout_ms != expected->timeout_ms) ||
      (expected->check_repeated_start &&
       xfer->repeated_start != expected->repeated_start)) {
    ++s_ctx.mismatches;
    return ESY_ERR_IO;
  }
  if (expected->result >= 0 && xfer->rx_len != 0u) {
    size_t transferred = (size_t)expected->result;
    size_t copied = transferred > xfer->tx_len ? transferred - xfer->tx_len : 0u;
    if (copied > xfer->rx_len) copied = xfer->rx_len;
    if (copied != 0u) memcpy(xfer->rx, expected->rx, copied);
  }
  return expected->result;
}

int32_t esy_hal_i2c_write_blocking(esy_i2c_t *i2c, uint16_t addr,
    const uint8_t *data, size_t length) {
  const esy_i2c_xfer_t xfer = {.addr = addr, .tx = data, .tx_len = length};
  return esy_hal_i2c_transfer(i2c, &xfer);
}

int32_t esy_hal_i2c_read_blocking(esy_i2c_t *i2c, uint16_t addr,
    uint8_t *data, size_t length) {
  const esy_i2c_xfer_t xfer = {.addr = addr, .rx = data, .rx_len = length};
  return esy_hal_i2c_transfer(i2c, &xfer);
}
