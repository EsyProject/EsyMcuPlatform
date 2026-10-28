/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/touch/esy_xpt2046.h"
#include "esy_xpt2046_internal.h"

#include <stddef.h>
#include <string.h>

#include "esy_hal/esy_hal_gpio.h"

#define CMD_X                       0xD0u /* 1101 0000: read X+ */
#define CMD_Y                       0x90u /* 1001 0000: read Y+ */
#define CMD_Z1                      0xB0u /* 1011 0000: read Z1 */
#define CMD_Z2                      0xC0u /* 1100 0000: read Z2 */
#define ADC_TRANSFER_BYTES          3u
#define XPT2046_ADC_FULL_SCALE      4096u

/* -------------------------------------------------------------------------- */
/* Private context (hidden)                                                   */
/* -------------------------------------------------------------------------- */

/* Ensure the unified touch handle storage is large enough for this driver. */
_Static_assert(ESY_TOUCH_PRIV_BYTES >= sizeof(esy_xpt2046_ctx_t),
               "ESY_TOUCH_PRIV_BYTES too small for XPT2046 context");

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

static void toggle_current_touch_data(esy_xpt2046_ctx_t *ctx) {
  if (ctx->cur_touch_data == &ctx->touch_data[0]) {
    ctx->cur_touch_data = &ctx->touch_data[1];
  } else {
    ctx->cur_touch_data = &ctx->touch_data[0];
  }
}

static esy_touch_data_t *get_back_touch_data(esy_xpt2046_ctx_t *ctx) {
  if (ctx->cur_touch_data == &ctx->touch_data[0]) {
    return &ctx->touch_data[1];
  } else {
    return &ctx->touch_data[0];
  }
}

static bool is_valid_dir(esy_lcd_dir_t dir) {
  return (dir == ESY_LCD_DIR_D2U_L2R) || (dir == ESY_LCD_DIR_L2R_U2D) ||
         (dir == ESY_LCD_DIR_R2L_D2U) || (dir == ESY_LCD_DIR_U2D_R2L);
}

