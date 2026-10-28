/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* HAL assembly for UART over XIO: 8N1, no dynamic memory. */
#include "esy_hal/xio/esy_hal_xio_uart.h"
#include "../esy_hal_uart_internal.h"
#include "esy_hal/xio/esy_hal_xio_uart_device.h"

#include <string.h>

#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_uart_defs.h"
#include "esy_hal_xio_uart_internal.h"
#include "esy_hal/esy_hal_uart.h"

#include "esy_board/esy_board.h"

void esy_hal_xio_uart_default_cfg(esy_xio_uart_config_t *cfg) {
  if (!cfg) return;

  memset(cfg, 0, sizeof(*cfg));
  cfg->pin = ESY_PIN_UNUSED; /* Not configured. */
  cfg->baudrate = 115200u;
  cfg->instance = 0xFFu;
  cfg->sm_index = 0xFFu;
}

bool esy_hal_xio_uart_is_busy(esy_uart_t *uart, bool is_tx) {
  if (!uart) return false;
  const esy_uart_dma_runtime_t *runtime = is_tx ? &esy_hal_uart_runtime(uart)->tx_dma : &esy_hal_uart_runtime(uart)->rx_dma;
  if (atomic_load_explicit(&runtime->state, memory_order_acquire) != 0u) return true;
  if (is_tx && esy_hal_xio_uart_tx_is_busy(uart)) return true;

  return false;
}

static void xio_uart_hardware_deinit(esy_uart_t *uart) {
  /* Both TX and RX deinit paths are no-op safe in the HAL backend. */
  esy_hal_xio_uart_tx_deinit(uart);
  esy_hal_xio_uart_rx_deinit(uart);
}

static esy_err_t xio_uart_abort_dma(esy_uart_t *uart, bool is_tx) {
  return is_tx ? esy_hal_xio_uart_tx_abort_dma(uart)
               : esy_hal_xio_uart_rx_abort_dma(uart);
}

static esy_err_t xio_uart_poll(esy_uart_t *uart) {
  esy_err_t tx = esy_hal_xio_uart_tx_poll(uart);
  esy_err_t rx = esy_hal_xio_uart_rx_poll(uart);
  return tx != ESY_OK ? tx : rx;
}

static const esy_uart_vtable_t k_xio_uart_tx_ops = {
    .send_blocking = esy_hal_xio_uart_tx_send_blocking,
    .send_dma = esy_hal_xio_uart_tx_send_dma,
    .is_busy = esy_hal_xio_uart_is_busy,
    .abort_dma = xio_uart_abort_dma,
    .poll = xio_uart_poll,
};

static const esy_uart_vtable_t k_xio_uart_rx_ops = {
    .receive_blocking = esy_hal_xio_uart_rx_receive_blocking,
    .receive_dma = esy_hal_xio_uart_rx_receive_dma,
    .is_readable = esy_hal_xio_uart_rx_is_readable,
    .is_busy = esy_hal_xio_uart_is_busy,
    .abort_dma = xio_uart_abort_dma,
    .poll = xio_uart_poll,
};

static const esy_uart_vtable_t k_xio_uart_duplex_ops = {
    .send_blocking = esy_hal_xio_uart_tx_send_blocking,
    .send_dma = esy_hal_xio_uart_tx_send_dma,
    .receive_blocking = esy_hal_xio_uart_rx_receive_blocking,
    .receive_dma = esy_hal_xio_uart_rx_receive_dma,
    .is_readable = esy_hal_xio_uart_rx_is_readable,
    .is_busy = esy_hal_xio_uart_is_busy,
    .abort_dma = xio_uart_abort_dma,
    .poll = xio_uart_poll,
};

static esy_err_t xio_uart_hardware_init(esy_uart_t *uart,
    const esy_xio_uart_config_t *tx, const esy_xio_uart_config_t *rx) {
  esy_err_t ret = ESY_OK;
  uart->ops = NULL;
  memset(uart->ctx, 0, sizeof(uart->ctx));
  if (tx != NULL) ret = esy_hal_xio_uart_tx_init(uart, tx);
  if (ret == ESY_OK && rx != NULL)
    ret = esy_hal_xio_uart_rx_init(uart, rx);
  if (ret != ESY_OK) {
    xio_uart_hardware_deinit(uart);
    uart->ops = NULL;
    return ret;
  }
  uart->ops = tx != NULL ? (rx != NULL ? &k_xio_uart_duplex_ops : &k_xio_uart_tx_ops)
                         : &k_xio_uart_rx_ops;
  return ESY_OK;
}

