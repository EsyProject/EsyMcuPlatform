/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_usb_dev.c
 * @brief HAL USB Device implementation forwarding through handle operations.
 *
 * Design constraints:
 * - The caller provides esy_usb_dev_t storage; the HAL assembles ops
 *   and forwards through the HAL backend.
 * - HAL public APIs call dev->ops callbacks and avoid chip-specific clock, cache,
 *   and alignment policy here.
 * - Board pins, pull-ups, and equivalent resources are owned by esy_board_*.
 */

#include "esy_hal/esy_hal_usb_dev.h"
#include "esy_hal/esy_hal_usb_dev_device.h"
#include "esy_hal_usb_dev_internal.h"

#include <string.h>

#include "esy_hal_backend/esy_hb_usb_dev.h" /* backend: esy_hb_usb_dev_xxx */
#include "esy_board/esy_board.h"

static esy_err_t usb_acquire(esy_usb_dev_t *usb) {
  esy_device_t *device = esy_hal_usb_runtime(usb)->device;
  return device == NULL ? ESY_OK : esy_device_get(device);
}

static void usb_release(esy_usb_dev_t *usb) {
  if (esy_hal_usb_runtime(usb)->device != NULL) {
    (void)esy_device_put(esy_hal_usb_runtime(usb)->device);
  }
}

/* ================= vt_*: sole ops implementation =================
 * Notes:
 * - HAL public APIs call dev->ops callbacks and do not perform board or index
 *   completion.
 * - vt_* functions perform required board work and HAL backend forwarding when
 * needed.
 */

/* -------- control plane -------- */

/**
 * @brief Set the USB device address by forwarding to the HAL backend.
 */
static esy_err_t vt_set_address(esy_usb_dev_t *dev, uint8_t addr) {
  return esy_hb_usb_dev_set_address(dev, addr);
}

/* -------- endpoint lifecycle -------- */

static esy_err_t vt_ep_init(esy_usb_dev_t *dev, const esy_usb_ep_cfg_t *cfg) {
  return esy_hb_usb_dev_ep_init(dev, cfg);
}

static esy_err_t vt_ep_deinit(esy_usb_dev_t *dev, uint8_t ep_addr) {
  return esy_hb_usb_dev_ep_deinit(dev, ep_addr);
}

/* -------- endpoint control and transfer -------- */

static esy_err_t vt_ep_stall(esy_usb_dev_t *dev, uint8_t ep_addr, bool stall) {
  return esy_hb_usb_dev_ep_stall(dev, ep_addr, stall);
}

static esy_err_t vt_ep_xfer(esy_usb_dev_t *dev, uint8_t ep_addr,
                            esy_usb_xfer_t *xfer) {
  return esy_hb_usb_dev_ep_xfer(dev, ep_addr, xfer);
}

static esy_err_t vt_ep_cancel(esy_usb_dev_t *dev, uint8_t ep_addr) {
  return esy_hb_usb_dev_ep_cancel(dev, ep_addr);
}

/* -------- optional EP0 helpers -------- */

static esy_err_t vt_ep0_read(esy_usb_dev_t *dev, uint8_t *buf, uint16_t len) {
  return esy_hb_usb_dev_ep0_read(dev, buf, len);
}

static esy_err_t vt_ep0_write(esy_usb_dev_t *dev, const uint8_t *buf,
                              uint16_t len) {
  return esy_hb_usb_dev_ep0_write(dev, buf, len);
}

static esy_err_t vt_ep0_status(esy_usb_dev_t *dev) {
  return esy_hb_usb_dev_ep0_status(dev);
}

/* -------- queries -------- */

static esy_err_t vt_get_caps(const esy_usb_dev_t *dev, uint32_t *out_caps) {
  return esy_hb_usb_dev_get_caps(dev, out_caps);
}

static esy_err_t vt_get_phys_index(const esy_usb_dev_t *dev,
                                   uint8_t *out_index) {
  return esy_hb_usb_dev_get_phys_index(dev, out_index);
}

