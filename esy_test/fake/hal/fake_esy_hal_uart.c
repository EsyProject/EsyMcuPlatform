/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_uart.h"

#include <string.h>

static fake_esy_hal_uart_slot_t
    s_slots[FAKE_ESY_HAL_UART_MAX_INSTANCES];

static fake_esy_hal_uart_slot_t *fake_esy_hal_uart_get_slot(
    esy_uart_t *uart, bool create_if_missing) {
  if (uart == NULL) {
    return NULL;
  }

  for (uint32_t i = 0u; i < FAKE_ESY_HAL_UART_MAX_INSTANCES; i++) {
    if (s_slots[i].used && s_slots[i].uart == uart) {
      return &s_slots[i];
    }
  }

  if (!create_if_missing) {
    return NULL;
  }

  for (uint32_t i = 0u; i < FAKE_ESY_HAL_UART_MAX_INSTANCES; i++) {
    if (!s_slots[i].used) {
      memset(&s_slots[i], 0, sizeof(s_slots[i]));
      s_slots[i].used = true;
      s_slots[i].uart = uart;
      s_slots[i].send_dma_result = ESY_ERR_NOT_SUPPORTED;
      return &s_slots[i];
    }
  }

  return NULL;
}

void fake_esy_hal_uart_reset(void) { memset(s_slots, 0, sizeof(s_slots)); }

fake_esy_hal_uart_slot_t *fake_esy_hal_uart_find(esy_uart_t *uart) {
  return fake_esy_hal_uart_get_slot(uart, false);
}

void fake_esy_hal_uart_set_rx_data(esy_uart_t *uart, const uint8_t *data,
                                        size_t len) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, true);
  if (slot != NULL) {
    slot->rx_data = data;
    slot->rx_len = len;
    slot->rx_pos = 0u;
  }
}

void fake_esy_hal_uart_set_rx_text(esy_uart_t *uart, const char *text) {
  fake_esy_hal_uart_set_rx_data(uart, (const uint8_t *)text,
                                     text != NULL ? strlen(text) : 0u);
}

void fake_esy_hal_uart_set_send_dma_result(esy_uart_t *uart,
                                                esy_err_t ret) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, true);
  if (slot != NULL) {
    slot->send_dma_result = ret;
  }
}

void fake_esy_hal_uart_complete_last_dma(esy_uart_t *uart) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot == NULL || !slot->dma_pending || slot->last_dma_callback == NULL) {
    return;
  }

  void (*callback)(void *arg) = slot->last_dma_callback;
  void *callback_arg = slot->last_dma_callback_arg;
  slot->tx_busy = slot->keep_tx_busy_after_dma;
  slot->dma_pending = false;
  slot->last_dma_callback = NULL;
  slot->last_dma_callback_arg = NULL;
  slot->last_dma_error_callback = NULL;
  slot->last_dma_error_callback_arg = NULL;
  callback(callback_arg);
}

void fake_esy_hal_uart_fail_last_dma(esy_uart_t *uart) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot == NULL || !slot->dma_pending || slot->last_dma_error_callback == NULL) {
    return;
  }

  void (*callback)(void *arg) = slot->last_dma_error_callback;
  void *callback_arg = slot->last_dma_error_callback_arg;
  slot->tx_busy = slot->keep_tx_busy_after_dma;
  slot->dma_pending = false;
  slot->last_dma_callback = NULL;
  slot->last_dma_callback_arg = NULL;
  slot->last_dma_error_callback = NULL;
  slot->last_dma_error_callback_arg = NULL;
  callback(callback_arg);
}

size_t fake_esy_hal_uart_push_rx_stream(esy_uart_t *uart,
                                             const uint8_t *data, size_t len) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot == NULL || data == NULL || slot->rx_stream_storage == NULL) {
    return 0u;
  }

  size_t free_bytes = slot->rx_stream_storage_size - slot->rx_stream_used;
  size_t copy_len = len < free_bytes ? len : free_bytes;
  memcpy(slot->rx_stream_storage + slot->rx_stream_used, data, copy_len);
  slot->rx_stream_used += copy_len;
  if (copy_len > 0u && slot->rx_stream_callback != NULL) {
    slot->rx_stream_callback(uart, ESY_UART_RX_STREAM_EVENT_DATA,
                             slot->rx_stream_user_data);
  }
  if (copy_len < len && slot->rx_stream_callback != NULL) {
    slot->rx_stream_callback(uart, ESY_UART_RX_STREAM_EVENT_OVERFLOW,
                             slot->rx_stream_user_data);
  }
  return copy_len;
}

size_t fake_esy_hal_uart_copy_last_tx(esy_uart_t *uart, uint8_t *out,
                                           size_t out_cap) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot == NULL || out == NULL) {
    return 0u;
  }

  size_t copy_len = slot->last_tx_len;
  if (copy_len > out_cap) {
    copy_len = out_cap;
  }
  memcpy(out, slot->last_tx, copy_len);
  return copy_len;
}