static esy_err_t validate_config(const esy_xpt2046_config_t *config) {
  if (!config) return ESY_ERR_INVALID_ARG;
  if (!config->spi_get_write_buffer_cb || !config->spi_submit_result_cb)
    return ESY_ERR_INVALID_ARG;
  if ((config->cs_gpio == NULL) || !esy_hal_gpio_is_initialized(config->cs_gpio)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((config->irq_gpio != NULL) && !esy_hal_gpio_is_initialized(config->irq_gpio)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((config->width == 0u) || (config->height == 0u))
    return ESY_ERR_INVALID_ARG;
  if (!is_valid_dir(config->dir)) return ESY_ERR_INVALID_ARG;

  switch (config->calibration.mode) {
    case ESY_XPT2046_CALIBRATION_LINEAR:
      if ((config->calibration.linear.x_scale_q16_16 == 0) ||
          (config->calibration.linear.y_scale_q16_16 == 0)) {
        return ESY_ERR_INVALID_ARG;
      }
      break;

    case ESY_XPT2046_CALIBRATION_RANGE:
      if ((config->calibration.range.raw_x_min ==
           config->calibration.range.raw_x_max) ||
          (config->calibration.range.raw_y_min ==
           config->calibration.range.raw_y_max)) {
        return ESY_ERR_INVALID_ARG;
      }
      break;

    case ESY_XPT2046_CALIBRATION_AFFINE:
      break;

    default:
      return ESY_ERR_INVALID_ARG;
  }

  switch (config->touch_detect.mode) {
    case ESY_XPT2046_TOUCH_DETECT_ALWAYS:
      break;

    case ESY_XPT2046_TOUCH_DETECT_IRQ:
      if (config->irq_gpio == NULL) {
        return ESY_ERR_INVALID_ARG;
      }
      break;

    case ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD:
      if ((config->touch_detect.x_plate_ohms == 0u) ||
          (config->touch_detect.pressure_max_ohms == 0u)) {
        return ESY_ERR_INVALID_ARG;
      }
      break;

    default:
      return ESY_ERR_INVALID_ARG;
  }

  return ESY_OK;
}

static int64_t apply_fixed_scale(uint16_t raw, int32_t scale_q16_16,
                                 int32_t offset) {
  return ((int64_t)raw * scale_q16_16) / ESY_XPT2046_Q16_16_ONE +
         (int64_t)offset;
}

static uint16_t clamp_to_axis(int64_t value, uint16_t limit) {
  if (value < 0) return 0u;
  if (value >= (int32_t)limit) return (uint16_t)(limit - 1u);
  return (uint16_t)value;
}

static uint16_t normalize_range_axis(uint16_t raw, uint16_t raw_min,
                                     uint16_t raw_max, uint16_t limit) {
  int32_t span = (int32_t)raw_max - (int32_t)raw_min;
  int32_t value = (int32_t)raw - (int32_t)raw_min;

  if (span < 0) {
    span = -span;
    value = (int32_t)raw_min - (int32_t)raw;
  }

  if (value < 0) value = 0;
  if (value > span) value = span;

  return (uint16_t)(((int64_t)value * (int64_t)(limit - 1u)) / (int64_t)span);
}

static int32_t apply_affine_axis(uint16_t raw_x, uint16_t raw_y,
                                 int32_t a_q16_16, int32_t b_q16_16,
                                 int32_t c_q16_16) {
  int64_t value = ((int64_t)raw_x * (int64_t)a_q16_16) +
                  ((int64_t)raw_y * (int64_t)b_q16_16) + (int64_t)c_q16_16;

  return (int32_t)(value / (int64_t)ESY_XPT2046_Q16_16_ONE);
}

static void apply_post_flags(const esy_xpt2046_ctx_t *ctx, int32_t *x,
                             int32_t *y) {
  if ((ctx->calibration.flags & ESY_XPT2046_CALIBRATION_FLAG_SWAP_XY) != 0u) {
    int32_t tmp = *x;
    *x = *y;
    *y = tmp;
  }

  if ((ctx->calibration.flags & ESY_XPT2046_CALIBRATION_FLAG_INVERT_X) != 0u) {
    *x = (int32_t)ctx->width - 1 - *x;
  }

  if ((ctx->calibration.flags & ESY_XPT2046_CALIBRATION_FLAG_INVERT_Y) != 0u) {
    *y = (int32_t)ctx->height - 1 - *y;
  }
}

static void transform_linear_data(esy_xpt2046_ctx_t *ctx,
                                  esy_touch_data_t *data) {
  uint16_t raw_x = data->x;
  uint16_t raw_y = data->y;
  int64_t x = 0;
  int64_t y = 0;
  const esy_xpt2046_linear_calibration_t *cal = &ctx->calibration.linear;

  switch (ctx->dir) {
    case ESY_LCD_DIR_R2L_D2U:
      x = apply_fixed_scale(raw_x, cal->x_scale_q16_16, cal->x_offset);
      y = apply_fixed_scale(raw_y, cal->y_scale_q16_16, cal->y_offset);
      break;

    case ESY_LCD_DIR_L2R_U2D:
      x = (int64_t)ctx->width -
          apply_fixed_scale(raw_x, cal->x_scale_q16_16, cal->x_offset);
      y = (int64_t)ctx->height -
          apply_fixed_scale(raw_y, cal->y_scale_q16_16, cal->y_offset);
      break;

    case ESY_LCD_DIR_U2D_R2L:
      x = apply_fixed_scale(raw_y, cal->x_scale_q16_16, cal->x_offset);
      y = apply_fixed_scale(raw_x, cal->y_scale_q16_16, cal->y_offset);
      break;

    case ESY_LCD_DIR_D2U_L2R:
    default:
      x = (int64_t)ctx->width -
          apply_fixed_scale(raw_y, cal->x_scale_q16_16, cal->x_offset);
      y = (int64_t)ctx->height -
          apply_fixed_scale(raw_x, cal->y_scale_q16_16, cal->y_offset);
      break;
  }

  data->x = clamp_to_axis(x, ctx->width);
  data->y = clamp_to_axis(y, ctx->height);
}

static void transform_range_data(esy_xpt2046_ctx_t *ctx,
                                 esy_touch_data_t *data) {
  uint16_t raw_x = data->x;
  uint16_t raw_y = data->y;
  const esy_xpt2046_range_calibration_t *range = &ctx->calibration.range;
  const bool swap =
      (ctx->calibration.flags & ESY_XPT2046_CALIBRATION_FLAG_SWAP_XY) != 0u;
  /* Each raw axis must span its destination axis after the optional swap. */
  const uint16_t x_limit = swap ? ctx->height : ctx->width;
  const uint16_t y_limit = swap ? ctx->width : ctx->height;
  int32_t x = (int32_t)normalize_range_axis(raw_x, range->raw_x_min,
                                            range->raw_x_max, x_limit);
  int32_t y = (int32_t)normalize_range_axis(raw_y, range->raw_y_min,
                                            range->raw_y_max, y_limit);

  apply_post_flags(ctx, &x, &y);

  data->x = clamp_to_axis(x, ctx->width);
  data->y = clamp_to_axis(y, ctx->height);
}

static void transform_affine_data(esy_xpt2046_ctx_t *ctx,
                                  esy_touch_data_t *data) {
  uint16_t raw_x = data->x;
  uint16_t raw_y = data->y;
  const esy_xpt2046_affine_calibration_t *affine = &ctx->calibration.affine;
  int32_t x = apply_affine_axis(raw_x, raw_y, affine->ax_q16_16,
                                affine->bx_q16_16, affine->cx_q16_16);
  int32_t y = apply_affine_axis(raw_x, raw_y, affine->ay_q16_16,
                                affine->by_q16_16, affine->cy_q16_16);

  apply_post_flags(ctx, &x, &y);

  data->x = clamp_to_axis(x, ctx->width);
  data->y = clamp_to_axis(y, ctx->height);
}

static void transform_data(esy_xpt2046_ctx_t *ctx, esy_touch_data_t *data) {
  switch (ctx->calibration.mode) {
    case ESY_XPT2046_CALIBRATION_RANGE:
      transform_range_data(ctx, data);
      break;

    case ESY_XPT2046_CALIBRATION_AFFINE:
      transform_affine_data(ctx, data);
      break;

    case ESY_XPT2046_CALIBRATION_LINEAR:
    default:
      transform_linear_data(ctx, data);
      break;
  }
}

static uint32_t estimate_touch_resistance_ohms(const esy_xpt2046_ctx_t *ctx) {
  uint32_t z1 = ctx->pending_z1;
  uint32_t z2 = ctx->pending_z2;

  if ((z1 == 0u) || (z2 <= z1)) {
    return UINT32_MAX;
  }

  return (uint32_t)((((uint64_t)ctx->pending_touch_data.x *
                      (uint64_t)ctx->touch_detect.x_plate_ohms *
                      (uint64_t)(z2 - z1)) /
                     (uint64_t)z1) /
                    (uint64_t)XPT2046_ADC_FULL_SCALE);
}

static bool detect_touch_pressed(esy_xpt2046_ctx_t *ctx) {
  switch (ctx->touch_detect.mode) {
    case ESY_XPT2046_TOUCH_DETECT_IRQ: {
      bool level = false;
      esy_err_t result = esy_hal_gpio_read(ctx->irq_gpio, &level);
      if (result != ESY_OK) ctx->pending_error = result;
      return result == ESY_OK && !level;
    }

    case ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD:
      return estimate_touch_resistance_ohms(ctx) <=
             (uint32_t)ctx->touch_detect.pressure_max_ohms;

    case ESY_XPT2046_TOUCH_DETECT_ALWAYS:
    default:
      return true;
  }
}

/* -------------------------------------------------------------------------- */
/* SPI callbacks                                                              */
/* -------------------------------------------------------------------------- */

static void pre_read_adc(void *arg) {
  const esy_xpt2046_transfer_ctx_t *transfer =
      (const esy_xpt2046_transfer_ctx_t *)arg;
  esy_xpt2046_ctx_t *ctx = transfer->owner;
  if (transfer->generation != ctx->active_generation) return;
  esy_err_t result = esy_hal_gpio_write(ctx->cs_gpio, false);
  if (result != ESY_OK) ctx->pending_error = result;
}

static uint16_t decode_adc_value(const uint8_t *rx_buf) {
  return (uint16_t)((((uint16_t)rx_buf[1] << 8) | (uint16_t)rx_buf[2]) >> 3);
}

static void commit_pending_sample(esy_xpt2046_ctx_t *ctx) {
  esy_touch_data_t *data = get_back_touch_data(ctx);
  *data = ctx->pending_touch_data;
  data->is_pressed = detect_touch_pressed(ctx);
  if (ctx->pending_error != ESY_OK) {
    ctx->sample_state = XPT2046_SAMPLE_IDLE;
    return;
  }

  transform_data(ctx, data);
  toggle_current_touch_data(ctx);
  ctx->pending_error = ESY_OK;
  ctx->sample_state = XPT2046_SAMPLE_IDLE;
}

static void store_adc_value(esy_xpt2046_ctx_t *ctx,
                            esy_xpt2046_channel_t channel, uint16_t value) {
  switch (channel) {
    case XPT2046_CHANNEL_X:
      ctx->pending_touch_data.x = value;
      break;

    case XPT2046_CHANNEL_Y:
      ctx->pending_touch_data.y = value;
      break;

    case XPT2046_CHANNEL_Z1:
      ctx->pending_z1 = value;
      break;

    case XPT2046_CHANNEL_Z2:
      ctx->pending_z2 = value;
      break;

    default:
      break;
  }
}

static void post_read_adc_result(esy_err_t result, uint8_t *rx_buf, void *arg) {
  const esy_xpt2046_transfer_ctx_t *transfer =
      (const esy_xpt2046_transfer_ctx_t *)arg;
  esy_xpt2046_ctx_t *ctx = transfer->owner;
  if (transfer->generation != ctx->active_generation) return;
  esy_err_t cleanup = esy_hal_gpio_write(ctx->cs_gpio, true);
  ctx->cs_cleanup_pending = cleanup != ESY_OK;
  if (cleanup != ESY_OK) ctx->pending_error = cleanup;

  if (result != ESY_OK || !rx_buf) {
    ctx->pending_error = result != ESY_OK ? result : ESY_ERR_IO;
  } else {
    store_adc_value(ctx, (esy_xpt2046_channel_t)transfer->channel, decode_adc_value(rx_buf));
  }
  if (ctx->completed_transfer_count < ctx->expected_transfer_count) {
    ctx->completed_transfer_count++;
  }

  if (ctx->completed_transfer_count < ctx->expected_transfer_count) {
    return;
  }

  if (ctx->sample_state == XPT2046_SAMPLE_ABORTING) {
    ctx->sample_state = XPT2046_SAMPLE_IDLE;
    return;
  }

  if (ctx->sample_state == XPT2046_SAMPLE_READING) {
    if (ctx->pending_error != ESY_OK) {
      ctx->sample_state = XPT2046_SAMPLE_IDLE;
      return;
    }
    commit_pending_sample(ctx);
  }
}

/* -------------------------------------------------------------------------- */
/* Core read                                                                  */
/* -------------------------------------------------------------------------- */

static esy_err_t read_adc(const esy_xpt2046_ctx_t *ctx, uint8_t cmd,
                          const esy_xpt2046_transfer_ctx_t *transfer) {
  uint8_t *tx_buf = NULL;
  uint8_t *rx_buf = NULL;
  esy_err_t err = ctx->spi_get_write_buffer_cb(
      &tx_buf, &rx_buf, ADC_TRANSFER_BYTES, ctx->spi_get_write_buffer_cb_arg);

  if (err != ESY_OK) {
    return err;
  }

  if (!tx_buf || !rx_buf) {
    return ESY_ERR_INVALID_ARG;
  }

  tx_buf[0] = cmd;
  tx_buf[1] = 0x00u;
  tx_buf[2] = 0x00u;

  rx_buf[0] = 0x00u;
  rx_buf[1] = 0x00u;
  rx_buf[2] = 0x00u;

  return ctx->spi_submit_result_cb(tx_buf, rx_buf, ADC_TRANSFER_BYTES, pre_read_adc,
      (void *)transfer, post_read_adc_result, (void *)transfer, ctx->spi_submit_cb_arg);
}

static uint8_t sample_transfer_count(const esy_xpt2046_ctx_t *ctx) {
  if (ctx->touch_detect.mode == ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD) {
    return 4u;
  }

  return 2u;
}

static void prepare_transfer(esy_xpt2046_ctx_t *ctx,
                             esy_xpt2046_channel_t channel) {
  esy_xpt2046_transfer_ctx_t *transfer =
      &ctx->transfer[ctx->active_transfer_slot][channel];

  transfer->owner = ctx;
  transfer->generation = ctx->active_generation;
  transfer->channel = (uint8_t)channel;
}

static esy_err_t submit_sample_transfer(esy_xpt2046_ctx_t *ctx,
                                        esy_xpt2046_channel_t channel,
                                        uint8_t cmd) {
  esy_err_t err;

  prepare_transfer(ctx, channel);
  err = read_adc(ctx, cmd, &ctx->transfer[ctx->active_transfer_slot][channel]);
  if (err == ESY_OK) {
    ctx->submitted_transfer_count++;
  }

  return err;
}

static esy_err_t abort_pending_sample(esy_xpt2046_ctx_t *ctx, esy_err_t err) {
  ctx->pending_error = err;
  ctx->expected_transfer_count = ctx->submitted_transfer_count;

  if (ctx->completed_transfer_count >= ctx->expected_transfer_count) {
    ctx->sample_state = XPT2046_SAMPLE_IDLE;
  } else {
    ctx->sample_state = XPT2046_SAMPLE_ABORTING;
  }

  return err;
}

static esy_err_t esy_xpt2046_read(esy_xpt2046_ctx_t *ctx) {
  esy_err_t err;

  if (ctx->sample_state != XPT2046_SAMPLE_IDLE) {
    return ctx->pending_error;
  }

  if (ctx->cs_cleanup_pending) {
    err = esy_hal_gpio_write(ctx->cs_gpio, true);
    if (err != ESY_OK) return err;
    ctx->cs_cleanup_pending = false;
  }
  memset(&ctx->pending_touch_data, 0, sizeof(ctx->pending_touch_data));
  ctx->pending_z1 = 0u;
  ctx->pending_z2 = 0u;
  ctx->sample_state = XPT2046_SAMPLE_READING;
  ctx->pending_error = ESY_OK;
  ctx->expected_transfer_count = sample_transfer_count(ctx);
  ctx->submitted_transfer_count = 0u;
  ctx->completed_transfer_count = 0u;
  ctx->active_generation++;
  ctx->active_transfer_slot ^= 1u;

  err = submit_sample_transfer(ctx, XPT2046_CHANNEL_X, CMD_X);
  if (err != ESY_OK) {
    ctx->sample_state = XPT2046_SAMPLE_IDLE;
    ctx->pending_error = err;
    return err;
  }

  err = submit_sample_transfer(ctx, XPT2046_CHANNEL_Y, CMD_Y);
  if (err != ESY_OK) {
    return abort_pending_sample(ctx, err);
  }

  if (ctx->touch_detect.mode == ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD) {
    err = submit_sample_transfer(ctx, XPT2046_CHANNEL_Z1, CMD_Z1);
    if (err != ESY_OK) {
      return abort_pending_sample(ctx, err);
    }

    err = submit_sample_transfer(ctx, XPT2046_CHANNEL_Z2, CMD_Z2);
    if (err != ESY_OK) {
      return abort_pending_sample(ctx, err);
    }
  }

  return ctx->pending_error;
}

static esy_err_t esy_xpt2046_poll(void *arg) {
  if (!arg) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_touch_t *touch = (esy_touch_t *)arg;
  if (!touch->ctx) {
    return ESY_ERR_STATE;
  }

  return esy_xpt2046_read((esy_xpt2046_ctx_t *)touch->ctx);
}

static esy_err_t esy_xpt2046_get_sample(esy_touch_data_t *sample, void *arg) {
  if (!sample || !arg) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_touch_t *touch = (esy_touch_t *)arg;
  if (!touch->ctx) {
    return ESY_ERR_STATE;
  }

  esy_xpt2046_ctx_t *ctx = (esy_xpt2046_ctx_t *)touch->ctx;
  if (ctx->pending_error != ESY_OK) return ctx->pending_error;
  *sample = *ctx->cur_touch_data;
  return ESY_OK;
}

ESY_INTERNAL esy_xpt2046_ctx_t *esy_xpt2046_context(esy_touch_t *touch) {
  if ((touch == NULL) || (touch->ctx == NULL) ||
      (touch->poll != esy_xpt2046_poll) ||
      (touch->get_sample != esy_xpt2046_get_sample)) {
    return NULL;
  }
  return (esy_xpt2046_ctx_t *)touch->ctx;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  crc = ~crc;
  for (size_t i = 0u; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint8_t bit = 0u; bit < 8u; bit++) {
      uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

static uint32_t calibration_blob_crc(
    const esy_xpt2046_calibration_blob_t *blob) {
  return crc32_update(0u, (const uint8_t *)blob,
                      offsetof(esy_xpt2046_calibration_blob_t, crc32));
}

void esy_xpt2046_default_config(esy_xpt2046_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->cs_gpio = NULL;
  config->irq_gpio = NULL;
  config->calibration.mode = ESY_XPT2046_CALIBRATION_LINEAR;
  config->calibration.linear.x_scale_q16_16 = ESY_XPT2046_Q16_16_ONE;
  config->calibration.linear.y_scale_q16_16 = ESY_XPT2046_Q16_16_ONE;
  config->touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_ALWAYS;
}

esy_err_t esy_xpt2046_calibration_export(
    const esy_xpt2046_calibration_t *calibration,
    esy_xpt2046_calibration_blob_t *blob) {
  if (!calibration || !blob) {
    return ESY_ERR_INVALID_ARG;
  }

  memset(blob, 0, sizeof(*blob));
  blob->magic = ESY_XPT2046_CALIBRATION_BLOB_MAGIC;
  blob->version = ESY_XPT2046_CALIBRATION_BLOB_VERSION;
  blob->size = (uint16_t)sizeof(*blob);
  blob->calibration = *calibration;
  blob->crc32 = calibration_blob_crc(blob);

  return ESY_OK;
}

esy_err_t esy_xpt2046_calibration_import(
    const esy_xpt2046_calibration_blob_t *blob,
    esy_xpt2046_calibration_t *calibration) {
  if (!blob || !calibration) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((blob->magic != ESY_XPT2046_CALIBRATION_BLOB_MAGIC) ||
      (blob->version != ESY_XPT2046_CALIBRATION_BLOB_VERSION) ||
      (blob->size != (uint16_t)sizeof(*blob))) {
    return ESY_ERR_INVALID_ARG;
  }
  if (blob->crc32 != calibration_blob_crc(blob)) {
    return ESY_ERR_CRC_FAIL;
  }

  *calibration = blob->calibration;
  return ESY_OK;
}

esy_err_t esy_xpt2046_init(esy_touch_t *touch,
                           const esy_xpt2046_config_t *config) {
  if (!touch) return ESY_ERR_INVALID_ARG;
  if ((touch->ctx != NULL) || (touch->poll != NULL) ||
      (touch->get_sample != NULL) || (touch->device != NULL)) {
    return ESY_ERR_STATE;
  }

  esy_err_t err = validate_config(config);
  if (err != ESY_OK) return err;

  /* Place the private context into touch->priv.bytes (Scheme C). */
  esy_xpt2046_ctx_t *ctx = (esy_xpt2046_ctx_t *)(void *)&touch->priv.bytes[0];
  memset(ctx, 0, sizeof(*ctx));

  ctx->magic = XPT2046_CTX_MAGIC;
  ctx->dir = config->dir;
  ctx->width = config->width;
  ctx->height = config->height;
  ctx->cs_gpio = config->cs_gpio;
  ctx->irq_gpio = config->irq_gpio;
  ctx->spi_device = config->spi_device;
  ctx->calibration = config->calibration;
  ctx->touch_detect = config->touch_detect;

  ctx->spi_get_write_buffer_cb = config->spi_get_write_buffer_cb;
  ctx->spi_get_write_buffer_cb_arg = config->spi_get_write_buffer_cb_arg;
  ctx->spi_submit_result_cb = config->spi_submit_result_cb;
  ctx->spi_submit_cb_arg = config->spi_submit_cb_arg;

  ctx->cur_touch_data = &ctx->touch_data[0];
  ctx->touch_data[0].is_pressed = false;
  ctx->touch_data[1].is_pressed = false;

  touch->device = NULL;
  touch->ctx = ctx;
  touch->poll = esy_xpt2046_poll;
  touch->get_sample = esy_xpt2046_get_sample;

  return ESY_OK;
}
