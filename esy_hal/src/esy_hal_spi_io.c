/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_spi.h"
#include "esy_hal_spi_internal.h"
#include "esy_hal/esy_hal_spi_device.h"

#include <string.h>

static esy_err_t spi_validate_owner(const esy_spi_t *spi) {
  if (esy_hal_spi_runtime_const(spi)->device != NULL) {
    const esy_spi_device_binding_t *binding = esy_hal_spi_runtime_const(spi)->device->driver_ctx;
    if (binding == NULL || binding->spi != spi) return ESY_ERR_STATE;
  }
  return ESY_OK;
}

static esy_err_t spi_acquire(esy_spi_t *spi) {
  esy_err_t ret = spi_validate_owner(spi);
  if (ret != ESY_OK) return ret;
  if (esy_hal_spi_runtime(spi)->device == NULL) {
    return ESY_OK;
  }
  return esy_device_get(esy_hal_spi_runtime(spi)->device);
}

static void spi_release(esy_spi_t *spi) {
  if (esy_hal_spi_runtime(spi)->device != NULL) {
    (void)esy_device_put(esy_hal_spi_runtime(spi)->device);
  }
}

static esy_spi_device_binding_t *spi_binding(esy_spi_t *spi) {
  if ((spi == NULL) || (esy_hal_spi_runtime(spi)->device == NULL)) {
    return NULL;
  }
  return (esy_spi_device_binding_t *)esy_hal_spi_runtime(spi)->device->driver_ctx;
}

static void spi_dma_complete(esy_spi_t *spi,
                             const esy_spi_dma_transfer_result_t *result,
                             void *user_data) {
  esy_spi_device_binding_t *binding = (esy_spi_device_binding_t *)user_data;
  esy_spi_dma_complete_cb_t complete_cb = binding->async_complete_cb;
  void *complete_arg = binding->async_user_data;

  binding->async_complete_cb = NULL;
  binding->async_user_data = NULL;
  binding->async_ref_held = false;
  if (complete_cb != NULL) {
    complete_cb(spi, result, complete_arg);
  }
  (void)esy_device_put_from_isr(esy_hal_spi_runtime(spi)->device);
}

esy_err_t esy_hal_spi_unregister_device(
    esy_spi_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->spi == NULL) ||
      (esy_hal_spi_runtime(binding->spi)->device != &binding->device) || binding->async_ref_held) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    esy_hal_spi_runtime(binding->spi)->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

const esy_device_t *esy_hal_spi_get_device(const esy_spi_t *spi) {
  return spi == NULL ? NULL : esy_hal_spi_runtime_const(spi)->device;
}

int32_t esy_hal_spi_write_blocking_for_pm(esy_spi_t *spi,
                                         const esy_device_t *requester,
                                         const uint8_t *data, size_t len) {
  if (spi == NULL || requester == NULL || data == NULL || len == 0u)
    return ESY_ERR_INVALID_ARG;
  esy_device_t *device = esy_hal_spi_runtime(spi)->device;
  if (device == NULL || device->registry == NULL ||
      requester->registry != device->registry || spi_validate_owner(spi) != ESY_OK)
    return ESY_ERR_STATE;
  esy_device_state_t requester_state, provider_state;
  if (esy_device_get_state(requester, &requester_state) != ESY_OK ||
      requester_state != ESY_DEVICE_STATE_TRANSITIONING ||
      esy_device_get_state(device, &provider_state) != ESY_OK ||
      provider_state != ESY_DEVICE_STATE_ACTIVE)
    return ESY_ERR_STATE;
  bool dependent = false;
  if (requester->dependencies == NULL) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < requester->dependency_count; ++i) {
    if (requester->dependencies[i] == device) dependent = true;
  }
  if (!dependent) return ESY_ERR_STATE;
  const esy_spi_device_binding_t *binding = device->driver_ctx;
  if (binding->async_ref_held || esy_hal_spi_is_busy(spi)) return ESY_ERR_BUSY;
  if (spi->ops == NULL || spi->ops->write_blocking == NULL)
    return ESY_ERR_NOT_SUPPORTED;
  return spi->ops->write_blocking(spi, data, len);
}

/* ================ blocking I/O ================ */

extern int32_t esy_hal_spi_write_blocking(esy_spi_t *spi,
                                               const uint8_t *tx_buf,
                                               size_t len) {
  int32_t result;

  if (!spi || !tx_buf || !len) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->write_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result = spi->ops->write_blocking(spi, tx_buf, len);
  spi_release(spi);
  return result;
}

