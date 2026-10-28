/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_XPT2046_INTERNAL_H
#define ESY_XPT2046_INTERNAL_H

#include "esy_middleware/touch/esy_xpt2046.h"
#include "esy_common/device/esy_device.h"

#define XPT2046_TRANSFER_SLOT_COUNT 2u
#define XPT2046_CTX_MAGIC 0x58505432u

typedef struct esy_xpt2046_ctx_s esy_xpt2046_ctx_t;

typedef struct {
  esy_xpt2046_ctx_t *owner;
  uint32_t generation;
  uint8_t channel;
} esy_xpt2046_transfer_ctx_t;

typedef enum {
  XPT2046_SAMPLE_IDLE = 0,
  XPT2046_SAMPLE_READING,
  XPT2046_SAMPLE_ABORTING
} esy_xpt2046_sample_state_t;

typedef enum {
  XPT2046_CHANNEL_X = 0,
  XPT2046_CHANNEL_Y,
  XPT2046_CHANNEL_Z1,
  XPT2046_CHANNEL_Z2,
  XPT2046_CHANNEL_COUNT
} esy_xpt2046_channel_t;

struct esy_xpt2046_ctx_s {
  uint32_t magic;
  esy_gpio_t *cs_gpio;
  esy_gpio_t *irq_gpio;
  esy_device_t *spi_device;
  uint16_t width;
  uint16_t height;
  esy_lcd_dir_t dir;

  esy_touch_spi_get_write_buffer spi_get_write_buffer_cb;
  void *spi_get_write_buffer_cb_arg;
  esy_spi_submit_result_t spi_submit_result_cb;
  void *spi_submit_cb_arg;

  esy_xpt2046_calibration_t calibration;
  esy_xpt2046_touch_detect_t touch_detect;

  esy_touch_data_t touch_data[2];
  esy_touch_data_t *cur_touch_data;
  esy_touch_data_t pending_touch_data;
  esy_xpt2046_transfer_ctx_t transfer[XPT2046_TRANSFER_SLOT_COUNT]
                                     [XPT2046_CHANNEL_COUNT];
  uint16_t pending_z1;
  uint16_t pending_z2;
  uint32_t active_generation;
  esy_err_t pending_error;
  esy_xpt2046_sample_state_t sample_state;
  uint8_t active_transfer_slot;
  uint8_t expected_transfer_count;
  uint8_t submitted_transfer_count;
  uint8_t completed_transfer_count;
  bool suspended;
  bool cs_cleanup_pending;
};

/**
 * @brief Obtain backend storage only from a handle using the XPT2046 callbacks.
 *
 * Module-private access for the optional device adapter. Foreign provider
 * contexts are never inspected. Serialize access with handle lifecycle calls.
 *
 * @param[in,out] touch Touch handle to identify.
 * @return Borrowed XPT2046 context, or NULL for a missing or foreign backend.
 */
ESY_INTERNAL esy_xpt2046_ctx_t *esy_xpt2046_context(esy_touch_t *touch);

#endif /* ESY_XPT2046_INTERNAL_H */