esy_err_t esy_hal_xio_uart_init(esy_uart_t *uart,
    esy_xio_uart_config_t *tx_cfg, esy_xio_uart_config_t *rx_cfg) {
  if (uart == NULL || (tx_cfg == NULL && rx_cfg == NULL))
    return ESY_ERR_INVALID_ARG;
  if (esy_hal_uart_runtime(uart)->device != NULL || (uart->ops != NULL && uart->ops->send_blocking != NULL) ||
      (uart->ops != NULL && uart->ops->receive_blocking != NULL)) return ESY_ERR_STATE;
  return xio_uart_hardware_init(uart, tx_cfg, rx_cfg);
}

void esy_hal_xio_uart_deinit(esy_uart_t *uart) {
  if (!uart || (esy_hal_uart_runtime(uart)->device != NULL)) return;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->tx_dma.state, memory_order_acquire) != 0u ||
      atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) != 0u) return;
  if (esy_hal_xio_uart_tx_is_busy(uart)) return;
  xio_uart_hardware_deinit(uart);
  memset(uart, 0, sizeof(*uart));
}

static esy_err_t xio_uart_device_pm_action(esy_device_t *device,
                                           esy_device_pm_action_t action,
                                           esy_power_state_t system_state) {
  esy_xio_uart_device_binding_t *binding;
  esy_err_t ret;

  (void)system_state;
  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_xio_uart_device_binding_t *)device->driver_ctx;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (binding->base.suspended) return ESY_ERR_STATE;
      if (esy_hal_uart_is_busy(binding->base.uart, true) ||
          esy_hal_uart_is_busy(binding->base.uart, false)) {
        return ESY_ERR_BUSY;
      }
      xio_uart_hardware_deinit(binding->base.uart);
      binding->base.suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!binding->base.suspended) return ESY_ERR_STATE;
      ret = xio_uart_hardware_init(
          binding->base.uart, binding->has_tx ? &binding->tx_resume_config : NULL,
          binding->has_rx ? &binding->rx_resume_config : NULL);
      if (ret != ESY_OK) return ret;
      binding->base.suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static bool xio_uart_device_is_runtime_idle(const esy_device_t *device) {
  esy_xio_uart_device_binding_t *binding = device->driver_ctx;
  return binding->base.suspended ||
      (!esy_hal_uart_is_busy(binding->base.uart, true) &&
       !esy_hal_uart_is_busy(binding->base.uart, false));
}

static const esy_device_ops_t k_xio_uart_device_ops = {
    .pm_action = xio_uart_device_pm_action,
    .is_runtime_idle = xio_uart_device_is_runtime_idle,
};

esy_err_t esy_hal_xio_uart_register_device(
    esy_xio_uart_device_binding_t *binding, esy_uart_t *uart,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_xio_uart_config_t *tx_resume_config,
    const esy_xio_uart_config_t *rx_resume_config) {
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (uart == NULL) || (registry == NULL) ||
      (device_config == NULL) ||
      ((tx_resume_config == NULL) && (rx_resume_config == NULL)) ||
      (esy_hal_uart_runtime(uart)->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->base.uart != NULL) return ESY_ERR_STATE;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->tx_dma.state, memory_order_acquire) != 0u ||
      atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) != 0u)
    return ESY_ERR_BUSY;
  memset(binding, 0, sizeof(*binding));
  binding->base.uart = uart;
  binding->has_tx = tx_resume_config != NULL;
  binding->has_rx = rx_resume_config != NULL;
  if (tx_resume_config != NULL) binding->tx_resume_config = *tx_resume_config;
  if (rx_resume_config != NULL) binding->rx_resume_config = *rx_resume_config;
  config.name = device_config->name;
  config.ops = &k_xio_uart_device_ops;
  config.driver_ctx = &binding->base;
  config.dependencies = device_config->dependencies;
  config.dependency_count = device_config->dependency_count;
  config.supported_state_mask = device_config->supported_state_mask;
  ret = esy_device_register(registry, &binding->base.device, &config);
  if (ret == ESY_OK) {
    esy_hal_uart_runtime(uart)->device = &binding->base.device;
  } else {
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

esy_err_t esy_hal_xio_uart_unregister_device(
    esy_xio_uart_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->base.uart == NULL) ||
      (esy_hal_uart_runtime(binding->base.uart)->device != &binding->base.device))
    return ESY_ERR_INVALID_ARG;
  esy_uart_t *uart = binding->base.uart;
  if (atomic_load_explicit(&esy_hal_uart_runtime(uart)->tx_dma.state, memory_order_acquire) != 0u ||
      atomic_load_explicit(&esy_hal_uart_runtime(uart)->rx_dma.state, memory_order_acquire) != 0u)
    return ESY_ERR_BUSY;
  if (binding->base.tx_dma_ref_held || binding->base.rx_dma_ref_held)
    return ESY_ERR_BUSY;
  ret = binding->base.device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->base.device);
  if (ret == ESY_OK) {
    esy_hal_uart_runtime(binding->base.uart)->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}
