/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_spi.h"
#include "esy_hal_spi_internal.h"
#include "esy_hal/esy_hal_spi_device.h"

#include <string.h>

#include "esy_board/esy_board.h"
#include "esy_hal_backend/esy_hb_spi.h" /* backend: esy_hb_spi_xxx */

typedef struct {
  esy_spi_config_t config;
  bool suspended;
} esy_hal_spi_device_ctx_t;

_Static_assert(sizeof(esy_hal_spi_device_ctx_t) <=
                   ESY_SPI_DEVICE_BACKEND_BYTES,
               "ESY_SPI_DEVICE_BACKEND_BYTES is too small");

/* ================= vt_*: sole ops implementation =================
 * Notes:
 * - HAL public APIs call spi->ops->* and do not perform board or index
 * completion.
 * - vt_* functions perform required board work and HAL backend forwarding.
 */

/* -------- blocking I/O -------- */

static int32_t vt_write_blocking(esy_spi_t *spi, const uint8_t *tx_buf,
                                 size_t len) {
  return esy_hb_spi_write_blocking(spi, tx_buf, len);
}

static int32_t vt_read_blocking(esy_spi_t *spi, uint8_t *rx_buf, size_t len) {
  return esy_hb_spi_read_blocking(spi, rx_buf, len);
}

static int32_t vt_transfer_blocking(esy_spi_t *spi, const uint8_t *tx_buf,
                                    uint8_t *rx_buf, size_t len) {
  return esy_hb_spi_transfer_blocking(spi, tx_buf, rx_buf, len);
}

static int32_t vt_write16_blocking(esy_spi_t *spi, const uint16_t *tx_buf,
                                   size_t len) {
  return esy_hb_spi_write16_blocking(spi, tx_buf, len);
}

static int32_t vt_read16_blocking(esy_spi_t *spi, uint16_t *rx_buf,
                                  size_t len) {
  return esy_hb_spi_read16_blocking(spi, rx_buf, len);
}

static int32_t vt_transfer16_blocking(esy_spi_t *spi, const uint16_t *tx_buf,
                                      uint16_t *rx_buf, size_t len) {
  return esy_hb_spi_transfer16_blocking(spi, tx_buf, rx_buf, len);
}

/* -------- DMA I/O -------- */

static esy_err_t vt_transfer_dma(esy_spi_t *spi, esy_dma_config_t *tx_dma_cfg,
                                 esy_dma_config_t *rx_dma_cfg,
                                 esy_spi_dma_complete_cb_t complete_cb,
                                 void *user_data) {
  if (!spi || (!tx_dma_cfg && !rx_dma_cfg)) return ESY_ERR_INVALID_ARG;

  /* Resolve the physical index and complete board DMA metadata in the vt
   * function */
  uint8_t idx;
  esy_err_t r = esy_hb_spi_get_phys_index(spi, &idx);
  if (r != ESY_OK) return r;

  /* Allow half-duplex transfers by mirroring length and width into a
   * placeholder config */
  esy_dma_config_t dummy = {0};
  if (!tx_dma_cfg) {
    dummy.src = NULL; /* placeholder only */
    if (rx_dma_cfg) {
      dummy.transfer_size = rx_dma_cfg->transfer_size;
      dummy.length = rx_dma_cfg->length;
    }
    tx_dma_cfg = &dummy;
  }
  if (!rx_dma_cfg) {
    dummy.dst = NULL; /* placeholder only */
    if (tx_dma_cfg) {
      dummy.transfer_size = tx_dma_cfg->transfer_size;
      dummy.length = tx_dma_cfg->length;
    }
    rx_dma_cfg = &dummy;
  }

  /* Complete board DREQ and channel metadata */
  if (!esy_board_spi_get_dma_info(idx, &tx_dma_cfg->inner, &rx_dma_cfg->inner))
    return ESY_ERR_NOT_SUPPORTED;

  return esy_hb_spi_transfer_dma(spi, tx_dma_cfg, rx_dma_cfg, complete_cb,
                                     user_data);
}

/* -------- queries/config -------- */

static esy_err_t vt_get_caps(const esy_spi_t *spi, uint32_t *out_caps) {
  return esy_hb_spi_get_caps(spi, out_caps);
}

static esy_err_t vt_get_phys_index(const esy_spi_t *spi, uint8_t *out_index) {
  return esy_hb_spi_get_phys_index(spi, out_index);
}

