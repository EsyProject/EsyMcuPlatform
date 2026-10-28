/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lcd/esy_lcd.h"
#include "esy_middleware/lcd/esy_lcd_device.h"

#include <string.h>

static esy_lcd_device_binding_t *lcd_binding(esy_lcd_t *lcd) {
  if ((lcd == NULL) || (lcd->device == NULL)) return NULL;
  return (esy_lcd_device_binding_t *)lcd->device->driver_ctx;
}

static void lcd_flush_complete(void *arg) {
  esy_lcd_device_binding_t *binding = (esy_lcd_device_binding_t *)arg;
  void (*done_cb)(void *) = binding->flush_done_cb;
  void *done_arg = binding->flush_done_arg;

  binding->flush_done_cb = NULL;
  binding->flush_done_arg = NULL;
  (void)esy_device_put(&binding->device);
  atomic_store_explicit(&binding->flush_ref_held, 0u, memory_order_release);
  if (done_cb != NULL) done_cb(done_arg);
}

bool esy_lcd_is_attached(const esy_lcd_t *lcd) {
  return (lcd != NULL) && (lcd->ctx != NULL) && (lcd->flush != NULL);
}

esy_err_t esy_lcd_detach(esy_lcd_t *lcd) {
  if (lcd == NULL) return ESY_OK;
  if (lcd->device != NULL) return ESY_ERR_BUSY;
  if (lcd->deinit != NULL) return lcd->deinit(lcd);
  memset(lcd, 0, sizeof(*lcd));
  return ESY_OK;
}

esy_err_t esy_lcd_flush(esy_lcd_t *lcd, uint16_t x0, uint16_t y0, uint16_t x1,
                        uint16_t y1, const uint8_t *color_buf,
                        void (*done_cb)(void *), void *cb_arg) {
  if ((lcd == NULL) || (color_buf == NULL) || (x0 > x1) || (y0 > y1)) {
    return ESY_ERR_INVALID_ARG;
  }

  if (!esy_lcd_is_attached(lcd)) {
    return ESY_ERR_STATE;
  }

  esy_lcd_device_binding_t *binding = lcd_binding(lcd);
  if (binding == NULL) {
    /* Registry teardown invalidates the device before binding cleanup. */
    if (lcd->device != NULL) return ESY_ERR_STATE;
    return lcd->flush(x0, y0, x1, y1, color_buf, done_cb, cb_arg, lcd);
  }
  uint32_t expected = 0u;
  if (!atomic_compare_exchange_strong_explicit(
          &binding->flush_ref_held, &expected, 1u, memory_order_acquire,
          memory_order_relaxed)) {
    return ESY_ERR_BUSY;
  }

  esy_err_t ret = esy_device_get(&binding->device);
  if (ret != ESY_OK) {
    atomic_store_explicit(&binding->flush_ref_held, 0u, memory_order_release);
    return ret;
  }
  binding->flush_done_cb = done_cb;
  binding->flush_done_arg = cb_arg;
  ret = lcd->flush(x0, y0, x1, y1, color_buf, lcd_flush_complete, binding, lcd);
  if (ret != ESY_OK) {
    binding->flush_done_cb = NULL;
    binding->flush_done_arg = NULL;
    (void)esy_device_put(&binding->device);
    atomic_store_explicit(&binding->flush_ref_held, 0u, memory_order_release);
  }
  return ret;
}
