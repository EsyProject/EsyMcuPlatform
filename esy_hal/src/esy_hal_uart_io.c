/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/defs/coordination/esy_atomic_defs.h"
#include "esy_hal_uart_internal.h"
#include "esy_hal/esy_hal_uart.h"
#include "esy_hal/esy_hal_uart_device.h"

static esy_err_t uart_acquire(esy_uart_t *uart) {
  return (esy_hal_uart_runtime(uart)->device == NULL) ? ESY_OK : esy_device_get(esy_hal_uart_runtime(uart)->device);
}

static void uart_release(esy_uart_t *uart) {
  if (esy_hal_uart_runtime(uart)->device != NULL) {
    (void)esy_device_put(esy_hal_uart_runtime(uart)->device);
  }
}

static esy_uart_device_binding_t *uart_binding(esy_uart_t *uart) {
  if ((uart == NULL) || (esy_hal_uart_runtime(uart)->device == NULL)) return NULL;
  return (esy_uart_device_binding_t *)esy_hal_uart_runtime(uart)->device->driver_ctx;
}

enum { UART_DMA_IDLE = 0u, UART_DMA_ACTIVE = 1u, UART_DMA_CALLBACK = 2u };

static esy_uart_dma_runtime_t *uart_dma_runtime(esy_uart_t *uart, bool is_tx) {
  return is_tx ? &esy_hal_uart_runtime(uart)->tx_dma : &esy_hal_uart_runtime(uart)->rx_dma;
}

static void uart_dma_release(esy_uart_dma_runtime_t *runtime) {
  esy_uart_t *uart = runtime->owner;
  esy_uart_device_binding_t *binding = uart_binding(uart);
  runtime->complete_cb = NULL;
  runtime->complete_arg = NULL;
  runtime->half_cb = NULL;
  runtime->half_arg = NULL;
  runtime->error_cb = NULL;
  runtime->error_arg = NULL;
  runtime->circular = false;
  if (binding != NULL) {
    bool *lease = runtime == &esy_hal_uart_runtime(uart)->tx_dma
        ? &binding->tx_dma_ref_held : &binding->rx_dma_ref_held;
    if (*lease) {
      *lease = false;
      (void)esy_device_put_from_isr(&binding->device);
    }
  }
  /* Keep owner/fields borrowed until callback return and lease release. */
  atomic_store_explicit(&runtime->state, UART_DMA_IDLE, memory_order_release);
}

static void uart_dma_notify(esy_uart_dma_runtime_t *runtime,
                            bool error, bool half) {
  uint32_t expected = UART_DMA_ACTIVE;
  if (!atomic_compare_exchange_strong_explicit(&runtime->state, &expected,
      UART_DMA_CALLBACK, memory_order_acq_rel, memory_order_acquire)) return;
  esy_dma_callback_t callback = error ? runtime->error_cb
      : (half ? runtime->half_cb : runtime->complete_cb);
  void *arg = error ? runtime->error_arg
      : (half ? runtime->half_arg : runtime->complete_arg);
  if (callback != NULL) callback(arg);
  if (!error && (half || runtime->circular)) {
    atomic_store_explicit(&runtime->state, UART_DMA_ACTIVE, memory_order_release);
  } else {
    uart_dma_release(runtime);
  }
}

static void uart_dma_complete(void *arg) { uart_dma_notify(arg, false, false); }
static void uart_dma_half(void *arg) { uart_dma_notify(arg, false, true); }
static void uart_dma_error(void *arg) { uart_dma_notify(arg, true, false); }

/* ================ blocking I/O ================ */

extern int32_t esy_hal_uart_send_blocking(esy_uart_t *uart, const uint8_t *data,
                                        size_t len) {
  if (!uart || !data || !len) return ESY_ERR_INVALID_ARG;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->tx_dma.state, memory_order_acquire) !=
      UART_DMA_IDLE) return ESY_ERR_BUSY;
  if (uart->ops == NULL || uart->ops->send_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  esy_err_t ret = uart_acquire(uart);
  if (ret != ESY_OK) return ret;
  int32_t count = uart->ops->send_blocking(uart, data, len);
  uart_release(uart);
  return count;
}

