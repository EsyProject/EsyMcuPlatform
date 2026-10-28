/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* HAL assembly for SPI over XIO/PIO: no dynamic memory, vtable-compatible.
 */

#include "esy_hal/xio/esy_hal_xio_spi.h"
#include "../esy_hal_spi_internal.h"
#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/xio/esy_hal_xio_spi_device.h"

#include <string.h>

#include "esy_common/defs/peripheral/esy_spi_defs.h" /* esy_spi_t / esy_spi_config_t */
#include "esy_common/defs/peripheral/xio/esy_xio_spi_defs.h" /* esy_xio_spi_config_t */
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_board/esy_board.h"

/* XIO HAL backend implemented with PIO. */
#include "esy_hal_backend/xio/spi/esy_hb_xio_spi.h"

#define XIO_SPI_SM_COUNT 4u

typedef struct {
  esy_xio_spi_config_t config;
  bool suspended;
} esy_hal_xio_spi_device_ctx_t;

_Static_assert(sizeof(esy_hal_xio_spi_device_ctx_t) <=
                   ESY_SPI_DEVICE_BACKEND_BYTES,
               "ESY_SPI_DEVICE_BACKEND_BYTES is too small for XIO SPI");

/* ============== default configuration ============== */

void esy_hal_xio_spi_default_cfg(esy_xio_spi_config_t *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->pin_sck = ESY_PIN_UNUSED;
  cfg->pin_mosi = ESY_PIN_UNUSED;
  cfg->pin_miso = ESY_PIN_UNUSED;
  cfg->pin_cs = ESY_PIN_UNUSED;
  cfg->pin_trigger = ESY_PIN_UNUSED;

  cfg->role = ESY_SPI_ROLE_MASTER;
  cfg->mode = ESY_SPI_MODE_0;
  cfg->is_msb_first = true;
  cfg->data_bits = 8;
  cfg->speed_hz = 1u * 1000u * 1000u;

  cfg->cs_active_low = true;
  cfg->cs_setup_ns = 0;
  cfg->cs_hold_ns = 0;
  cfg->cs_inactive_ns = 0;

  /* instance/sm are explicitly selected by the caller. */
  cfg->instance = 0xFFu;
  cfg->sm_index = 0xFFu;
  cfg->slave_cs_sm_index = 0xFFu;
}

/* ============== DMA wrapper: complete DREQ/channel metadata ============== */
/* Notes:
 * - PIO DREQ is usually tied to the SM TX/RX direction.
 * - The HAL uses the board pin-to-DREQ mapping.
 * - TX uses the MOSI pin and RX uses the MISO pin for DMA metadata lookup.
 */

/* HAL transfer_dma implementation bound into ops. */
static esy_err_t vt_transfer_dma(esy_spi_t *spi, esy_dma_config_t *tx_dma_cfg,
                                 esy_dma_config_t *rx_dma_cfg,
                                 esy_spi_dma_complete_cb_t complete_cb,
                                 void *user_data) {
  if (!spi || (!tx_dma_cfg && !rx_dma_cfg)) return ESY_ERR_INVALID_ARG;

  esy_dma_config_t tx, rx;
  if (tx_dma_cfg) { tx = *tx_dma_cfg; tx_dma_cfg = &tx; }
  if (rx_dma_cfg) { rx = *rx_dma_cfg; rx_dma_cfg = &rx; }

  /* Allow half-duplex transfers by mirroring length and width into dummy cfg.
   */
  esy_dma_config_t dummy = {0};
  if (!tx_dma_cfg && rx_dma_cfg) {
    dummy.src = NULL;
    dummy.transfer_size = rx_dma_cfg->transfer_size;
    dummy.length = rx_dma_cfg->length;
    tx_dma_cfg = &dummy;
  }
  if (!rx_dma_cfg && tx_dma_cfg) {
    dummy.dst = NULL;
    dummy.transfer_size = tx_dma_cfg->transfer_size;
    dummy.length = tx_dma_cfg->length;
    rx_dma_cfg = &dummy;
  }

  /* Complete board DREQ/channel metadata. */
  if (tx_dma_cfg) {
    uint8_t mosi = esy_hb_xio_spi_get_tx_pin(spi);
    if (!esy_board_xio_get_dma_info(mosi, &tx_dma_cfg->inner)) {
      return ESY_ERR_INVALID_ARG;
    }
  }
  if (rx_dma_cfg) {
    uint8_t miso = esy_hb_xio_spi_get_rx_pin(spi);
    if (!esy_board_xio_get_dma_info(miso, &rx_dma_cfg->inner)) {
      return ESY_ERR_INVALID_ARG;
    }
  }

  return esy_hb_xio_spi_transfer_dma(spi, tx_dma_cfg, rx_dma_cfg,
                                         complete_cb, user_data);
}

/* ============== lifecycle: assemble ops and initialize HAL backend ==============
 */

static esy_hal_xio_spi_device_ctx_t *xio_spi_device_ctx(
    esy_spi_device_binding_t *binding) {
  return (esy_hal_xio_spi_device_ctx_t *)(void *)binding->backend.bytes;
}

