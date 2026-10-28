/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lcd/esy_ili9488.h"
#include "esy_ili9488_internal.h"
#include "esy_common/device/esy_device.h"

#include <string.h>
#include <stdatomic.h>

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_system.h"

#define ILI9488_CMD_COLUMN_ADDRESS_SET 0x2Au
#define ILI9488_CMD_ROW_ADDRESS_SET    0x2Bu
#define ILI9488_CMD_MEMORY_WRITE       0x2Cu
#define ILI9488_CMD_MEMORY_ACCESS_CTRL 0x36u
#define ILI9488_CMD_PIXEL_FORMAT_SET   0x3Au
#define ILI9488_CMD_DISPLAY_FUNCTION   0xB6u
#define ILI9488_CMD_DISPLAY_INVERSION  0x21u
#define ILI9488_CMD_POWER_CONTROL_3    0xC2u
#define ILI9488_CMD_VCOM_CONTROL_1     0xC5u
#define ILI9488_CMD_FRAME_RATE_CONTROL 0xB1u
#define ILI9488_CMD_POSITIVE_GAMMA     0xE0u
#define ILI9488_CMD_NEGATIVE_GAMMA     0xE1u
#define ILI9488_CMD_SLEEP_IN           0x10u
#define ILI9488_CMD_SLEEP_OUT          0x11u
#define ILI9488_CMD_DISPLAY_OFF        0x28u
#define ILI9488_CMD_DISPLAY_ON         0x29u
#define ILI9488_RGB565_BYTES_PER_PIXEL 2u
#define ILI9488_PIXEL_FORMAT_RGB565    0x55u
#define ILI9488_MADCTL_BGR_BIT         0x08u
#define ILI9488_RESET_LOW_DELAY_MS     20u
#define ILI9488_RESET_HIGH_DELAY_MS    150u
#define ILI9488_SLEEP_OUT_DELAY_MS     120u
#define ILI9488_SLEEP_IN_DELAY_MS      120u

/* -------------------------------------------------------------------------- */
/* Private context (hidden)                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
  esy_lcd_dir_t dir;
  uint8_t madctl;
  uint8_t disfun;
} esy_ili9488_dir_cfg_t;

_Static_assert(ESY_LCD_PRIV_BYTES >= sizeof(esy_ili9488_ctx_t),
               "ESY_LCD_PRIV_BYTES too small for ILI9488 context");

static const uint8_t k_power_control_3[] = {0x33u};
static const uint8_t k_vcom_control_1[] = {0x00u, 0x1eu, 0x80u};
static const uint8_t k_frame_rate_control[] = {0xB0u};
static const uint8_t k_initial_memory_access_control[] = {0x28u};
static const uint8_t k_positive_gamma[] = {0x00u, 0x13u, 0x18u, 0x04u, 0x0Fu,
                                           0x06u, 0x3Au, 0x56u, 0x4Du, 0x03u,
                                           0x0Au, 0x06u, 0x30u, 0x3Eu, 0x0Fu};
static const uint8_t k_negative_gamma[] = {0x00u, 0x13u, 0x18u, 0x01u, 0x11u,
                                           0x06u, 0x38u, 0x34u, 0x4Du, 0x06u,
                                           0x0Du, 0x0Bu, 0x31u, 0x37u, 0x0Fu};
static const uint8_t k_pixel_format_rgb565[] = {ILI9488_PIXEL_FORMAT_RGB565};

static const esy_ili9488_command_t k_init_sequence[] = {
    {ILI9488_CMD_DISPLAY_INVERSION, NULL, 0u, 0u},
    {ILI9488_CMD_POWER_CONTROL_3, k_power_control_3, sizeof(k_power_control_3),
     0u},
    {ILI9488_CMD_VCOM_CONTROL_1, k_vcom_control_1, sizeof(k_vcom_control_1),
     0u},
    {ILI9488_CMD_FRAME_RATE_CONTROL, k_frame_rate_control,
     sizeof(k_frame_rate_control), 0u},
    {ILI9488_CMD_MEMORY_ACCESS_CTRL, k_initial_memory_access_control,
     sizeof(k_initial_memory_access_control), 0u},
    {ILI9488_CMD_POSITIVE_GAMMA, k_positive_gamma, sizeof(k_positive_gamma),
     0u},
    {ILI9488_CMD_NEGATIVE_GAMMA, k_negative_gamma, sizeof(k_negative_gamma),
     0u},
    {ILI9488_CMD_PIXEL_FORMAT_SET, k_pixel_format_rgb565,
     sizeof(k_pixel_format_rgb565), 0u},
    {ILI9488_CMD_SLEEP_OUT, NULL, 0u, ILI9488_SLEEP_OUT_DELAY_MS},
    {ILI9488_CMD_DISPLAY_ON, NULL, 0u, 0u},
};

static const esy_ili9488_panel_profile_t k_default_panel_profile = {
    k_init_sequence,
    (uint32_t)(sizeof(k_init_sequence) / sizeof(k_init_sequence[0])),
};

static const esy_ili9488_dir_cfg_t k_direction_table[] = {
    {ESY_LCD_DIR_L2R_U2D, 0x08u, 0x22u},
    {ESY_LCD_DIR_R2L_D2U, 0x08u, 0x42u},
    {ESY_LCD_DIR_U2D_R2L, 0x28u, 0x02u},
    {ESY_LCD_DIR_D2U_L2R, 0x28u, 0x62u},
};

/* -------------------------------------------------------------------------- */
/* GPIO/SPI helpers                                                           */
/* -------------------------------------------------------------------------- */

