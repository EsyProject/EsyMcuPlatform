/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_uart.h"
#include "esy_hal_uart_internal.h"
#include "esy_hal/esy_hal_uart_device.h"

#include "esy_hal_backend/esy_hb_uart.h"
#include "esy_board/esy_board.h"
#include <string.h>

/* ================= vt_*: sole ops implementation =================
 * Notes:
 * - HAL public APIs call uart->ops->* and do not perform board or index
 * completion.
 * - vt_* functions perform required board work and HAL backend forwarding.
 */

static int32_t vt_send_blocking(esy_uart_t *uart, const uint8_t *data,
                                size_t len) {
  return esy_hb_uart_send_blocking(uart, data, len);
}

static int32_t vt_receive_blocking(esy_uart_t *uart, uint8_t *data,
                                   size_t len) {
  return esy_hb_uart_receive_blocking(uart, data, len);
}

static esy_err_t vt_send_dma(esy_uart_t *uart, esy_dma_config_t *dma_cfg) {
  if (!uart || !dma_cfg) return ESY_ERR_INVALID_ARG;

  /* Resolve the physical index and complete TX DMA metadata in the vt function
   */
  uint8_t idx;
  esy_err_t r = esy_hb_uart_get_phys_index(uart, &idx);
  if (r != ESY_OK) return r;

  if (!esy_board_uart_get_dma_info(idx, &dma_cfg->inner, true))
    return ESY_ERR_NOT_SUPPORTED;
  return esy_hb_uart_send_dma(uart, dma_cfg);
}

static esy_err_t vt_receive_dma(esy_uart_t *uart, esy_dma_config_t *dma_cfg) {
  if (!uart || !dma_cfg) return ESY_ERR_INVALID_ARG;

  /* Resolve the physical index and complete RX DMA metadata in the vt function
   */
  uint8_t idx;
  esy_err_t r = esy_hb_uart_get_phys_index(uart, &idx);
  if (r != ESY_OK) return r;

  if (!esy_board_uart_get_dma_info(idx, &dma_cfg->inner, false))
    return ESY_ERR_NOT_SUPPORTED;
  return esy_hb_uart_receive_dma(uart, dma_cfg);
}

static esy_err_t vt_abort_dma(esy_uart_t *uart, bool is_tx) {
  return esy_hb_uart_abort_dma(uart, is_tx);
}

static esy_err_t vt_rx_stream_start(esy_uart_t *uart, uint8_t *storage,
                                    size_t storage_size,
                                    esy_uart_rx_stream_callback_t callback,
                                    void *user_data) {
  return esy_hb_uart_rx_stream_start(uart, storage, storage_size, callback,
                                         user_data);
}

static esy_err_t vt_rx_stream_peek(esy_uart_t *uart, const uint8_t **data,
                                   size_t *length) {
  return esy_hb_uart_rx_stream_peek(uart, data, length);
}

static esy_err_t vt_rx_stream_consume(esy_uart_t *uart, size_t length) {
  return esy_hb_uart_rx_stream_consume(uart, length);
}

static esy_err_t vt_rx_stream_stop(esy_uart_t *uart) {
  return esy_hb_uart_rx_stream_stop(uart);
}

static esy_err_t vt_suspend(esy_uart_t *uart) {
  return esy_hb_uart_suspend(uart);
}

static esy_err_t vt_resume(esy_uart_t *uart) {
  return esy_hb_uart_resume(uart);
}

static bool vt_is_readable(const esy_uart_t *uart) {
  return esy_hb_uart_is_readable(uart);
}

static esy_err_t vt_set_baudrate(esy_uart_t *uart, uint32_t baudrate) {
  return esy_hb_uart_set_baudrate(uart, baudrate);
}

static esy_err_t vt_get_caps(const esy_uart_t *uart, uint32_t *out_caps) {
  return esy_hb_uart_get_caps(uart, out_caps);
}

static esy_err_t vt_get_phys_index(const esy_uart_t *uart, uint8_t *out_index) {
  return esy_hb_uart_get_phys_index(uart, out_index);
}

/* Backend queries include only this UART's DMA and physical TX drain. */
static bool vt_is_busy(esy_uart_t *uart, bool is_tx) {
  if (!uart) return false;
  return esy_hb_uart_is_dma_busy(uart, is_tx) ||
      (is_tx && esy_hb_uart_is_transmitting(uart));
}