extern int32_t esy_hal_spi_read_blocking(esy_spi_t *spi, uint8_t *rx_buf,
                                              size_t len) {
  int32_t result;

  if (!spi || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->read_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result = spi->ops->read_blocking(spi, rx_buf, len);
  spi_release(spi);
  return result;
}

extern int32_t esy_hal_spi_transfer_blocking(esy_spi_t *spi,
                                                  const uint8_t *tx_buf,
                                                  uint8_t *rx_buf, size_t len) {
  int32_t result;

  if (!spi || !tx_buf || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->transfer_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result = spi->ops->transfer_blocking(spi, tx_buf, rx_buf, len);
  spi_release(spi);
  return result;
}

extern esy_err_t esy_hal_spi_write_then_read_blocking(
    esy_spi_t *spi, const uint8_t *tx_buf, size_t tx_len, uint8_t *rx_buf,
    size_t rx_len) {
  esy_err_t result;

  if (!spi || !tx_buf || !tx_len || !rx_buf || !rx_len)
    return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->write_then_read_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result =
      spi->ops->write_then_read_blocking(spi, tx_buf, tx_len, rx_buf, rx_len);
  spi_release(spi);
  return result;
}

extern int32_t esy_hal_spi_write16_blocking(esy_spi_t *spi,
                                                 const uint16_t *tx_buf,
                                                 size_t len) {
  int32_t result;

  if (!spi || !tx_buf || !len) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->write16_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result = spi->ops->write16_blocking(spi, tx_buf, len);
  spi_release(spi);
  return result;
}

extern int32_t esy_hal_spi_read16_blocking(esy_spi_t *spi,
                                                uint16_t *rx_buf, size_t len) {
  int32_t result;

  if (!spi || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->read16_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result = spi->ops->read16_blocking(spi, rx_buf, len);
  spi_release(spi);
  return result;
}

extern int32_t esy_hal_spi_transfer16_blocking(esy_spi_t *spi,
                                                    const uint16_t *tx_buf,
                                                    uint16_t *rx_buf,
                                                    size_t len) {
  int32_t result;

  if (!spi || !tx_buf || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->transfer16_blocking == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result = spi->ops->transfer16_blocking(spi, tx_buf, rx_buf, len);
  spi_release(spi);
  return result;
}

/* ================ DMA I/O ================ */

extern esy_err_t esy_hal_spi_write_dma(esy_spi_t *spi,
                                            esy_dma_config_t *dma_cfg) {
  return esy_hal_spi_transfer_dma(spi, dma_cfg, NULL, NULL, NULL);
}

extern esy_err_t esy_hal_spi_read_dma(esy_spi_t *spi,
                                           esy_dma_config_t *dma_cfg) {
  return esy_hal_spi_transfer_dma(spi, NULL, dma_cfg, NULL, NULL);
}

extern esy_err_t esy_hal_spi_transfer_dma(
    esy_spi_t *spi, esy_dma_config_t *tx_dma_cfg, esy_dma_config_t *rx_dma_cfg,
    esy_spi_dma_complete_cb_t complete_cb, void *user_data) {
  esy_spi_device_binding_t *binding;
  esy_err_t ret;

  if (!spi || (!tx_dma_cfg && !rx_dma_cfg)) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->transfer_dma == NULL) return ESY_ERR_NOT_SUPPORTED;
  ret = spi_validate_owner(spi);
  if (ret != ESY_OK) return ret;
  const esy_dma_config_t *configs[2] = {tx_dma_cfg, rx_dma_cfg};
  for (size_t i = 0u; i < 2u; ++i) {
    const esy_dma_config_t *config = configs[i];
    if (config == NULL) continue;
    const void *memory = i == 0u ? config->src : config->dst;
    if (memory == NULL || config->length == 0u ||
        config->length > SIZE_MAX / 8u) return ESY_ERR_INVALID_ARG;
    if (config->circular || config->transfer_size == ESY_DMA_TRANSFER_SIZE_32)
      return ESY_ERR_NOT_SUPPORTED;
    if (config->transfer_size != ESY_DMA_TRANSFER_SIZE_8 &&
        config->transfer_size != ESY_DMA_TRANSFER_SIZE_16)
      return ESY_ERR_INVALID_ARG;
    const size_t unit_bytes = config->transfer_size == ESY_DMA_TRANSFER_SIZE_16
                                  ? sizeof(uint16_t) : sizeof(uint8_t);
    if (config->length % unit_bytes != 0u ||
        (uintptr_t)memory % unit_bytes != 0u) return ESY_ERR_INVALID_ARG;
  }
  if (tx_dma_cfg != NULL && rx_dma_cfg != NULL &&
      (tx_dma_cfg->length != rx_dma_cfg->length ||
       tx_dma_cfg->transfer_size != rx_dma_cfg->transfer_size))
    return ESY_ERR_INVALID_ARG;
  esy_dma_config_t tx, rx;
  if (tx_dma_cfg) { tx = *tx_dma_cfg; tx_dma_cfg = &tx; }
  if (rx_dma_cfg) { rx = *rx_dma_cfg; rx_dma_cfg = &rx; }
  binding = spi_binding(spi);
  if (binding == NULL) {
    return spi->ops->transfer_dma(spi, tx_dma_cfg, rx_dma_cfg, complete_cb,
                                 user_data);
  }
  if (binding->async_ref_held) {
    return ESY_ERR_BUSY;
  }
  ret = spi_acquire(spi);
  if (ret != ESY_OK) {
    return ret;
  }
  binding->async_complete_cb = complete_cb;
  binding->async_user_data = user_data;
  binding->async_ref_held = true;
  ret = spi->ops->transfer_dma(spi, tx_dma_cfg, rx_dma_cfg, spi_dma_complete,
                              binding);
  if ((ret != ESY_OK) && binding->async_ref_held &&
      esy_hal_spi_is_busy(spi) == false) {
    binding->async_complete_cb = NULL;
    binding->async_user_data = NULL;
    binding->async_ref_held = false;
    spi_release(spi);
  }
  return ret;
}

extern esy_err_t esy_hal_spi_get_dma_trans_count_remaining(
    const esy_spi_t *spi, size_t *out_tx_remaining, size_t *out_rx_remaining) {
  if (!spi) return ESY_ERR_INVALID_ARG;
  if ((out_tx_remaining == NULL) && (out_rx_remaining == NULL))
    return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->get_dma_trans_count_remaining == NULL) return ESY_ERR_NOT_SUPPORTED;
  return spi->ops->get_dma_trans_count_remaining(spi, out_tx_remaining, out_rx_remaining);
}

/* ================ config / control wrappers ================ */

extern esy_err_t esy_hal_spi_set_speed(esy_spi_t *spi, uint32_t speed_hz) {
  esy_err_t result;

  if (!spi || !speed_hz) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->set_speed == NULL) return ESY_ERR_NOT_SUPPORTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result = spi->ops->set_speed(spi, speed_hz);
  spi_release(spi);
  return result;
}

extern esy_err_t esy_hal_spi_abort(esy_spi_t *spi) {
  esy_err_t result;

  if (!spi) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->abort == NULL) return ESY_ERR_NOT_SUPPORTED;
  result = spi_acquire(spi);
  if (result != ESY_OK) return result;
  result = spi->ops->abort(spi);
  spi_release(spi);
  if (result == ESY_OK) {
    esy_spi_device_binding_t *binding = spi_binding(spi);
    if ((binding != NULL) && binding->async_ref_held) {
      binding->async_complete_cb = NULL;
      binding->async_user_data = NULL;
      binding->async_ref_held = false;
      spi_release(spi);
    }
  }
  return result;
}

/* ================ queries ================ */

extern bool esy_hal_spi_is_busy(esy_spi_t *spi) {
  if (!spi || (spi->ops == NULL || spi->ops->is_busy == NULL)) return false;
  return spi->ops->is_busy(spi);
}

extern esy_err_t esy_hal_spi_get_caps(const esy_spi_t *spi,
                                           uint32_t *out_caps) {
  if (!spi || !out_caps) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->get_caps == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return spi->ops->get_caps(spi, out_caps);
}

extern esy_err_t esy_hal_spi_get_phys_index(const esy_spi_t *spi,
                                                 uint8_t *out_index) {
  if (!spi || !out_index) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->get_phys_index == NULL) return ESY_ERR_NOT_IMPLEMENTED;
  return spi->ops->get_phys_index(spi, out_index);
}

esy_err_t esy_hal_spi_poll(esy_spi_t *spi) {
  if (!spi) return ESY_ERR_INVALID_ARG;
  if (spi->ops == NULL || spi->ops->poll == NULL) return ESY_ERR_STATE;
  return spi->ops->poll(spi);
}