void esy_hal_uart_default_cfg(esy_uart_config_t *cfg) {
  if (cfg != NULL) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->baudrate = 115200u;
    cfg->data_bits = 8u;
    cfg->stop_bits = 1u;
    cfg->parity = ESY_UART_PARITY_NONE;
  }
}

esy_err_t esy_hal_uart_init(esy_uart_t *uart,
                                 const esy_uart_config_t *cfg) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, true);
  if (slot == NULL || cfg == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  slot->cfg = *cfg;
  slot->phys_index = cfg->phys_index;
  slot->initialized = true;
  return ESY_OK;
}

esy_err_t esy_hal_uart_deinit(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot != NULL) {
    if (slot->dma_pending || slot->tx_busy || slot->rx_busy || slot->rx_stream_storage != NULL)
      return ESY_ERR_BUSY;
    slot->deinitialized = true;
    slot->initialized = false;
  }
  return ESY_OK;
}

int32_t esy_hal_uart_send_blocking(esy_uart_t *uart, const uint8_t *data,
                                        size_t len) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, true);
  if (slot == NULL || (data == NULL && len > 0u)) {
    return ESY_ERR_INVALID_ARG;
  }

  size_t copy_len = len;
  if (copy_len > sizeof(slot->last_tx)) {
    copy_len = sizeof(slot->last_tx);
  }
  if (copy_len > 0u) {
    memcpy(slot->last_tx, data, copy_len);
  }
  slot->last_tx_len = copy_len;
  slot->send_count++;
  if (slot->send_hook) slot->send_hook(uart);
  return slot->override_send_result ? slot->send_result : (int32_t)len;
}

int32_t esy_hal_uart_receive_blocking(esy_uart_t *uart, uint8_t *data,
                                           size_t len) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot == NULL || data == NULL || len == 0u ||
      slot->rx_pos >= slot->rx_len) {
    return 0;
  }

  *data = slot->rx_data[slot->rx_pos++];
  slot->receive_count++;
  return 1;
}

esy_err_t esy_hal_uart_send_dma(esy_uart_t *uart,
                                     esy_dma_config_t *dma_cfg) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, true);
  if (slot == NULL || dma_cfg == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  if (slot->dma_pending || slot->tx_busy) return ESY_ERR_BUSY;
  slot->send_dma_count++;
  if (slot->send_dma_result != ESY_OK && !slot->retain_failed_submission)
    return slot->send_dma_result;
  slot->dma_pending = true;
  slot->last_dma_src = dma_cfg->src;
  slot->last_dma_len = dma_cfg->length;
  slot->last_dma_callback = dma_cfg->callback;
  slot->last_dma_callback_arg = dma_cfg->callback_arg;
  slot->last_dma_error_callback = dma_cfg->error_callback;
  slot->last_dma_error_callback_arg = dma_cfg->error_callback_arg;
  if (slot->send_dma_result == ESY_OK || slot->retain_failed_submission) {
    slot->tx_busy = true;
  }
  if (slot->send_dma_hook) slot->send_dma_hook(uart);
  return slot->send_dma_result;
}

esy_err_t esy_hal_uart_receive_dma(esy_uart_t *uart,
                                        esy_dma_config_t *dma_cfg) {
  (void)uart;
  (void)dma_cfg;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_uart_poll(esy_uart_t *uart) {
  fake_esy_hal_uart_slot_t *slot = fake_esy_hal_uart_get_slot(uart, true);
  if (slot == NULL) return ESY_ERR_INVALID_ARG;
  slot->poll_count++;
  if (slot->poll_result == ESY_OK && slot->poll_hook != NULL) slot->poll_hook(uart);
  return slot->poll_result;
}

esy_err_t esy_hal_uart_abort_dma(esy_uart_t *uart, bool is_tx) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);

  if (slot == NULL) return ESY_ERR_INVALID_ARG;
  slot->abort_count++;
  if (slot->abort_result != ESY_OK) return slot->abort_result;
  if (is_tx) {
    if (!slot->dma_pending && !slot->tx_busy) return ESY_ERR_STATE;
    slot->tx_busy = false;
    slot->dma_pending = false;
  } else {
    if (!slot->rx_busy) return ESY_ERR_STATE;
    slot->rx_busy = false;
  }
  slot->last_dma_callback = NULL;
  slot->last_dma_callback_arg = NULL;
  slot->last_dma_error_callback = NULL;
  slot->last_dma_error_callback_arg = NULL;
  return ESY_OK;
}

