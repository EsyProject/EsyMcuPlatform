/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_usb_dev.h"

#include <string.h>

#include <tusb.h>
#include <bsp/board_api.h>

#include "esy_common/defs/peripheral/esy_usb_defs.h"

typedef struct {
  uint8_t rhport;
  esy_usb_event_cb_t evt_cb;
  void *evt_arg;
  esy_usb_speed_t speed_hint;
  uint16_t ep0_mps;
  uint8_t phys_index;
  bool inited;
} pico_usb_dev_ctx_t;

typedef struct {
  esy_usb_dev_t *dev_singleton;
} esy_hb_usb_dev_module_ctx_t;

/* TinyUSB callbacks and every handle's lifecycle share one serialized owner.
 * This module owns only the event binding; the startup owner keeps the shared
 * TinyUSB/controller lifecycle, including final shutdown of all class users. */
static esy_hb_usb_dev_module_ctx_t s_ctx;

_Static_assert(ESY_USB_PRIV_BYTES >= sizeof(pico_usb_dev_ctx_t),
               "usb->backend.bytes is too small for pico_usb_dev_ctx_t");

static inline pico_usb_dev_ctx_t *get_ctx(const esy_usb_dev_t *dev) {
  return dev ? (pico_usb_dev_ctx_t *)dev->ctx : NULL;
}

void esy_hb_usb_dev_board_init(void) {
  board_init();
  tusb_init();
  if (board_init_after_tusb) {
    board_init_after_tusb();
  }
}

esy_err_t esy_hb_usb_dev_init(esy_usb_dev_t *dev,
                                  const esy_usb_dev_config_t *cfg) {
  if (!dev || !cfg || !cfg->event_cb) return ESY_ERR_INVALID_ARG;
  if (cfg->phys_index != 0u) return ESY_ERR_INVALID_ARG;
  if (dev->ctx != NULL) return ESY_ERR_STATE;
  if (s_ctx.dev_singleton != NULL) return ESY_ERR_BUSY;

  if (sizeof(pico_usb_dev_ctx_t) > sizeof(dev->backend.bytes)) {
    return ESY_ERR_NO_MEM;
  }

  pico_usb_dev_ctx_t *c = (pico_usb_dev_ctx_t *)(void *)dev->backend.bytes;
  memset(c, 0, sizeof(*c));
  c->rhport = 0;
  c->evt_cb = cfg->event_cb;
  c->evt_arg = cfg->event_arg;
  c->speed_hint = cfg->speed_hint;
  c->ep0_mps = cfg->ep0_mps;
  c->phys_index = cfg->phys_index;
  c->inited = true;

  dev->ctx = c;
  s_ctx.dev_singleton = dev;

  return ESY_OK;
}

void esy_hb_usb_dev_deinit(esy_usb_dev_t *dev) {
  if (!dev || !dev->ctx || s_ctx.dev_singleton != dev) return;
  pico_usb_dev_ctx_t *c = get_ctx(dev);

  /* Dispatch is quiescent on this owner. Detach only our callback binding;
   * unrelated TinyUSB users and the shared controller keep their lifecycle. */
  s_ctx.dev_singleton = NULL;
  c->inited = false;
  dev->ctx = NULL;
}