static esy_err_t uart_device_pm_action(esy_device_t *device,
                                       esy_device_pm_action_t action,
                                       esy_power_state_t system_state) {
  esy_uart_device_binding_t *binding;
  esy_err_t ret;
  uint8_t index;
  esy_board_controller_pin_state_t target_pin_state;

  (void)system_state;
  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_uart_device_binding_t *)device->driver_ctx;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (binding->suspended) return ESY_ERR_STATE;
      if (binding->rx_stream_ref_held || esy_hal_uart_is_busy(binding->uart, true) ||
          esy_hal_uart_is_busy(binding->uart, false)) {
        return ESY_ERR_BUSY;
      }
      ret = vt_get_phys_index(binding->uart, &index);
      if (ret != ESY_OK) return ret;
      target_pin_state =
          action == ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND
              ? ESY_BOARD_CONTROLLER_PIN_STATE_IDLE
              : ESY_BOARD_CONTROLLER_PIN_STATE_SLEEP;
      ret = vt_suspend(binding->uart);
      if (ret != ESY_OK) return ret;
      ret = esy_board_uart_select_pin_state(index, target_pin_state);
      if (ret != ESY_OK) {
        if (vt_resume(binding->uart) != ESY_OK) return ESY_ERR_STATE;
        return ret;
      }
      binding->pin_state = target_pin_state;
      binding->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!binding->suspended) return ESY_ERR_STATE;
      ret = vt_get_phys_index(binding->uart, &index);
      if (ret != ESY_OK) return ret;
      ret = esy_board_uart_select_pin_state(
          index, ESY_BOARD_CONTROLLER_PIN_STATE_DEFAULT);
      if (ret != ESY_OK) return ret;
      ret = vt_resume(binding->uart);
      if (ret != ESY_OK) {
        (void)esy_board_uart_select_pin_state(
            index, (esy_board_controller_pin_state_t)binding->pin_state);
        return ret;
      }
      binding->pin_state = ESY_BOARD_CONTROLLER_PIN_STATE_DEFAULT;
      binding->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static const esy_device_ops_t k_uart_device_ops = {
    .pm_action = uart_device_pm_action,
};

static const esy_uart_vtable_t k_uart_ops = {
    .send_blocking = vt_send_blocking,
    .receive_blocking = vt_receive_blocking,
    .send_dma = vt_send_dma,
    .receive_dma = vt_receive_dma,
    .abort_dma = vt_abort_dma,
    .poll = esy_hb_uart_poll,
    .get_dma_trans_count_remaining = esy_hb_uart_get_dma_trans_count_remaining,
    .rx_stream_start = vt_rx_stream_start,
    .rx_stream_peek = vt_rx_stream_peek,
    .rx_stream_consume = vt_rx_stream_consume,
    .rx_stream_stop = vt_rx_stream_stop,
    .suspend = vt_suspend,
    .resume = vt_resume,
    .is_readable = vt_is_readable,
    .is_busy = vt_is_busy,
    .set_baudrate = vt_set_baudrate,
    .get_caps = vt_get_caps,
    .get_phys_index = vt_get_phys_index,
};

/* ================ defaults ================ */

extern void esy_hal_uart_default_cfg(esy_uart_config_t *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->phys_index = 0;
  cfg->baudrate = 115200u;
  cfg->data_bits = 8;
  cfg->stop_bits = 1;
  cfg->parity = ESY_UART_PARITY_NONE;
  cfg->inner.irq_priority = 0;
}

/* ================ lifecycle ================ */

extern esy_err_t esy_hal_uart_init(esy_uart_t *uart,
                                        const esy_uart_config_t *cfg) {
  if (!uart || !cfg) return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_runtime(uart)->device != NULL || uart->ctx[0] != NULL || uart->ctx[1] != NULL ||
      (uart->ops != NULL && uart->ops->send_blocking != NULL) || (uart->ops != NULL && uart->ops->receive_blocking != NULL))
    return ESY_ERR_STATE;

  esy_uart_config_t local = *cfg;
  cfg = &local;

  /* Reserve before touching pins, including before a competing init fails. */
  esy_err_t ret = esy_hb_uart_validate_config(cfg);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_uart_claim(uart, cfg->phys_index);
  if (ret != ESY_OK) return ret;

  /* Complete board pins and controller IRQ metadata before HAL backend init. */
  esy_board_uart_configure_pins(cfg->phys_index,
                                &((esy_uart_config_t *)cfg)->inner);

  uart->ops = &k_uart_ops;

  atomic_init(&esy_hal_uart_runtime(uart)->tx_dma.state, 0u);
  atomic_init(&esy_hal_uart_runtime(uart)->rx_dma.state, 0u);
  esy_hal_uart_runtime(uart)->rx_stream_owned = false;
  /* Call HAL backend init directly without routing through ops */
  ret = esy_hb_uart_init(uart, cfg);
  if (ret == ESY_OK) return ESY_OK;
  /* Partial native state stays owned until cleanup succeeds. Failed init never
   * exposes I/O; ctx remains available for deinit retry after cleanup failure. */
  uart->ops = NULL;
  esy_err_t cleanup = esy_hb_uart_deinit(uart);
  if (cleanup != ESY_OK) return cleanup;
  esy_board_uart_reset_pins(cfg->phys_index);
  cleanup = esy_hb_uart_release(uart);
  if (cleanup != ESY_OK) return cleanup;
  memset(uart, 0, sizeof(*uart));
  return ret;
}