static bool is_valid_dir(esy_lcd_dir_t dir) {
  for (uint32_t i = 0u;
       i < (uint32_t)(sizeof(k_direction_table) / sizeof(k_direction_table[0]));
       ++i) {
    if (k_direction_table[i].dir == dir) {
      return true;
    }
  }

  return false;
}

static bool is_valid_color_format(esy_color_format_t color_format) {
  return color_format == ESY_COLOR_FORMAT_RGB565;
}

static bool is_valid_panel_profile(const esy_ili9488_panel_profile_t *profile) {
  if (!profile) return true;
  if (!profile->init_sequence || profile->init_sequence_count == 0u)
    return false;

  for (uint32_t i = 0u; i < profile->init_sequence_count; ++i) {
    const esy_ili9488_command_t *entry = &profile->init_sequence[i];
    if ((entry->data_len > 0u) && !entry->data) {
      return false;
    }
  }

  return true;
}

static esy_err_t validate_config(const esy_ili9488_config_t *config) {
  if (!config) return ESY_ERR_INVALID_ARG;
  if (!config->spi) return ESY_ERR_INVALID_ARG;
  if ((config->cs_gpio == NULL) || !esy_hal_gpio_is_initialized(config->cs_gpio) ||
      (config->dc_gpio == NULL) || !esy_hal_gpio_is_initialized(config->dc_gpio) ||
      (config->rst_gpio == NULL) || !esy_hal_gpio_is_initialized(config->rst_gpio)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((config->width == 0u) || (config->height == 0u))
    return ESY_ERR_INVALID_ARG;
  if (!is_valid_color_format(config->color_format))
    return ESY_ERR_NOT_SUPPORTED;
  if (!is_valid_dir(config->dir)) return ESY_ERR_INVALID_ARG;
  if (config->parameter_format != ESY_ILI9488_PARAMETER_U8 &&
      config->parameter_format != ESY_ILI9488_PARAMETER_U16_BE)
    return ESY_ERR_INVALID_ARG;
  if (!is_valid_panel_profile(config->panel_profile))
    return ESY_ERR_INVALID_ARG;
  if (!config->spi_submit_cb) return ESY_ERR_INVALID_ARG;
  return ESY_OK;
}

static esy_err_t begin_transfer(const esy_ili9488_ctx_t *ctx, bool data) {
  esy_err_t err = esy_hal_gpio_write(ctx->dc_gpio, data);
  if (err != ESY_OK) return err;
  return esy_hal_gpio_write(ctx->cs_gpio, false);
}

static esy_err_t end_transfer(const esy_ili9488_ctx_t *ctx) {
  return esy_hal_gpio_write(ctx->cs_gpio, true);
}

static const esy_ili9488_dir_cfg_t *find_direction_cfg(esy_lcd_dir_t dir) {
  for (uint32_t i = 0u;
       i < (uint32_t)(sizeof(k_direction_table) / sizeof(k_direction_table[0]));
       ++i) {
    if (k_direction_table[i].dir == dir) {
      return &k_direction_table[i];
    }
  }

  return NULL;
}

/* Command bytes use blocking I/O, including queued flush preparation. */
static esy_err_t write_transfer(esy_ili9488_ctx_t *ctx, bool data,
                             const uint8_t *bytes, uint32_t len) {
  esy_err_t err = begin_transfer(ctx, data);
  if (err == ESY_OK) {
    int32_t written = esy_hal_spi_write_blocking(ctx->spi, bytes, len);
    err = (written == (int32_t)len) ? ESY_OK
          : (written < 0 ? (esy_err_t)written : ESY_ERR_IO);
  }
  esy_err_t cleanup = end_transfer(ctx);
  if (cleanup != ESY_OK &&
      atomic_load_explicit(&ctx->state, memory_order_relaxed) !=
          ILI9488_STATE_FLUSHING) {
    /* Accepted preparation still owns ctx until complete_flush publishes it. */
    atomic_store_explicit(&ctx->state, ILI9488_STATE_CLEANUP_FAILED,
                          memory_order_release);
  }
  return err == ESY_OK ? cleanup : err;
}

static esy_err_t write_bytes(esy_ili9488_ctx_t *ctx, bool data,
                             const uint8_t *bytes, uint32_t len) {
  if (!data || ctx->parameter_format == ESY_ILI9488_PARAMETER_U8)
    return write_transfer(ctx, data, bytes, len);

  for (uint32_t i = 0u; i < len; ++i) {
    const uint8_t parameter[] = {0u, bytes[i]};
    esy_err_t err = write_transfer(ctx, true, parameter, sizeof(parameter));
    if (err != ESY_OK) return err;
  }
  return ESY_OK;
}

static esy_err_t send_cmd(esy_ili9488_ctx_t *ctx, uint8_t cmd) {
  return write_bytes(ctx, false, &cmd, 1u);
}

static esy_err_t send_data_byte(esy_ili9488_ctx_t *ctx, uint8_t data) {
  return write_bytes(ctx, true, &data, 1u);
}

static esy_err_t send_cmd_data(esy_ili9488_ctx_t *ctx,
                                const esy_ili9488_command_t *seq) {
  esy_err_t err = send_cmd(ctx, seq->cmd);
  if (err != ESY_OK) return err;

  for (uint8_t i = 0u; i < seq->data_len; ++i) {
    err = send_data_byte(ctx, seq->data[i]);
    if (err != ESY_OK) return err;
  }
  if (seq->delay_ms > 0u) {
    esy_hal_system_delay_ms(seq->delay_ms);
  }
  return ESY_OK;
}

static esy_err_t run_init_sequence(esy_ili9488_ctx_t *ctx) {
  const esy_ili9488_panel_profile_t *profile =
      ctx->panel_profile ? ctx->panel_profile : &k_default_panel_profile;

  for (uint32_t i = 0u; i < profile->init_sequence_count; ++i) {
    esy_err_t err = send_cmd_data(ctx, &profile->init_sequence[i]);
    if (err != ESY_OK) return err;
  }
  return ESY_OK;
}

/* The provider runs this entire prefix under exclusive bus ownership. */
static esy_err_t prepare_flush(void *arg) {
  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)arg;
  const uint8_t columns[] = {(uint8_t)(ctx->x0 >> 8), (uint8_t)ctx->x0,
                             (uint8_t)(ctx->x1 >> 8), (uint8_t)ctx->x1};
  const uint8_t rows[] = {(uint8_t)(ctx->y0 >> 8), (uint8_t)ctx->y0,
                          (uint8_t)(ctx->y1 >> 8), (uint8_t)ctx->y1};
  esy_err_t err = send_cmd(ctx, ILI9488_CMD_COLUMN_ADDRESS_SET);
  if (err != ESY_OK) return err;
  err = write_bytes(ctx, true, columns, sizeof(columns));
  if (err != ESY_OK) return err;
  err = send_cmd(ctx, ILI9488_CMD_ROW_ADDRESS_SET);
  if (err != ESY_OK) return err;
  err = write_bytes(ctx, true, rows, sizeof(rows));
  if (err != ESY_OK) return err;
  err = send_cmd(ctx, ILI9488_CMD_MEMORY_WRITE);
  if (err != ESY_OK) return err;
  return begin_transfer(ctx, true);
}