esy_err_t esy_hb_usb_dev_get_caps(const esy_usb_dev_t *dev,
                                      uint32_t *out_caps) {
  (void)dev;
  if (!out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_USB_CAP_DEVICE;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_get_phys_index(const esy_usb_dev_t *dev,
                                           uint8_t *out_index) {
  if (!dev || !out_index) return ESY_ERR_INVALID_ARG;
  pico_usb_dev_ctx_t *c = get_ctx(dev);
  if (!c || !c->inited) return ESY_ERR_FAIL;
  *out_index = c->phys_index;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_set_address(esy_usb_dev_t *dev, uint8_t addr) {
  pico_usb_dev_ctx_t *c = get_ctx(dev);
  if (!c || !c->inited) return ESY_ERR_INVALID_ARG;
  (void)addr;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_usb_dev_ep_init(esy_usb_dev_t *dev,
                                     const esy_usb_ep_cfg_t *cfg) {
  (void)dev;
  (void)cfg;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_usb_dev_ep_deinit(esy_usb_dev_t *dev, uint8_t ep_addr) {
  (void)dev;
  (void)ep_addr;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_usb_dev_ep_stall(esy_usb_dev_t *dev,
                                      uint8_t ep_addr,
                                      bool stall) {
  (void)dev;
  (void)ep_addr;
  (void)stall;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_usb_dev_ep_xfer(esy_usb_dev_t *dev,
                                     uint8_t ep_addr,
                                     esy_usb_xfer_t *xfer) {
  (void)dev;
  (void)ep_addr;
  (void)xfer;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_usb_dev_ep_cancel(esy_usb_dev_t *dev, uint8_t ep_addr) {
  (void)dev;
  (void)ep_addr;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_usb_dev_ep0_read(esy_usb_dev_t *dev,
                                      uint8_t *buf,
                                      uint16_t len) {
  (void)dev;
  (void)buf;
  (void)len;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_usb_dev_ep0_write(esy_usb_dev_t *dev,
                                       const uint8_t *buf,
                                       uint16_t len) {
  (void)dev;
  (void)buf;
  (void)len;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_usb_dev_ep0_status(esy_usb_dev_t *dev) {
  (void)dev;
  return ESY_ERR_NOT_SUPPORTED;
}

void tud_mount_cb(void) {
  if (!s_ctx.dev_singleton) return;
  pico_usb_dev_ctx_t *c = get_ctx(s_ctx.dev_singleton);
  if (!c || !c->evt_cb) return;

  esy_usb_event_t ev = {0};
  ev.type = ESY_USB_EVT_CONNECT;
  switch (tud_speed_get()) {
    case TUSB_SPEED_LOW:  ev.u.connect.speed = ESY_USB_SPEED_LOW;  break;
    case TUSB_SPEED_FULL: ev.u.connect.speed = ESY_USB_SPEED_FULL; break;
    case TUSB_SPEED_HIGH: ev.u.connect.speed = ESY_USB_SPEED_HIGH; break;
    default:              ev.u.connect.speed = ESY_USB_SPEED_FULL; break;
  }
  c->evt_cb(c->evt_arg, &ev);
}

void tud_umount_cb(void) {
  if (!s_ctx.dev_singleton) return;
  pico_usb_dev_ctx_t *c = get_ctx(s_ctx.dev_singleton);
  if (!c || !c->evt_cb) return;

  esy_usb_event_t ev = { .type = ESY_USB_EVT_DISCONNECT };
  c->evt_cb(c->evt_arg, &ev);
}

void tud_suspend_cb(bool remote_wakeup_en) {
  (void)remote_wakeup_en;
  if (!s_ctx.dev_singleton) return;
  pico_usb_dev_ctx_t *c = get_ctx(s_ctx.dev_singleton);
  if (!c || !c->evt_cb) return;

  esy_usb_event_t ev = { .type = ESY_USB_EVT_SUSPEND };
  c->evt_cb(c->evt_arg, &ev);
}

void tud_resume_cb(void) {
  if (!s_ctx.dev_singleton) return;
  pico_usb_dev_ctx_t *c = get_ctx(s_ctx.dev_singleton);
  if (!c || !c->evt_cb) return;

  esy_usb_event_t ev = { .type = ESY_USB_EVT_RESUME };
  c->evt_cb(c->evt_arg, &ev);
}

void tud_sof_cb(uint32_t frame_count) {
  (void)frame_count;
  if (!s_ctx.dev_singleton) return;
  pico_usb_dev_ctx_t *c = get_ctx(s_ctx.dev_singleton);
  if (!c || !c->evt_cb) return;

  esy_usb_event_t ev = { .type = ESY_USB_EVT_SOF };
  c->evt_cb(c->evt_arg, &ev);
}
