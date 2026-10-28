/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/touch/esy_touch.h"
#include "esy_common/device/esy_device.h"

#include <string.h>

static esy_err_t touch_acquire(esy_touch_t *touch) {
  return (touch->device == NULL) ? ESY_OK : esy_device_get(touch->device);
}

static void touch_release(esy_touch_t *touch) {
  if (touch->device != NULL) {
    (void)esy_device_put(touch->device);
  }
}

bool esy_touch_is_attached(const esy_touch_t *touch) {
  return (touch != NULL) && (touch->ctx != NULL) && (touch->poll != NULL) &&
         (touch->get_sample != NULL);
}

esy_err_t esy_touch_detach(esy_touch_t *touch) {
  if (touch == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (touch->device != NULL) {
    return ESY_ERR_STATE;
  }

  memset(touch, 0, sizeof(*touch));
  return ESY_OK;
}

esy_err_t esy_touch_poll(esy_touch_t *touch) {
  esy_err_t ret;

  if (touch == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  if (!esy_touch_is_attached(touch)) {
    return ESY_ERR_STATE;
  }

  ret = touch_acquire(touch);
  if (ret != ESY_OK) return ret;
  ret = touch->poll(touch);
  touch_release(touch);
  return ret;
}

esy_err_t esy_touch_get_sample(esy_touch_t *touch, esy_touch_data_t *sample) {
  esy_err_t ret;

  if ((touch == NULL) || (sample == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  if (!esy_touch_is_attached(touch)) {
    return ESY_ERR_STATE;
  }

  ret = touch_acquire(touch);
  if (ret != ESY_OK) return ret;
  ret = touch->get_sample(sample, touch);
  touch_release(touch);
  return ret;
}

esy_err_t esy_touch_read_input(esy_touch_t *touch, uint16_t *x, uint16_t *y,
                               bool *pressed) {
  esy_err_t ret;

  if ((touch == NULL) || (x == NULL) || (y == NULL) || (pressed == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }

  if (!esy_touch_is_attached(touch)) {
    return ESY_ERR_STATE;
  }

  ret = touch_acquire(touch);
  if (ret != ESY_OK) return ret;
  ret = touch->poll(touch);
  if (ret != ESY_OK) {
    touch_release(touch);
    return ret;
  }

  esy_touch_data_t sample;
  ret = touch->get_sample(&sample, touch);
  if (ret != ESY_OK) {
    touch_release(touch);
    return ret;
  }

  *x = sample.x;
  *y = sample.y;
  *pressed = sample.is_pressed;
  touch_release(touch);
  return ESY_OK;
}