static void complete_flush(esy_err_t result, void *arg) {
  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)arg;
  void (*done_cb)(void *) = ctx->lvgl_flush_done_cb;
  void *done_arg = ctx->lvgl_flush_done_cb_arg;

  /* Release the buffer even on failure, but never publish IDLE with CS stuck.
   * The owner can query the latched fault from its completion callback. */
  (void)result;
  esy_err_t cleanup = end_transfer(ctx);
  ctx->lvgl_flush_done_cb = NULL;
  ctx->lvgl_flush_done_cb_arg = NULL;
  atomic_store_explicit(&ctx->state,
                        cleanup == ESY_OK ? ILI9488_STATE_IDLE
                                          : ILI9488_STATE_CLEANUP_FAILED,
                        memory_order_release);
  /* Do not access ctx after publication: done_cb may detach or reenter. */
  if (done_cb != NULL) done_cb(done_arg);
}

static esy_err_t esy_ili9488_flush(uint16_t x0, uint16_t y0, uint16_t x1,
                                   uint16_t y1, const uint8_t *color_buf,
                                   void (*done_cb)(void *), void *cb_arg,
                                   void *arg) {
  esy_lcd_t *lcd = (esy_lcd_t *)arg;
  if ((lcd == NULL) || (color_buf == NULL)) return ESY_ERR_INVALID_ARG;
  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)lcd->ctx;
  if (ctx == NULL) return ESY_ERR_STATE;
  const bool rotated = ctx->dir == ESY_LCD_DIR_U2D_R2L ||
                       ctx->dir == ESY_LCD_DIR_D2U_L2R;
  const uint16_t width = rotated ? ctx->height : ctx->width;
  const uint16_t height = rotated ? ctx->width : ctx->height;
  if ((x0 > x1) || (y0 > y1) || (x1 >= width) || (y1 >= height))
    return ESY_ERR_INVALID_ARG;

  uint32_t pixels = (uint32_t)(x1 - x0 + 1u) * (uint32_t)(y1 - y0 + 1u);
  if (pixels > UINT32_MAX / ILI9488_RGB565_BYTES_PER_PIXEL)
    return ESY_ERR_INVALID_ARG;
  int expected = ILI9488_STATE_IDLE;
  if (!atomic_compare_exchange_strong_explicit(
          &ctx->state, &expected, ILI9488_STATE_FLUSHING,
          memory_order_acquire, memory_order_relaxed)) {
    return expected == ILI9488_STATE_FLUSHING ? ESY_ERR_BUSY : ESY_ERR_STATE;
  }
  ctx->x0 = x0;
  ctx->y0 = y0;
  ctx->x1 = x1;
  ctx->y1 = y1;
  ctx->lvgl_flush_done_cb = done_cb;
  ctx->lvgl_flush_done_cb_arg = cb_arg;
  esy_err_t err = ctx->spi_submit_cb(
      color_buf, pixels * ILI9488_RGB565_BYTES_PER_PIXEL, prepare_flush,
      complete_flush, ctx, ctx->spi_submit_cb_arg);
  if (err != ESY_OK) {
    /* Rejection guarantees neither callback ran and nothing was retained. */
    ctx->lvgl_flush_done_cb = NULL;
    ctx->lvgl_flush_done_cb_arg = NULL;
    atomic_store_explicit(&ctx->state, ILI9488_STATE_IDLE, memory_order_release);
  }
  return err;
}

