/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tusb.h"

#include <string.h>

typedef struct {
  fake_tinyusb_cdc_itf_state_t itfs[FAKE_TINYUSB_CDC_MAX_IFACES];
} fake_tinyusb_ctx_t;

static fake_tinyusb_ctx_t s_ctx;

static bool fake_tinyusb_itf_valid(uint8_t itf) {
  return itf < FAKE_TINYUSB_CDC_MAX_IFACES;
}

void fake_tinyusb_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  for (uint8_t itf = 0u; itf < FAKE_TINYUSB_CDC_MAX_IFACES; ++itf) {
    s_ctx.itfs[itf].write_available = FAKE_TINYUSB_FIFO_BYTES;
    s_ctx.itfs[itf].read_limit = UINT32_MAX;
    s_ctx.itfs[itf].write_limit = UINT32_MAX;
  }
}

fake_tinyusb_cdc_itf_state_t *fake_tinyusb_cdc_get_itf(uint8_t itf) {
  if (!fake_tinyusb_itf_valid(itf)) return NULL;

  return &s_ctx.itfs[itf];
}

void fake_tinyusb_cdc_set_rx_data(uint8_t itf, const uint8_t *data, uint32_t len) {
  if (!fake_tinyusb_itf_valid(itf)) return;

  fake_tinyusb_cdc_itf_state_t *state = &s_ctx.itfs[itf];
  uint32_t copy_len = len;
  if (copy_len > FAKE_TINYUSB_FIFO_BYTES) {
    copy_len = FAKE_TINYUSB_FIFO_BYTES;
  }

  if (copy_len > 0u && data != NULL) {
    memcpy(state->read_data, data, copy_len);
  }
  state->available = copy_len;
}

void tud_task(void) { s_ctx.itfs[0].task_count++; }

bool tud_cdc_n_connected(uint8_t itf) {
  if (!fake_tinyusb_itf_valid(itf)) return false;

  return s_ctx.itfs[itf].connected;
}

uint8_t tud_cdc_n_get_line_state(uint8_t itf) {
  if (!fake_tinyusb_itf_valid(itf)) return 0u;

  return s_ctx.itfs[itf].line_state;
}

uint32_t tud_cdc_n_available(uint8_t itf) {
  if (!fake_tinyusb_itf_valid(itf)) return 0u;

  return s_ctx.itfs[itf].available;
}

uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t bufsize) {
  if (!fake_tinyusb_itf_valid(itf) || buffer == NULL || bufsize == 0u) return 0u;

  fake_tinyusb_cdc_itf_state_t *state = &s_ctx.itfs[itf];
  uint32_t read_len = bufsize;
  if (state->read_overreport) return bufsize + 1u;
  if (read_len > state->read_limit) read_len = state->read_limit;
  if (read_len > state->available) {
    read_len = state->available;
  }

  memcpy(buffer, state->read_data, read_len);
  if (read_len < state->available) {
    memmove(state->read_data, &state->read_data[read_len], state->available - read_len);
  }
  state->available -= read_len;
  state->read_count++;
  state->last_read_itf = itf;
  return read_len;
}

uint32_t tud_cdc_n_write_available(uint8_t itf) {
  if (!fake_tinyusb_itf_valid(itf)) return 0u;

  return s_ctx.itfs[itf].write_available;
}

uint32_t tud_cdc_n_write(uint8_t itf, const void *buffer, uint32_t bufsize) {
  if (!fake_tinyusb_itf_valid(itf) || buffer == NULL || bufsize == 0u) return 0u;

  fake_tinyusb_cdc_itf_state_t *state = &s_ctx.itfs[itf];
  uint32_t write_len = bufsize;
  if (state->write_overreport) return bufsize + 1u;
  if (write_len > state->write_limit) write_len = state->write_limit;
  if (write_len > state->write_available) {
    write_len = state->write_available;
  }
  if (write_len > FAKE_TINYUSB_FIFO_BYTES) {
    write_len = FAKE_TINYUSB_FIFO_BYTES;
  }

  memcpy(state->written_data, buffer, write_len);
  state->write_available -= write_len;
  state->write_count++;
  state->last_write_itf = itf;
  return write_len;
}

void tud_cdc_n_write_flush(uint8_t itf) {
  if (!fake_tinyusb_itf_valid(itf)) return;

  s_ctx.itfs[itf].flush_count++;
  s_ctx.itfs[itf].last_flush_itf = itf;
}

void tud_cdc_n_read_flush(uint8_t itf) {
  if (!fake_tinyusb_itf_valid(itf)) return;
  s_ctx.itfs[itf].available = 0u;
  s_ctx.itfs[itf].read_flush_count++;
}

bool tud_cdc_n_write_clear(uint8_t itf) {
  if (!fake_tinyusb_itf_valid(itf)) return false;
  s_ctx.itfs[itf].write_available = FAKE_TINYUSB_FIFO_BYTES;
  memset(s_ctx.itfs[itf].written_data, 0, sizeof(s_ctx.itfs[itf].written_data));
  s_ctx.itfs[itf].write_clear_count++;
  return true;
}