static bool vt_is_busy(esy_spi_t *spi) {
  return esy_hb_spi_is_busy(spi);
}
static esy_err_t vt_poll(esy_spi_t *spi) {
  return esy_hb_spi_poll(spi);
}

/* -------- speed and abort -------- */

static esy_err_t vt_abort(esy_spi_t *spi) { return esy_hb_spi_abort(spi); }

static esy_hal_spi_device_ctx_t *spi_device_ctx(
    esy_spi_device_binding_t *binding) {
  return (esy_hal_spi_device_ctx_t *)(void *)binding->backend.bytes;
}

static esy_err_t vt_set_speed(esy_spi_t *spi, uint32_t speed_hz) {
  esy_err_t ret = esy_hb_spi_set_speed(spi, speed_hz);
  if (ret == ESY_OK && esy_hal_spi_runtime(spi)->device != NULL) {
    esy_spi_device_binding_t *binding = esy_hal_spi_runtime(spi)->device->driver_ctx;
    spi_device_ctx(binding)->config.speed_hz = speed_hz;
  }
  return ret;
}

static const esy_spi_vtable_t k_spi_ops = {
    .write_blocking = vt_write_blocking,
    .read_blocking = vt_read_blocking,
    .transfer_blocking = vt_transfer_blocking,
    .write16_blocking = vt_write16_blocking,
    .read16_blocking = vt_read16_blocking,
    .transfer16_blocking = vt_transfer16_blocking,
    .transfer_dma = vt_transfer_dma,
    .get_caps = vt_get_caps,
    .get_phys_index = vt_get_phys_index,
    .is_busy = vt_is_busy,
    .poll = vt_poll,
    .set_speed = vt_set_speed,
    .abort = vt_abort,
    .get_dma_trans_count_remaining = esy_hb_spi_get_dma_trans_count_remaining,
};

static void bind_hardware_ops(esy_spi_t *spi) {
  spi->ops = &k_spi_ops;
}

static esy_err_t hardware_cleanup(esy_spi_t *spi, bool retain_claim) {
  uint8_t index;
  esy_err_t ret = esy_hb_spi_get_phys_index(spi, &index);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_spi_deinit(spi);
  if (ret != ESY_OK) return ret;
  /* Keep the claim until pin teardown finishes. A competing initializer must
   * not acquire the controller between native retirement and board reset. */
  if (esy_hal_spi_runtime(spi)->pins_configured) {
    esy_board_spi_reset_pins(index);
    esy_hal_spi_runtime(spi)->pins_configured = false;
  }
  return retain_claim ? ESY_OK : esy_hb_spi_release(spi);
}

static esy_err_t hardware_init(esy_spi_t *spi,
                               const esy_spi_config_t *config,
                               bool resuming) {
  esy_spi_config_t local = *config;
  esy_err_t ret = esy_hb_spi_claim(spi, &local);
  if (ret != ESY_OK) return ret;
  bind_hardware_ops(spi);
  esy_board_spi_configure_pins(local.phys_index, local.role, &local.inner);
  esy_hal_spi_runtime(spi)->pins_configured = true;
  ret = esy_hb_spi_init(spi, &local);
  if (ret != ESY_OK) {
    esy_err_t cleanup = hardware_cleanup(spi, resuming);
    if (cleanup != ESY_OK) return cleanup;
    if (!resuming) memset(spi, 0, sizeof(*spi));
  }
  return ret;
}

static esy_err_t hardware_suspend(
    esy_spi_t *spi, const esy_spi_config_t *config,
    esy_board_controller_pin_state_t pin_state) {
  uint8_t index;
  esy_err_t ret = esy_hb_spi_get_phys_index(spi, &index);
  if (ret != ESY_OK) return ret;
  ret = esy_hb_spi_deinit(spi);
  if (ret != ESY_OK) {
    /* A cleanup error may leave a partially retired SDK controller. Force the
     * registry fault path rather than claiming an intact active graph. */
    return ret == ESY_ERR_BUSY ? ret : ESY_ERR_STATE;
  }
  ret = esy_board_spi_select_pin_state(index, pin_state);
  if (ret == ESY_OK) return ESY_OK;
  /* A failed board transition must leave a usable active graph or fault it. */
  return hardware_init(spi, config, true) == ESY_OK ? ret : ESY_ERR_STATE;
}