static esy_err_t esy_ili9488_deinit_adapter(void *arg) {
  return esy_ili9488_deinit((esy_lcd_t *)arg);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void esy_ili9488_default_config(esy_ili9488_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->cs_gpio = NULL;
  config->dc_gpio = NULL;
  config->rst_gpio = NULL;
  config->color_format = ESY_COLOR_FORMAT_RGB565;
}

static esy_err_t apply_direction(esy_ili9488_ctx_t *ctx, esy_lcd_dir_t dir) {
  const esy_ili9488_dir_cfg_t *dir_cfg = find_direction_cfg(dir);
  if (dir_cfg == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t err = send_cmd(ctx, ILI9488_CMD_DISPLAY_FUNCTION);
  if (err != ESY_OK) return err;
  err = send_data_byte(ctx, 0x00u);
  if (err != ESY_OK) return err;
  err = send_data_byte(ctx, dir_cfg->disfun);
  if (err != ESY_OK) return err;
  err = send_cmd(ctx, ILI9488_CMD_MEMORY_ACCESS_CTRL);
  if (err != ESY_OK) return err;
  uint8_t madctl = dir_cfg->madctl & (uint8_t)~ILI9488_MADCTL_BGR_BIT;
  if (ctx->bgr) madctl |= ILI9488_MADCTL_BGR_BIT;
  err = send_data_byte(ctx, madctl);
  if (err == ESY_OK) ctx->dir = dir;
  return err;
}

esy_err_t esy_ili9488_set_direction(esy_lcd_t *lcd, esy_lcd_dir_t dir) {
  if ((lcd == NULL) || !is_valid_dir(dir)) return ESY_ERR_INVALID_ARG;
  if (lcd->ctx == NULL) return ESY_ERR_STATE;
  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)lcd->ctx;
  int state = atomic_load_explicit(&ctx->state, memory_order_acquire);
  if (state != ILI9488_STATE_IDLE)
    return state == ILI9488_STATE_FLUSHING ? ESY_ERR_BUSY : ESY_ERR_STATE;
  esy_err_t err = lcd->device == NULL ? ESY_OK : esy_device_get(lcd->device);
  if (err != ESY_OK) return err;
  err = apply_direction(ctx, dir);
  if (lcd->device != NULL) (void)esy_device_put(lcd->device);
  return err;
}

esy_err_t esy_ili9488_init(esy_lcd_t *lcd, const esy_ili9488_config_t *config) {
  if (!lcd) return ESY_ERR_INVALID_ARG;
  esy_err_t err = validate_config(config);
  if (err != ESY_OK) return err;

  if ((lcd->ctx != NULL) || (lcd->flush != NULL) || (lcd->deinit != NULL) ||
      (lcd->device != NULL)) return ESY_ERR_STATE;
  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)(void *)lcd->priv.bytes;
  memset(ctx, 0, sizeof(*ctx));
  atomic_init(&ctx->state, ILI9488_STATE_DETACHED);
  ctx->magic = ILI9488_CTX_MAGIC;

  ctx->spi = config->spi;
  ctx->cs_gpio = config->cs_gpio;
  ctx->dc_gpio = config->dc_gpio;
  ctx->rst_gpio = config->rst_gpio;
  ctx->bgr = config->bgr;
  ctx->parameter_format = config->parameter_format;
  ctx->width = config->width;
  ctx->height = config->height;
  ctx->color_format = config->color_format;
  ctx->dir = config->dir;
  ctx->panel_profile = config->panel_profile;
  ctx->backlight_set_cb = config->backlight_set_cb;
  ctx->backlight_set_cb_arg = config->backlight_set_cb_arg;
  ctx->backlight_enabled = config->backlight_initially_enabled;

  ctx->spi_submit_cb = config->spi_submit_cb;
  ctx->spi_submit_cb_arg = config->spi_submit_cb_arg;

  err = esy_hal_gpio_write(ctx->rst_gpio, false);
  if (err != ESY_OK) return err;
  esy_hal_system_delay_ms(ILI9488_RESET_LOW_DELAY_MS);
  err = esy_hal_gpio_write(ctx->rst_gpio, true);
  if (err != ESY_OK) return err;
  esy_hal_system_delay_ms(ILI9488_RESET_HIGH_DELAY_MS);
  err = run_init_sequence(ctx);
  if (err == ESY_OK) err = apply_direction(ctx, config->dir);
  if (err != ESY_OK &&
      atomic_load_explicit(&ctx->state, memory_order_acquire) !=
          ILI9488_STATE_CLEANUP_FAILED) return err;

  if (err == ESY_OK)
    atomic_store_explicit(&ctx->state, ILI9488_STATE_IDLE, memory_order_release);
  lcd->ctx = ctx;
  lcd->flush = esy_ili9488_flush;
  lcd->deinit = esy_ili9488_deinit_adapter;
  return err;
}