extern int32_t esy_hal_uart_receive_blocking(esy_uart_t *uart, uint8_t *data,
                                           size_t len) {
  if (!uart || !data || !len) return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_runtime(uart)->rx_stream_owned) return ESY_ERR_BUSY;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) !=
      UART_DMA_IDLE) return ESY_ERR_BUSY;
  if (uart->ops == NULL || uart->ops->receive_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  esy_err_t ret = uart_acquire(uart);
  if (ret != ESY_OK) return ret;
  int32_t count = uart->ops->receive_blocking(uart, data, len);
  uart_release(uart);
  return count;
}

static esy_err_t uart_submit_dma(esy_uart_t *uart,
                                esy_dma_config_t *config, bool is_tx) {
  if (uart == NULL || config == NULL || config->length == 0u ||
      (is_tx ? config->src == NULL : config->dst == NULL))
    return ESY_ERR_INVALID_ARG;
  if (!is_tx && esy_hal_uart_runtime(uart)->rx_stream_owned) return ESY_ERR_BUSY;
  if (uart->ops == NULL) return ESY_ERR_NOT_SUPPORTED;
  esy_err_t (*submit)(esy_uart_t *, esy_dma_config_t *) =
      is_tx ? uart->ops->send_dma : uart->ops->receive_dma;
  if (submit == NULL || (uart->ops == NULL || uart->ops->abort_dma == NULL)) return ESY_ERR_NOT_SUPPORTED;
  esy_uart_dma_runtime_t *runtime = uart_dma_runtime(uart, is_tx);
  if (atomic_load_explicit(&runtime->state, memory_order_acquire) !=
      UART_DMA_IDLE) return ESY_ERR_BUSY;
  esy_err_t result = uart_acquire(uart);
  if (result != ESY_OK) return result;
  esy_uart_device_binding_t *binding = uart_binding(uart);
  if (binding != NULL) {
    if (is_tx) binding->tx_dma_ref_held = true;
    else binding->rx_dma_ref_held = true;
  }
  runtime->owner = uart;
  runtime->circular = config->circular;
  runtime->complete_cb = config->callback;
  runtime->complete_arg = config->callback_arg;
  runtime->half_cb = config->half_callback;
  runtime->half_arg = config->half_callback_arg;
  runtime->error_cb = config->error_callback;
  runtime->error_arg = config->error_callback_arg;
  esy_dma_config_t transfer = *config;
  transfer.callback = uart_dma_complete;
  transfer.callback_arg = runtime;
  transfer.half_callback = uart_dma_half;
  transfer.half_callback_arg = runtime;
  transfer.error_callback = uart_dma_error;
  transfer.error_callback_arg = runtime;
  atomic_store_explicit(&runtime->state, UART_DMA_ACTIVE, memory_order_release);
  result = submit(uart, &transfer);
  if (result != ESY_OK && atomic_load_explicit(&runtime->state,
      memory_order_acquire) != UART_DMA_IDLE) {
    esy_err_t cleanup = uart->ops->abort_dma(uart, is_tx);
    if (cleanup != ESY_OK) return cleanup;
    uint32_t expected = UART_DMA_ACTIVE;
    if (atomic_compare_exchange_strong_explicit(&runtime->state, &expected,
        UART_DMA_CALLBACK, memory_order_acq_rel, memory_order_acquire))
      uart_dma_release(runtime);
  }
  return result;
}

extern esy_err_t esy_hal_uart_send_dma(esy_uart_t *uart,
                                     esy_dma_config_t *config) {
  return uart_submit_dma(uart, config, true);
}

extern esy_err_t esy_hal_uart_receive_dma(esy_uart_t *uart,
                                        esy_dma_config_t *config) {
  return uart_submit_dma(uart, config, false);
}