/* ================ helpers ================ */

static const esy_usb_vtable_dev_t k_usb_ops = {
    .set_address = vt_set_address,
    .ep_init = vt_ep_init,
    .ep_deinit = vt_ep_deinit,
    .ep_stall = vt_ep_stall,
    .ep_xfer = vt_ep_xfer,
    .ep_cancel = vt_ep_cancel,
    .ep0_read = vt_ep0_read,
    .ep0_write = vt_ep0_write,
    .ep0_status = vt_ep0_status,
    .get_caps = vt_get_caps,
    .get_phys_index = vt_get_phys_index,
};

/* ================ defaults ================ */

extern void esy_hal_usb_dev_default_cfg(esy_usb_dev_config_t *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->ep0_mps = 64u;
}

/* ================ lifecycle ================ */

void esy_hal_usb_dev_board_init(void) { esy_hb_usb_dev_board_init(); }

extern esy_err_t esy_hal_usb_dev_init(esy_usb_dev_t *dev,
                                           const esy_usb_dev_config_t *cfg) {
  if (!dev || !cfg) return ESY_ERR_INVALID_ARG;
  if (esy_hal_usb_runtime(dev)->device != NULL || dev->ops != NULL)
    return ESY_ERR_STATE;

  /* Configure board pins and pull-ups for the device role. */
  //   esy_board_usb_dev_configure_pins(cfg->phys_index,
  //   &((esy_usb_dev_config_t*)cfg)->inner);

  dev->ops = &k_usb_ops;

  /* Call HAL backend init/deinit directly without routing through ops */
  esy_err_t ret = esy_hb_usb_dev_init(dev, cfg);
  if (ret != ESY_OK) {
    esy_hb_usb_dev_deinit(dev);
    memset(dev, 0, sizeof(*dev));
  }
  return ret;
}

extern void esy_hal_usb_dev_deinit(esy_usb_dev_t *dev) {
  if (!dev || (esy_hal_usb_runtime(dev)->device != NULL)) return;

  /* Release board resources after querying the physical index from the HAL backend.
   */
  //   uint8_t idx = 0xFF;
  //   if (esy_hb_usb_dev_get_phys_index(dev, &idx) == ESY_OK) {
  //     esy_board_usb_dev_reset_pins(idx);
  //   }

  esy_hb_usb_dev_deinit(dev);
  memset(dev, 0, sizeof(*dev));
}

esy_err_t esy_hal_usb_dev_register_device(
    esy_usb_dev_device_binding_t *binding, esy_usb_dev_t *usb,
    esy_device_registry_t *registry,
    const esy_device_registration_config_t *device_config) {
  const uint32_t deep_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
  esy_device_config_t config;
  esy_err_t ret;

  if ((binding == NULL) || (usb == NULL) || (registry == NULL) ||
      (device_config == NULL) || (esy_hal_usb_runtime(usb)->device != NULL) ||
      ((device_config->supported_state_mask & deep_mask) != 0u)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (binding->usb != NULL) return ESY_ERR_STATE;
  memset(binding, 0, sizeof(*binding));
  binding->usb = usb;
  config.name = device_config->name;
  config.ops = NULL;
  config.driver_ctx = binding;
  config.dependencies = device_config->dependencies;
  config.dependency_count = device_config->dependency_count;
  config.supported_state_mask = device_config->supported_state_mask;
  ret = esy_device_register(registry, &binding->device, &config);
  if (ret == ESY_OK) {
    esy_hal_usb_runtime(usb)->device = &binding->device;
  } else {
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

esy_err_t esy_hal_usb_dev_unregister_device(
    esy_usb_dev_device_binding_t *binding) {
  esy_err_t ret;

  if ((binding == NULL) || (binding->usb == NULL) ||
      (esy_hal_usb_runtime(binding->usb)->device != &binding->device)) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = binding->device.registry == NULL
            ? ESY_OK : esy_device_unregister(&binding->device);
  if (ret == ESY_OK) {
    esy_hal_usb_runtime(binding->usb)->device = NULL;
    memset(binding, 0, sizeof(*binding));
  }
  return ret;
}

/* ================ queries ================ */

extern esy_err_t esy_hal_usb_dev_get_caps(const esy_usb_dev_t *dev,
                                               uint32_t *out_caps) {
  if (!dev || !out_caps) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->get_caps) return ESY_ERR_NOT_IMPLEMENTED;
  return dev->ops->get_caps(dev, out_caps);
}

extern esy_err_t esy_hal_usb_dev_get_phys_index(const esy_usb_dev_t *dev,
                                                     uint8_t *out_index) {
  if (!dev || !out_index) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->get_phys_index) return ESY_ERR_NOT_IMPLEMENTED;
  return dev->ops->get_phys_index(dev, out_index);
}

/* ================ control & endpoints ================ */

extern esy_err_t esy_hal_usb_dev_set_address(esy_usb_dev_t *dev,
                                                  uint8_t addr) {
  esy_err_t ret;

  if (!dev) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->set_address) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->set_address(dev, addr);
  usb_release(dev);
  return ret;
}