esy_err_t esy_ili9488_deinit(esy_lcd_t *lcd) {
  if (lcd == NULL) return ESY_OK;
  if (lcd->device != NULL) return ESY_ERR_BUSY;
  if (lcd->ctx == NULL) return ESY_OK;
  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)lcd->ctx;
  if (ctx->magic != ILI9488_CTX_MAGIC) return ESY_ERR_STATE;
  int expected = atomic_load_explicit(&ctx->state, memory_order_acquire);
  if (expected == ILI9488_STATE_CLEANUP_FAILED) {
    /* The owner must exclusively hold the bus during fault recovery. */
    esy_err_t cleanup = end_transfer(ctx);
    if (cleanup != ESY_OK) return cleanup;
  } else if (expected != ILI9488_STATE_IDLE) {
    return ESY_ERR_BUSY;
  }
  if (!atomic_compare_exchange_strong_explicit(
          &ctx->state, &expected, ILI9488_STATE_DETACHED,
          memory_order_acquire, memory_order_relaxed)) return ESY_ERR_BUSY;
  memset(lcd, 0, sizeof(*lcd));
  return ESY_OK;
}

static esy_err_t get_idle_ctx(esy_lcd_t *lcd, esy_ili9488_ctx_t **out_ctx) {
  if (!lcd || !out_ctx) return ESY_ERR_INVALID_ARG;
  if (!lcd->ctx) return ESY_ERR_STATE;

  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)lcd->ctx;
  if ((ctx->magic != ILI9488_CTX_MAGIC) ||
      (atomic_load_explicit(&ctx->state, memory_order_acquire) == ILI9488_STATE_DETACHED)) {
    return ESY_ERR_STATE;
  }
  int state = atomic_load_explicit(&ctx->state, memory_order_acquire);
  if (state != ILI9488_STATE_IDLE)
    return state == ILI9488_STATE_FLUSHING ? ESY_ERR_BUSY : ESY_ERR_STATE;

  *out_ctx = ctx;
  return ESY_OK;
}