extern esy_err_t esy_hal_uart_rx_stream_start(
    esy_uart_t *uart, uint8_t *storage, size_t storage_size,
    esy_uart_rx_stream_callback_t callback, void *user_data) {
  esy_uart_device_binding_t *binding;
  esy_err_t ret;

  if (!uart || !storage || storage_size < 2u || !callback) {
    return ESY_ERR_INVALID_ARG;
  }
  if (uart->ops == NULL || uart->ops->rx_stream_start == NULL ||
      uart->ops->rx_stream_stop == NULL)
    return ESY_ERR_NOT_SUPPORTED;
  if (esy_hal_uart_runtime(uart)->rx_stream_owned) return ESY_ERR_STATE;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) !=
      UART_DMA_IDLE) return ESY_ERR_BUSY;
  binding = uart_binding(uart);
  if ((binding != NULL) && binding->rx_stream_ref_held) return ESY_ERR_STATE;
  ret = uart_acquire(uart);
  if (ret != ESY_OK) return ret;
  ret = uart->ops->rx_stream_start(uart, storage, storage_size, callback,
                                  user_data);
  if (ret == ESY_OK) {
    esy_hal_uart_runtime(uart)->rx_stream_owned = true;
    if (binding != NULL) binding->rx_stream_ref_held = true;
  } else {
    if (ret != ESY_ERR_INVALID_ARG && ret != ESY_ERR_NOT_SUPPORTED &&
        ret != ESY_ERR_NOT_IMPLEMENTED) {
      esy_hal_uart_runtime(uart)->rx_stream_owned = true;
      if (binding != NULL) {
        esy_err_t cleanup = uart->ops->rx_stream_stop(uart);
        if (cleanup != ESY_OK) {
          binding->rx_stream_ref_held = true;
          return cleanup;
        }
        esy_hal_uart_runtime(uart)->rx_stream_owned = false;
      }
    }
    uart_release(uart);
  }
  return ret;
}

extern esy_err_t esy_hal_uart_rx_stream_peek(esy_uart_t *uart,
                                                  const uint8_t **data,
                                                  size_t *length) {
  if (!uart || !data || !length) return ESY_ERR_INVALID_ARG;
  *data = NULL;
  *length = 0u;
  if (uart->ops == NULL || uart->ops->rx_stream_peek == NULL) return ESY_ERR_NOT_SUPPORTED;
  return uart->ops->rx_stream_peek(uart, data, length);
}

extern esy_err_t esy_hal_uart_rx_stream_consume(esy_uart_t *uart,
                                                     size_t length) {
  if (!uart || length == 0u) return ESY_ERR_INVALID_ARG;
  if (uart->ops == NULL || uart->ops->rx_stream_consume == NULL) return ESY_ERR_NOT_SUPPORTED;
  return uart->ops->rx_stream_consume(uart, length);
}

extern esy_err_t esy_hal_uart_rx_stream_stop(esy_uart_t *uart) {
  esy_uart_device_binding_t *binding;
  esy_err_t ret;

  if (!uart) return ESY_ERR_INVALID_ARG;
  if (uart->ops == NULL || uart->ops->rx_stream_stop == NULL) return ESY_ERR_NOT_SUPPORTED;
  binding = uart_binding(uart);
  ret = uart->ops->rx_stream_stop(uart);
  if (ret == ESY_OK) esy_hal_uart_runtime(uart)->rx_stream_owned = false;
  if ((ret == ESY_OK) && (binding != NULL) && binding->rx_stream_ref_held) {
    binding->rx_stream_ref_held = false;
    uart_release(uart);
  }
  return ret;
}

extern esy_err_t esy_hal_uart_abort_dma(esy_uart_t *uart, bool is_tx) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  esy_uart_dma_runtime_t *runtime = uart_dma_runtime(uart, is_tx);
  uint32_t state = atomic_load_explicit(&runtime->state, memory_order_acquire);
  if (state == UART_DMA_IDLE) return ESY_ERR_STATE;
  if (state == UART_DMA_CALLBACK) return ESY_ERR_BUSY;
  if (uart->ops == NULL || uart->ops->abort_dma == NULL) return ESY_ERR_NOT_SUPPORTED;
  esy_err_t result = uart->ops->abort_dma(uart, is_tx);
  if (result != ESY_OK) return result;
  uint32_t expected = UART_DMA_ACTIVE;
  if (atomic_compare_exchange_strong_explicit(&runtime->state, &expected,
      UART_DMA_CALLBACK, memory_order_acq_rel, memory_order_acquire))
    uart_dma_release(runtime);
  return ESY_OK;
}