extern esy_err_t esy_hal_usb_dev_ep_init(esy_usb_dev_t *dev,
                                              const esy_usb_ep_cfg_t *cfg) {
  esy_err_t ret;

  if (!dev || !cfg) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->ep_init) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->ep_init(dev, cfg);
  usb_release(dev);
  return ret;
}

extern esy_err_t esy_hal_usb_dev_ep_deinit(esy_usb_dev_t *dev,
                                                uint8_t ep_addr) {
  esy_err_t ret;

  if (!dev) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->ep_deinit) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->ep_deinit(dev, ep_addr);
  usb_release(dev);
  return ret;
}

extern esy_err_t esy_hal_usb_dev_ep_stall(esy_usb_dev_t *dev,
                                               uint8_t ep_addr, bool stall) {
  esy_err_t ret;

  if (!dev) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->ep_stall) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->ep_stall(dev, ep_addr, stall);
  usb_release(dev);
  return ret;
}

extern esy_err_t esy_hal_usb_dev_ep_xfer(esy_usb_dev_t *dev,
                                              uint8_t ep_addr,
                                              esy_usb_xfer_t *xfer) {
  esy_err_t ret;

  if (!dev || !xfer || (!xfer->buf && xfer->len)) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->ep_xfer) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->ep_xfer(dev, ep_addr, xfer);
  usb_release(dev);
  return ret;
}

extern esy_err_t esy_hal_usb_dev_ep_cancel(esy_usb_dev_t *dev,
                                                uint8_t ep_addr) {
  esy_err_t ret;

  if (!dev) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->ep_cancel) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->ep_cancel(dev, ep_addr);
  usb_release(dev);
  return ret;
}

/* ================ EP0 helpers (optional) ================ */

extern esy_err_t esy_hal_usb_dev_ep0_read(esy_usb_dev_t *dev, uint8_t *buf,
                                               uint16_t len) {
  esy_err_t ret;

  if (!dev || (!buf && len)) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->ep0_read) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->ep0_read(dev, buf, len);
  usb_release(dev);
  return ret;
}

extern esy_err_t esy_hal_usb_dev_ep0_write(esy_usb_dev_t *dev,
                                                const uint8_t *buf,
                                                uint16_t len) {
  esy_err_t ret;

  if (!dev || (!buf && len)) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->ep0_write) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->ep0_write(dev, buf, len);
  usb_release(dev);
  return ret;
}

extern esy_err_t esy_hal_usb_dev_ep0_status(esy_usb_dev_t *dev) {
  esy_err_t ret;

  if (!dev) return ESY_ERR_INVALID_ARG;
  if (dev->ops == NULL || !dev->ops->ep0_status) return ESY_ERR_NOT_SUPPORTED;
  ret = usb_acquire(dev);
  if (ret != ESY_OK) return ret;
  ret = dev->ops->ep0_status(dev);
  usb_release(dev);
  return ret;
}