static esy_err_t hardware_deinit(esy_spi_t *spi) {
  if (spi->ctx == NULL) {
    return (spi->ops == NULL || spi->ops->write_blocking == NULL) ? ESY_OK : ESY_ERR_STATE;
  }
  uint8_t index;
  if (esy_hb_spi_get_phys_index(spi, &index) != ESY_OK) return ESY_ERR_STATE;
  return hardware_cleanup(spi, false);
}

static esy_err_t standard_spi_transition(esy_spi_device_binding_t *binding,
                                         esy_device_pm_action_t action,
                                         esy_power_state_t system_state) {
  esy_hal_spi_device_ctx_t *ctx = spi_device_ctx(binding);
  esy_err_t ret;

  (void)system_state;
  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      if (ctx->suspended) {
        return ESY_ERR_STATE;
      }
      if (vt_is_busy(binding->spi)) {
        return ESY_ERR_BUSY;
      }
      ret = hardware_suspend(
          binding->spi, &ctx->config,
          action == ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND
              ? ESY_BOARD_CONTROLLER_PIN_STATE_IDLE
              : ESY_BOARD_CONTROLLER_PIN_STATE_SLEEP);
      if (ret != ESY_OK) return ret;
      ctx->suspended = true;
      return ESY_OK;

    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      if (!ctx->suspended) {
        return ESY_ERR_STATE;
      }
      if (hardware_init(binding->spi, &ctx->config, true) != ESY_OK) {
        return ESY_ERR_STATE;
      }
      ctx->suspended = false;
      return ESY_OK;

    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static esy_err_t spi_device_pm_action(esy_device_t *device,
                                      esy_device_pm_action_t action,
                                      esy_power_state_t system_state) {
  esy_spi_device_binding_t *binding;

  if ((device == NULL) || (device->driver_ctx == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  binding = (esy_spi_device_binding_t *)device->driver_ctx;
  return binding->transition(binding, action, system_state);
}

static bool spi_device_is_runtime_idle(const esy_device_t *device) {
  const esy_spi_device_binding_t *binding;
  const esy_hal_spi_device_ctx_t *ctx;

  if ((device == NULL) || (device->driver_ctx == NULL)) return false;
  binding = device->driver_ctx;
  ctx = (const esy_hal_spi_device_ctx_t *)(const void *)
            binding->backend.bytes;
  return (ctx->config.role != ESY_SPI_ROLE_SLAVE) &&
         !vt_is_busy(binding->spi);
}

static const esy_device_ops_t k_spi_device_ops = {
    .pm_action = spi_device_pm_action,
    .is_runtime_idle = spi_device_is_runtime_idle,
};

/* ================ defaults ================ */

extern void esy_hal_spi_default_cfg(esy_spi_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->speed_hz = 1000000u;
  config->data_bits = 8;
  config->is_msb_first = true;
  config->inner.cs_pin = ESY_PIN_UNUSED;
}

/* ================ lifecycle ================ */

extern esy_err_t esy_hal_spi_init(esy_spi_t *spi,
                                       const esy_spi_config_t *cfg) {
  if (!spi || !cfg) return ESY_ERR_INVALID_ARG;
  if (esy_hal_spi_runtime(spi)->device != NULL || spi->ctx != NULL ||
      (spi->ops != NULL && spi->ops->write_blocking != NULL)) return ESY_ERR_STATE;
  return hardware_init(spi, cfg, false);
}

esy_err_t esy_hal_spi_deinit(esy_spi_t *spi) {
  if (spi == NULL) return ESY_ERR_INVALID_ARG;
  if (esy_hal_spi_runtime(spi)->device != NULL) return ESY_ERR_STATE;
  esy_err_t ret = hardware_deinit(spi);
  if (ret == ESY_OK) memset(spi, 0, sizeof(*spi));
  return ret;
}

esy_err_t esy_hal_spi_register_device(
    esy_spi_device_binding_t *binding, esy_spi_t *spi,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config,
    const esy_spi_config_t *resume_config) {
  esy_device_config_t config;
  esy_hal_spi_device_ctx_t *ctx;
  esy_err_t ret;

  if ((binding == NULL) || (spi == NULL) || (registry == NULL) ||
      (device_config == NULL) || (resume_config == NULL) ||
      (esy_hal_spi_runtime(spi)->device != NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->spi != NULL) return ESY_ERR_STATE;
  memset(binding, 0, sizeof(*binding));
  binding->spi = spi;
  binding->transition = standard_spi_transition;
  ctx = spi_device_ctx(binding);
  ctx->config = *resume_config;

  config.name = device_config->name;
  config.ops = &k_spi_device_ops;
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