esy_err_t esy_ili9488_get_status(esy_lcd_t *lcd) {
  esy_ili9488_ctx_t *ctx = NULL;
  return get_idle_ctx(lcd, &ctx);
}

static esy_err_t ili9488_acquire(esy_lcd_t *lcd) {
  return (lcd->device == NULL) ? ESY_OK : esy_device_get(lcd->device);
}

static void ili9488_release(esy_lcd_t *lcd) {
  if (lcd->device != NULL) {
    (void)esy_device_put(lcd->device);
  }
}

static esy_err_t ili9488_send_public_command(esy_lcd_t *lcd, uint8_t command) {
  esy_ili9488_ctx_t *ctx = NULL;
  esy_err_t err = get_idle_ctx(lcd, &ctx);

  if (err != ESY_OK) return err;
  err = ili9488_acquire(lcd);
  if (err != ESY_OK) return err;
  err = send_cmd(ctx, command);
  ili9488_release(lcd);
  return err;
}

esy_err_t esy_ili9488_display_on(esy_lcd_t *lcd) {
  return ili9488_send_public_command(lcd, ILI9488_CMD_DISPLAY_ON);
}

esy_err_t esy_ili9488_display_off(esy_lcd_t *lcd) {
  return ili9488_send_public_command(lcd, ILI9488_CMD_DISPLAY_OFF);
}

esy_err_t esy_ili9488_sleep_in(esy_lcd_t *lcd) {
  esy_ili9488_ctx_t *ctx = NULL;
  esy_err_t err = get_idle_ctx(lcd, &ctx);
  if (err != ESY_OK) return err;
  err = ili9488_acquire(lcd);
  if (err != ESY_OK) return err;
  err = send_cmd(ctx, ILI9488_CMD_SLEEP_IN);
  /* This also satisfies the shorter next-command delay and allows an
   * immediate sleep_out call once this function returns. */
  if (err == ESY_OK) esy_hal_system_delay_ms(ILI9488_SLEEP_IN_DELAY_MS);
  ili9488_release(lcd);
  return err;
}

esy_err_t esy_ili9488_sleep_out(esy_lcd_t *lcd) {
  esy_ili9488_ctx_t *ctx = NULL;
  esy_err_t err = get_idle_ctx(lcd, &ctx);
  if (err != ESY_OK) return err;

  err = ili9488_acquire(lcd);
  if (err != ESY_OK) return err;
  err = send_cmd(ctx, ILI9488_CMD_SLEEP_OUT);
  if (err != ESY_OK) {
    ili9488_release(lcd);
    return err;
  }

  esy_hal_system_delay_ms(ILI9488_SLEEP_OUT_DELAY_MS);
  ili9488_release(lcd);
  return ESY_OK;
}

esy_err_t esy_ili9488_set_backlight(esy_lcd_t *lcd, bool enabled) {
  esy_err_t err;

  if (!lcd || !lcd->ctx) return ESY_ERR_INVALID_ARG;

  esy_ili9488_ctx_t *ctx = (esy_ili9488_ctx_t *)lcd->ctx;
  if ((ctx->magic != ILI9488_CTX_MAGIC) ||
      (atomic_load_explicit(&ctx->state, memory_order_acquire) == ILI9488_STATE_DETACHED)) {
    return ESY_ERR_STATE;
  }
  if (!ctx->backlight_set_cb) return ESY_ERR_NOT_SUPPORTED;

  err = ili9488_acquire(lcd);
  if (err != ESY_OK) return err;
  err = ctx->backlight_set_cb(enabled, ctx->backlight_set_cb_arg);
  if (err == ESY_OK) ctx->backlight_enabled = enabled;
  ili9488_release(lcd);
  return err;
}