esy_err_t esy_hal_uart_poll(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->tx_dma.state, memory_order_acquire) == UART_DMA_CALLBACK ||
      atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) == UART_DMA_CALLBACK)
    return ESY_ERR_BUSY;
  if (uart->ops == NULL || uart->ops->poll == NULL) return ESY_ERR_STATE;
  return uart->ops->poll(uart);
}

extern esy_err_t esy_hal_uart_get_dma_trans_count_remaining(
    const esy_uart_t *uart, size_t *out_tx_remaining,
    size_t *out_rx_remaining) {
  if (!uart) return ESY_ERR_INVALID_ARG;
  if ((out_tx_remaining == NULL) && (out_rx_remaining == NULL))
    return ESY_ERR_INVALID_ARG;
  if (uart->ops == NULL || uart->ops->get_dma_trans_count_remaining == NULL)
    return ESY_ERR_NOT_SUPPORTED;
  return uart->ops->get_dma_trans_count_remaining(uart, out_tx_remaining,
                                                out_rx_remaining);
}

/* ================ power/runtime ================ */

extern esy_err_t esy_hal_uart_suspend(esy_uart_t *uart) {
  if (!uart) return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_runtime(uart)->device != NULL) return ESY_ERR_STATE;
  if (esy_hal_uart_is_busy(uart, true) || esy_hal_uart_is_busy(uart, false))
    return ESY_ERR_BUSY;
  if (uart->ops == NULL || uart->ops->suspend == NULL) return ESY_ERR_NOT_SUPPORTED;
  return uart->ops->suspend(uart);
}

extern esy_err_t esy_hal_uart_resume(esy_uart_t *uart) {
  if (!uart) return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_runtime(uart)->device != NULL) return ESY_ERR_STATE;
  if (uart->ops == NULL || uart->ops->resume == NULL) return ESY_ERR_NOT_SUPPORTED;
  return uart->ops->resume(uart);
}

/* ================ queries/config ================ */

extern bool esy_hal_uart_is_busy(esy_uart_t *uart, bool is_tx) {
  if (!uart) return false;
  if (!is_tx && esy_hal_uart_runtime(uart)->rx_stream_owned) return true;
  if (atomic_load_explicit(&uart_dma_runtime(uart, is_tx)->state,
      memory_order_acquire) != UART_DMA_IDLE) return true;
  return (uart->ops != NULL && uart->ops->is_busy != NULL) && uart->ops->is_busy(uart, is_tx);
}

extern bool esy_hal_uart_is_readable(const esy_uart_t *uart) {
  if (!uart || (uart->ops == NULL || uart->ops->is_readable == NULL)) return false;
  return uart->ops->is_readable(uart);
}

extern esy_err_t esy_hal_uart_set_baudrate(esy_uart_t *uart,
                                                uint32_t baudrate) {
  esy_err_t ret;

  if (!uart || !baudrate) return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_is_busy(uart, true) || esy_hal_uart_is_busy(uart, false))
    return ESY_ERR_BUSY;
  if (uart->ops == NULL || uart->ops->set_baudrate == NULL) return ESY_ERR_NOT_SUPPORTED;
  ret = uart_acquire(uart);
  if (ret != ESY_OK) return ret;
  ret = uart->ops->set_baudrate(uart, baudrate);
  uart_release(uart);
  return ret;
}

extern esy_err_t esy_hal_uart_get_caps(const esy_uart_t *uart,
                                            uint32_t *out_caps) {
  if (!uart || !out_caps) return ESY_ERR_INVALID_ARG;
  if (uart->ops == NULL || uart->ops->get_caps == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return uart->ops->get_caps(uart, out_caps);
}

/* Public APIs call ops instead of the HAL backend directly */
extern esy_err_t esy_hal_uart_get_phys_index(const esy_uart_t *uart,
                                                  uint8_t *out_index) {
  if (!uart || !out_index) return ESY_ERR_INVALID_ARG;
  if (uart->ops == NULL || uart->ops->get_phys_index == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return uart->ops->get_phys_index(uart, out_index);
}