extern esy_err_t esy_hal_uart_deinit(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_runtime(uart)->device != NULL) return ESY_ERR_STATE;
  if (esy_hal_uart_runtime(uart)->rx_stream_owned) return ESY_ERR_BUSY;
  if (uart->ctx[0] == NULL && uart->ctx[1] == NULL &&
      (uart->ops == NULL || uart->ops->send_blocking == NULL) && (uart->ops == NULL || uart->ops->receive_blocking == NULL))
    return ESY_OK;
  uint8_t index;
  /* This also rejects XIO/foreign context pointers before native interpretation. */
  esy_err_t ret = esy_hb_uart_get_phys_index(uart, &index);
  if (ret != ESY_OK) return ESY_ERR_STATE;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->tx_dma.state, memory_order_acquire) != 0u ||
      atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) != 0u ||
      esy_hb_uart_is_dma_busy(uart, true) || esy_hb_uart_is_dma_busy(uart, false))
    return ESY_ERR_BUSY;

  ret = esy_hb_uart_deinit(uart);
  if (ret != ESY_OK) return ret;
  /* Keep the reservation until board cleanup completes; another owner must not
   * configure this controller between native teardown and pin reset. */
  esy_board_uart_reset_pins(index);
  ret = esy_hb_uart_release(uart);
  if (ret != ESY_OK) return ret;
  memset(uart, 0, sizeof(*uart));
  return ESY_OK;
}

esy_err_t esy_hal_uart_register_device(
    esy_uart_device_binding_t *binding, esy_uart_t *uart,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config) {
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (uart == NULL) || (registry == NULL) ||
      (device_config == NULL) || (esy_hal_uart_runtime(uart)->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->uart != NULL) return ESY_ERR_STATE;
  if (esy_hal_uart_runtime(uart)->rx_stream_owned) return ESY_ERR_BUSY;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->tx_dma.state, memory_order_acquire) != 0u ||
      atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) != 0u ||
      esy_hb_uart_is_dma_busy(uart, true) || esy_hb_uart_is_dma_busy(uart, false))
    return ESY_ERR_BUSY;
  memset(binding, 0, sizeof(*binding));
  binding->uart = uart;
  binding->pin_state = ESY_BOARD_CONTROLLER_PIN_STATE_DEFAULT;
  config.name = device_config->name;
  config.ops = &k_uart_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->dependencies;
  config.dependency_count = device_config->dependency_count;
  config.supported_state_mask = device_config->supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    esy_hal_uart_runtime(uart)->device = &binding->device;
  } else {
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

esy_err_t esy_hal_uart_unregister_device(
    esy_uart_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->uart == NULL) ||
      (esy_hal_uart_runtime(binding->uart)->device != &binding->device)) return ESY_ERR_INVALID_ARG;
  esy_uart_t *uart = binding->uart;
  /* Acquire callback retirement before observing its non-atomic lease fields. */
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->tx_dma.state, memory_order_acquire) != 0u ||
      atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) != 0u ||
      esy_hb_uart_is_dma_busy(uart, true) || esy_hb_uart_is_dma_busy(uart, false))
    return ESY_ERR_BUSY;
  if (binding->tx_dma_ref_held || binding->rx_dma_ref_held ||
      binding->rx_stream_ref_held) return ESY_ERR_BUSY;
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    esy_hal_uart_runtime(binding->uart)->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

extern void esy_hal_uart_handle_irq(uint8_t phys_index) {
  esy_hb_uart_handle_irq(phys_index);
}