static const esy_spi_vtable_t k_xio_spi_ops = {
    .write_blocking = esy_hb_xio_spi_write_blocking,
    .read_blocking = esy_hb_xio_spi_read_blocking,
    .transfer_blocking = esy_hb_xio_spi_transfer_blocking,
    .write_then_read_blocking = esy_hb_xio_spi_write_then_read_blocking,
    .write16_blocking = esy_hb_xio_spi_write16_blocking,
    .read16_blocking = esy_hb_xio_spi_read16_blocking,
    .transfer16_blocking = esy_hb_xio_spi_transfer16_blocking,
    .transfer_dma = vt_transfer_dma,
    .poll = esy_hb_xio_spi_poll,
    .abort = esy_hb_xio_spi_abort,
    .is_busy = esy_hb_xio_spi_is_busy,
};

static esy_err_t xio_spi_hardware_init(esy_spi_t *spi,
                                       esy_xio_spi_config_t *config) {
  /* Slave programs require a physical CS input to delimit frames. */
  if (config->role == ESY_SPI_ROLE_SLAVE && config->pin_cs == ESY_PIN_UNUSED)
    return ESY_ERR_NOT_SUPPORTED;
  if (config->role == ESY_SPI_ROLE_SLAVE &&
      config->pin_miso != ESY_PIN_UNUSED &&
      (config->slave_cs_sm_index >= XIO_SPI_SM_COUNT ||
       config->slave_cs_sm_index == config->sm_index ||
       config->pin_miso == config->pin_sck ||
       config->pin_miso == config->pin_cs ||
       config->pin_miso == config->pin_mosi))
    return ESY_ERR_INVALID_ARG;
  esy_xio_inner_config_t inner_config = {
      .instance = config->instance,
      .sm_index = config->sm_index,
  };

  spi->ops = &k_xio_spi_ops;

  if (config->pin_sck != ESY_PIN_UNUSED) {
    esy_board_xio_configure_pins(config->pin_sck, &inner_config);
  }
  esy_err_t ret = esy_hb_xio_spi_init(spi, config);
  if (ret != ESY_OK) spi->ops = NULL;
  return ret;
}

static esy_err_t xio_spi_transition(esy_spi_device_binding_t *binding,
                                    esy_device_pm_action_t action,
                                    esy_power_state_t system_state) {
  esy_hal_xio_spi_device_ctx_t *ctx = xio_spi_device_ctx(binding);

  (void)system_state;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (ctx->suspended || binding->async_ref_held ||
          esy_hb_xio_spi_is_busy(binding->spi)) {
        return ESY_ERR_BUSY;
      }
      esy_hb_xio_spi_deinit(binding->spi);
      ctx->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!ctx->suspended) {
        return ESY_ERR_STATE;
      }
      if (xio_spi_hardware_init(binding->spi, &ctx->config) != ESY_OK) {
        return ESY_ERR_FAIL;
      }
      ctx->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static esy_err_t xio_spi_device_pm_action(esy_device_t *device,
                                          esy_device_pm_action_t action,
                                          esy_power_state_t system_state) {
  esy_spi_device_binding_t *binding;

  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_spi_device_binding_t *)device->driver_ctx;
  return binding->transition(binding, action, system_state);
}

static const esy_device_ops_t k_xio_spi_device_ops = {
    .pm_action = xio_spi_device_pm_action,
};

esy_err_t esy_hal_xio_spi_init(esy_spi_t *spi, esy_xio_spi_config_t *cfg) {
  if (!spi || !cfg) return ESY_ERR_INVALID_ARG;
  if (esy_hal_spi_runtime(spi)->device != NULL || (spi->ops != NULL && spi->ops->write_blocking != NULL))
    return ESY_ERR_STATE;
  return xio_spi_hardware_init(spi, cfg);
}

esy_err_t esy_hal_xio_spi_register_device(
    esy_spi_device_binding_t *binding, esy_spi_t *spi,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_xio_spi_config_t *resume_config) {
  esy_device_config_t config;
  esy_hal_xio_spi_device_ctx_t *ctx;
  esy_err_t ret;

  if ((binding == NULL) || (spi == NULL) || (registry == NULL) ||
      (device_config == NULL) || (resume_config == NULL) ||
      (esy_hal_spi_runtime(spi)->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->spi != NULL) return ESY_ERR_STATE;
  memset(binding, 0, sizeof(*binding));
  binding->spi = spi;
  binding->transition = xio_spi_transition;
  ctx = xio_spi_device_ctx(binding);
  ctx->config = *resume_config;

  config.name = device_config->name;
  config.ops = &k_xio_spi_device_ops;
  config.driver_ctx = binding;
  config.dependencies = device_config->dependencies;
  config.dependency_count = device_config->dependency_count;
  config.supported_state_mask = device_config->supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    esy_hal_spi_runtime(spi)->device = &binding->device;
  } else {
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

void esy_hal_xio_spi_deinit(esy_spi_t *spi) {
  if (!spi || (esy_hal_spi_runtime(spi)->device != NULL)) return;

  if (esy_hb_xio_spi_is_busy(spi)) return;
  esy_hb_xio_spi_deinit(spi);
  memset(spi, 0, sizeof(*spi));
}

/* XIO and native handles share descriptor validation and async PM ownership. */
esy_err_t esy_hal_xio_spi_transfer_dma(
    esy_spi_t *spi, esy_dma_config_t *tx_dma_cfg, esy_dma_config_t *rx_dma_cfg,
    esy_spi_dma_complete_cb_t complete_cb, void *user_data) {
  return esy_hal_spi_transfer_dma(spi, tx_dma_cfg, rx_dma_cfg, complete_cb,
                                 user_data);
}