esy_err_t esy_hal_uart_rx_stream_start(
    esy_uart_t *uart, uint8_t *storage, size_t storage_size,
    esy_uart_rx_stream_callback_t callback, void *user_data) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, true);
  if (slot == NULL || storage == NULL || storage_size < 2u ||
      callback == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  ++slot->rx_stream_start_count;
  if (slot->rx_stream_storage != NULL) {
    return ESY_ERR_STATE;
  }

  esy_err_t result = slot->rx_stream_start_result;
  if (result != ESY_OK && (!slot->retain_failed_rx_start ||
      result == ESY_ERR_INVALID_ARG || result == ESY_ERR_NOT_SUPPORTED ||
      result == ESY_ERR_NOT_IMPLEMENTED)) return result;
  slot->rx_stream_storage = storage;
  slot->rx_stream_storage_size = storage_size;
  slot->rx_stream_used = 0u;
  slot->rx_stream_peek_length = 0u;
  slot->rx_stream_callback = callback;
  slot->rx_stream_user_data = user_data;
  if (slot->rx_stream_start_hook) slot->rx_stream_start_hook(uart);
  return result;
}

esy_err_t esy_hal_uart_rx_stream_peek(esy_uart_t *uart,
                                           const uint8_t **data,
                                           size_t *length) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, true);
  if (slot == NULL || data == NULL || length == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (slot->rx_stream_storage == NULL) {
    return ESY_ERR_STATE;
  }

  if (slot->rx_stream_peek_result != ESY_OK) return slot->rx_stream_peek_result;
  *data = slot->rx_stream_storage;
  *length = slot->rx_stream_used;
  slot->rx_stream_peek_length = slot->rx_stream_used;
  if (slot->rx_stream_peek_hook) slot->rx_stream_peek_hook(uart);
  return ESY_OK;
}

esy_err_t esy_hal_uart_rx_stream_consume(esy_uart_t *uart, size_t length) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot == NULL || length == 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (slot->rx_stream_storage == NULL) {
    return ESY_ERR_STATE;
  }
  if (slot->rx_stream_consume_result != ESY_OK) return slot->rx_stream_consume_result;
  if (length > slot->rx_stream_peek_length) {
    return ESY_ERR_INVALID_ARG;
  }

  memmove(slot->rx_stream_storage, slot->rx_stream_storage + length,
          slot->rx_stream_used - length);
  slot->rx_stream_used -= length;
  slot->rx_stream_peek_length = 0u;
  return ESY_OK;
}

esy_err_t esy_hal_uart_rx_stream_stop(esy_uart_t *uart) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  ++slot->rx_stream_stop_count;
  if (slot->rx_stream_stop_result != ESY_OK) return slot->rx_stream_stop_result;

  slot->rx_stream_storage = NULL;
  slot->rx_stream_storage_size = 0u;
  slot->rx_stream_used = 0u;
  slot->rx_stream_peek_length = 0u;
  slot->rx_stream_callback = NULL;
  slot->rx_stream_user_data = NULL;
  return ESY_OK;
}

void esy_hal_uart_handle_irq(uint8_t phys_index) { (void)phys_index; }

esy_err_t esy_hal_uart_get_dma_trans_count_remaining(
    const esy_uart_t *uart, size_t *out_tx_remaining,
    size_t *out_rx_remaining) {
  (void)uart;
  if (out_tx_remaining != NULL) {
    *out_tx_remaining = 0u;
  }
  if (out_rx_remaining != NULL) {
    *out_rx_remaining = 0u;
  }
  return ESY_ERR_NOT_SUPPORTED;
}

bool esy_hal_uart_is_busy(esy_uart_t *uart, bool is_tx) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, false);
  if (slot == NULL) {
    return false;
  }

  if (is_tx && slot->busy_hook != NULL) slot->busy_hook(uart);

  return is_tx ? slot->tx_busy : slot->rx_busy;
}

esy_err_t esy_hal_uart_suspend(esy_uart_t *uart) {
  (void)uart;
  return ESY_OK;
}

esy_err_t esy_hal_uart_resume(esy_uart_t *uart) {
  (void)uart;
  return ESY_OK;
}

bool esy_hal_uart_is_readable(const esy_uart_t *uart) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot((esy_uart_t *)uart, false);
  return slot != NULL && slot->rx_pos < slot->rx_len;
}

esy_err_t esy_hal_uart_set_baudrate(esy_uart_t *uart, uint32_t baudrate) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot(uart, true);
  if (slot == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  slot->cfg.baudrate = baudrate;
  return ESY_OK;
}

esy_err_t esy_hal_uart_get_caps(const esy_uart_t *uart,
                                     uint32_t *out_caps) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot((esy_uart_t *)uart, false);
  if (slot == NULL || out_caps == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  *out_caps = slot->caps;
  return ESY_OK;
}

esy_err_t esy_hal_uart_get_phys_index(const esy_uart_t *uart,
                                           uint8_t *out_index) {
  fake_esy_hal_uart_slot_t *slot =
      fake_esy_hal_uart_get_slot((esy_uart_t *)uart, false);
  if (slot == NULL || out_index == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  *out_index = slot->phys_index;
  return ESY_OK;
}

__attribute__((constructor)) static void fake_esy_hal_uart_ctor(void) {
  fake_esy_hal_uart_reset();
}
