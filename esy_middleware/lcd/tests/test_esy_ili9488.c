/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/lcd/esy_ili9488.h"
#include "esy_middleware/lcd/esy_ili9488_device.h"

#include <string.h>

#include "esy_middleware/lcd/esy_lcd.h"
#include "fake_esy_hal_gpio.h"
#include "fake_esy_hal_spi.h"
#include "fake_esy_hal_system.h"
#include "esy_hal/esy_hal_gpio_pm.h"
#include "unity.h"
#include "esy_middleware/io_scheduler/esy_spi_scheduler.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"

#define TEST_CS_PIN               10u
#define TEST_DC_PIN               11u
#define TEST_RST_PIN              12u
#define TEST_WIDTH                320u
#define TEST_HEIGHT               480u
#define TEST_TRANSFER_CAPACITY    40u

typedef struct {
  const uint8_t *tx_ptr[TEST_TRANSFER_CAPACITY];
  uint32_t len[TEST_TRANSFER_CAPACITY];
  esy_spi_prepare_cb_t prepare[TEST_TRANSFER_CAPACITY];
  esy_spi_prepared_done_cb_t done[TEST_TRANSFER_CAPACITY];
  void *callback_arg[TEST_TRANSFER_CAPACITY];
  uint32_t submit_count;
  uint32_t payload_count;
  esy_err_t submit_result;
  esy_err_t transfer_result;
  esy_err_t last_result;
  bool defer_callbacks;
} test_spi_t;

typedef struct {
  esy_lcd_t *lcd;
  const uint8_t *color_buf;
  uint32_t done_count;
  bool reenter_flush;
} test_done_t;

typedef struct {
  uint32_t call_count;
  bool last_enabled;
  esy_err_t result;
} test_backlight_t;

static test_spi_t s_spi_async;
static test_backlight_t s_backlight;
static esy_spi_t s_spi;
static esy_gpio_t s_cs_gpio;
static esy_gpio_t s_dc_gpio;
static esy_gpio_t s_rst_gpio;
static uint8_t s_color_buf[TEST_WIDTH * 2u];

static void init_gpio(esy_gpio_t *gpio, uint8_t pin, bool value) {
  esy_gpio_config_t config;

  if (esy_hal_gpio_is_initialized(gpio))
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(gpio));
  esy_hal_gpio_default_cfg(&config);
  config.pin = pin;
  config.mode = ESY_GPIO_MODE_OUTPUT;
  config.output_value = value;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(gpio, &config));
}

static void complete_transfer(uint32_t index) {
  TEST_ASSERT_TRUE(index < s_spi_async.submit_count);
  esy_err_t result = s_spi_async.prepare[index](s_spi_async.callback_arg[index]);
  if (result == ESY_OK) {
    s_spi_async.payload_count++;
    result = s_spi_async.transfer_result;
  }
  s_spi_async.last_result = result;
  s_spi_async.done[index](result, s_spi_async.callback_arg[index]);
}

static esy_err_t test_lcd_spi_submit(
    const uint8_t *tx_buf, uint32_t len, esy_spi_prepare_cb_t prepare,
    esy_spi_prepared_done_cb_t done, void *callback_arg, void *arg) {
  test_spi_t *spi = (test_spi_t *)arg;
  const uint32_t index = spi->submit_count;
  if (spi->submit_result != ESY_OK) return spi->submit_result;
  TEST_ASSERT_TRUE(index < TEST_TRANSFER_CAPACITY);
  spi->tx_ptr[index] = tx_buf;
  spi->len[index] = len;
  spi->prepare[index] = prepare;
  spi->done[index] = done;
  spi->callback_arg[index] = callback_arg;
  spi->submit_count++;
  if (!spi->defer_callbacks) complete_transfer(index);
  return ESY_OK;
}

static void assert_tx_byte_at(uint32_t index, uint8_t expected) {
  uint8_t tx = 0u;

  TEST_ASSERT_EQUAL_size_t(
      1u, fake_esy_hal_spi_copy_tx_at(&s_spi, index, &tx, 1u));
  TEST_ASSERT_EQUAL_UINT8(expected, tx);
}

static void assert_direction_writes(uint32_t start_index, uint8_t disfun,
                                    uint8_t madctl) {
  assert_tx_byte_at(start_index, 0xB6u);
  assert_tx_byte_at(start_index + 1u, 0x00u);
  assert_tx_byte_at(start_index + 2u, disfun);
  assert_tx_byte_at(start_index + 3u, 0x36u);
  assert_tx_byte_at(start_index + 4u, madctl);
}

static void done_cb(void *arg) {
  test_done_t *done = (test_done_t *)arg;

  done->done_count++;
  if (done->reenter_flush && done->done_count == 1u) {
    TEST_ASSERT_EQUAL_INT32(
        ESY_OK, esy_lcd_flush(done->lcd, 0u, 0u, 1u, 0u, done->color_buf,
                              done_cb, done));
  }
}

static esy_err_t test_backlight_set(bool enabled, void *arg) {
  test_backlight_t *backlight = (test_backlight_t *)arg;

  backlight->call_count++;
  if (backlight->result == ESY_OK) backlight->last_enabled = enabled;
  return backlight->result;
}

static void fill_default_config(esy_ili9488_config_t *cfg) {
  esy_ili9488_default_config(cfg);
  cfg->spi = &s_spi;
  init_gpio(&s_cs_gpio, TEST_CS_PIN, true);
  init_gpio(&s_dc_gpio, TEST_DC_PIN, true);
  init_gpio(&s_rst_gpio, TEST_RST_PIN, true);
  cfg->cs_gpio = &s_cs_gpio;
  cfg->dc_gpio = &s_dc_gpio;
  cfg->rst_gpio = &s_rst_gpio;
  cfg->width = TEST_WIDTH;
  cfg->height = TEST_HEIGHT;
  cfg->color_format = ESY_COLOR_FORMAT_RGB565;
  cfg->dir = ESY_LCD_DIR_L2R_U2D;
  cfg->spi_submit_cb = test_lcd_spi_submit;
  cfg->spi_submit_cb_arg = &s_spi_async;
}

static void init_lcd(esy_lcd_t *lcd) {
  esy_ili9488_config_t cfg;

  memset(lcd, 0, sizeof(*lcd));
  fill_default_config(&cfg);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_init(lcd, &cfg));
}

void setUp(void) {
  memset(&s_spi_async, 0, sizeof(s_spi_async));
  memset(&s_backlight, 0, sizeof(s_backlight));
  memset(&s_spi, 0, sizeof(s_spi));
  memset(&s_cs_gpio, 0, sizeof(s_cs_gpio));
  memset(&s_dc_gpio, 0, sizeof(s_dc_gpio));
  memset(&s_rst_gpio, 0, sizeof(s_rst_gpio));
  memset(s_color_buf, 0x5A, sizeof(s_color_buf));
  s_spi_async.submit_result = ESY_OK;
  s_spi_async.transfer_result = ESY_OK;
  s_backlight.result = ESY_OK;
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_hal_gpio_reset();
  fake_esy_hal_spi_reset();
  fake_esy_hal_system_reset();
}

void tearDown(void) {}

void test_default_config_uses_safe_defaults(void) {
  esy_ili9488_config_t cfg;

  memset(&cfg, 0xA5, sizeof(cfg));
  esy_ili9488_default_config(&cfg);

  TEST_ASSERT_NULL(cfg.cs_gpio);
  TEST_ASSERT_NULL(cfg.dc_gpio);
  TEST_ASSERT_NULL(cfg.rst_gpio);
  TEST_ASSERT_EQUAL_INT(ESY_COLOR_FORMAT_RGB565, cfg.color_format);
  TEST_ASSERT_NULL(cfg.spi);
  TEST_ASSERT_NULL(cfg.panel_profile);
  TEST_ASSERT_NULL(cfg.backlight_set_cb);
  TEST_ASSERT_NULL(cfg.spi_submit_cb);
}

void test_init_rejects_invalid_config(void) {
  esy_lcd_t lcd;
  esy_ili9488_config_t cfg;

  memset(&lcd, 0, sizeof(lcd));
  fill_default_config(&cfg);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_init(NULL, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_init(&lcd, NULL));

  fill_default_config(&cfg);
  cfg.spi = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_init(&lcd, &cfg));

  fill_default_config(&cfg);
  cfg.cs_gpio = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_init(&lcd, &cfg));

  fill_default_config(&cfg);
  cfg.width = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_init(&lcd, &cfg));

  fill_default_config(&cfg);
  cfg.dir = (esy_lcd_dir_t)99;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_init(&lcd, &cfg));

  fill_default_config(&cfg);
  cfg.color_format = ESY_COLOR_FORMAT_RGB666;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED, esy_ili9488_init(&lcd, &cfg));

  fill_default_config(&cfg);
  cfg.spi_submit_cb = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_init(&lcd, &cfg));

  static const esy_ili9488_panel_profile_t k_invalid_profile = {NULL, 1u};
  fill_default_config(&cfg);
  cfg.panel_profile = &k_invalid_profile;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_init(&lcd, &cfg));
}

void test_init_runs_reset_and_sync_command_sequence(void) {
  esy_lcd_t lcd;

  init_lcd(&lcd);

  TEST_ASSERT_NOT_NULL(lcd.ctx);
  TEST_ASSERT_NOT_NULL(lcd.flush);
  TEST_ASSERT_NOT_NULL(lcd.deinit);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_has_written(TEST_RST_PIN));
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_RST_PIN));
  TEST_ASSERT_EQUAL_UINT32(290u, fake_esy_hal_system_get_ctx()->tick_ms);
  TEST_ASSERT_EQUAL_size_t(52u, fake_esy_hal_spi_get_tx_count(&s_spi));
  assert_tx_byte_at(0u, 0x21u);
  assert_tx_byte_at(1u, 0xC2u);
  assert_tx_byte_at(2u, 0x33u);
  assert_tx_byte_at(3u, 0xC5u);
  assert_tx_byte_at(6u, 0x80u);
  assert_tx_byte_at(7u, 0xB1u);
  assert_tx_byte_at(9u, 0x36u);
  assert_tx_byte_at(11u, 0xE0u);
  assert_tx_byte_at(27u, 0xE1u);
  assert_tx_byte_at(43u, 0x3Au);
  assert_tx_byte_at(44u, 0x55u);
  assert_tx_byte_at(45u, 0x11u);
  assert_tx_byte_at(46u, 0x29u);
  assert_direction_writes(47u, 0x22u, 0x00u);
}

void test_init_uses_custom_panel_profile(void) {
  static const uint8_t k_custom_payload[] = {0x12u, 0x34u};
  static const esy_ili9488_command_t k_custom_init[] = {
      {0xA5u, k_custom_payload, sizeof(k_custom_payload), 7u},
      {0x5Au, NULL, 0u, 0u},
  };
  static const esy_ili9488_panel_profile_t k_custom_profile = {
      k_custom_init,
      (uint32_t)(sizeof(k_custom_init) / sizeof(k_custom_init[0])),
  };
  esy_lcd_t lcd;
  esy_ili9488_config_t cfg;

  memset(&lcd, 0, sizeof(lcd));
  fill_default_config(&cfg);
  cfg.panel_profile = &k_custom_profile;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_init(&lcd, &cfg));

  TEST_ASSERT_EQUAL_UINT32(177u, fake_esy_hal_system_get_ctx()->tick_ms);
  TEST_ASSERT_EQUAL_size_t(9u, fake_esy_hal_spi_get_tx_count(&s_spi));
  assert_tx_byte_at(0u, 0xA5u);
  assert_tx_byte_at(1u, 0x12u);
  assert_tx_byte_at(2u, 0x34u);
  assert_tx_byte_at(3u, 0x5Au);
  assert_direction_writes(4u, 0x22u, 0x00u);
}

void test_set_direction_writes_expected_register_values(void) {
  esy_lcd_t lcd;
  size_t base = 0u;

  init_lcd(&lcd);
  base = fake_esy_hal_spi_get_tx_count(&s_spi);

  esy_ili9488_set_direction(&lcd, ESY_LCD_DIR_R2L_D2U);
  assert_direction_writes((uint32_t)base, 0x42u, 0x00u);

  base = fake_esy_hal_spi_get_tx_count(&s_spi);
  esy_ili9488_set_direction(&lcd, ESY_LCD_DIR_U2D_R2L);
  assert_direction_writes((uint32_t)base, 0x02u, 0x20u);

  base = fake_esy_hal_spi_get_tx_count(&s_spi);
  esy_ili9488_set_direction(&lcd, ESY_LCD_DIR_D2U_L2R);
  assert_direction_writes((uint32_t)base, 0x62u, 0x20u);

  base = fake_esy_hal_spi_get_tx_count(&s_spi);
  esy_ili9488_set_direction(&lcd, ESY_LCD_DIR_L2R_U2D);
  assert_direction_writes((uint32_t)base, 0x22u, 0x00u);
}

void test_flush_defers_done_until_pixel_transfer_completes(void) {
  esy_lcd_t lcd;
  test_done_t done = {0};

  init_lcd(&lcd);
  s_spi_async.defer_callbacks = true;
  done.lcd = &lcd;
  done.color_buf = s_color_buf;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));

  TEST_ASSERT_EQUAL_UINT32(1u, s_spi_async.submit_count);
  TEST_ASSERT_EQUAL_UINT32(4u, s_spi_async.len[0]);
  TEST_ASSERT_EQUAL_PTR(s_color_buf, s_spi_async.tx_ptr[0]);
  TEST_ASSERT_EQUAL_UINT32(0u, done.done_count);
  TEST_ASSERT_EQUAL_size_t(52u, fake_esy_hal_spi_get_tx_count(&s_spi));
  complete_transfer(0u);
  TEST_ASSERT_EQUAL_UINT32(1u, done.done_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_spi_async.payload_count);

}

void test_flush_busy_request_completes_immediately_without_overwriting_callback(
    void) {
  esy_lcd_t lcd;
  test_done_t first = {0};
  test_done_t second = {0};

  init_lcd(&lcd);
  s_spi_async.defer_callbacks = true;
  first.lcd = &lcd;
  second.lcd = &lcd;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u,
                                                s_color_buf, done_cb, &first));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_BUSY,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &second));

  TEST_ASSERT_EQUAL_UINT32(0u, second.done_count);
  TEST_ASSERT_EQUAL_UINT32(0u, first.done_count);
  complete_transfer(0u);
  TEST_ASSERT_EQUAL_UINT32(1u, first.done_count);
}

void test_flush_done_callback_may_reenter_flush(void) {
  esy_lcd_t lcd;
  test_done_t done = {0};

  init_lcd(&lcd);
  done.lcd = &lcd;
  done.color_buf = s_color_buf;
  done.reenter_flush = true;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));

  TEST_ASSERT_EQUAL_UINT32(2u, done.done_count);
  TEST_ASSERT_EQUAL_UINT32(2u, s_spi_async.submit_count);
}

void test_deinit_preserves_queued_flush_until_completion(void) {
  esy_lcd_t lcd;
  test_done_t done = {0};
  init_lcd(&lcd);
  s_spi_async.defer_callbacks = true;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_lcd_detach(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_lcd_detach(&lcd));
  TEST_ASSERT_TRUE(esy_lcd_is_attached(&lcd));
  complete_transfer(0u);
  TEST_ASSERT_EQUAL_UINT32(1u, done.done_count);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_NULL(lcd.ctx);
  TEST_ASSERT_NULL(lcd.flush);
}

void test_flush_rejects_invalid_region_and_null_buffer(void) {
  esy_lcd_t lcd;
  test_done_t done = {0};

  init_lcd(&lcd);

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_lcd_flush(&lcd, 2u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_lcd_flush(&lcd, 0u, 0u, TEST_WIDTH, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, NULL, done_cb, &done));

  TEST_ASSERT_EQUAL_UINT32(0u, done.done_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_spi_async.submit_count);
}

void test_submit_rejection_keeps_transport_untouched_and_allows_retry(void) {
  esy_lcd_t lcd;
  test_done_t done = {0};
  init_lcd(&lcd);
  s_spi_async.defer_callbacks = true;
  s_spi_async.submit_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_UINT32(0u, done.done_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_spi_async.submit_count);
  TEST_ASSERT_EQUAL_size_t(52u, fake_esy_hal_spi_get_tx_count(&s_spi));
  s_spi_async.submit_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  complete_transfer(0u);
  TEST_ASSERT_EQUAL_UINT32(1u, done.done_count);
}

void test_display_sleep_controls_send_expected_commands(void) {
  esy_lcd_t lcd;
  size_t base = 0u;

  init_lcd(&lcd);

  base = fake_esy_hal_spi_get_tx_count(&s_spi);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_display_off(&lcd));
  assert_tx_byte_at((uint32_t)base, 0x28u);

  base = fake_esy_hal_spi_get_tx_count(&s_spi);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_sleep_in(&lcd));
  assert_tx_byte_at((uint32_t)base, 0x10u);

  base = fake_esy_hal_spi_get_tx_count(&s_spi);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_sleep_out(&lcd));
  assert_tx_byte_at((uint32_t)base, 0x11u);
  TEST_ASSERT_EQUAL_UINT32(530u, fake_esy_hal_system_get_ctx()->tick_ms);

  base = fake_esy_hal_spi_get_tx_count(&s_spi);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_display_on(&lcd));
  assert_tx_byte_at((uint32_t)base, 0x29u);
}

void test_controls_reject_busy_or_detached_handle(void) {
  esy_lcd_t lcd;
  test_done_t done = {0};

  init_lcd(&lcd);
  s_spi_async.defer_callbacks = true;
  done.lcd = &lcd;

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_ili9488_display_off(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_ili9488_sleep_in(&lcd));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_lcd_detach(&lcd));
  complete_transfer(0u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_ili9488_display_on(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_ili9488_display_on(NULL));
}

void test_backlight_callback_reports_state_and_errors(void) {
  esy_lcd_t lcd;
  esy_ili9488_config_t cfg;

  memset(&lcd, 0, sizeof(lcd));
  fill_default_config(&cfg);
  cfg.backlight_set_cb = test_backlight_set;
  cfg.backlight_set_cb_arg = &s_backlight;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_init(&lcd, &cfg));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_set_backlight(&lcd, true));
  TEST_ASSERT_EQUAL_UINT32(1u, s_backlight.call_count);
  TEST_ASSERT_TRUE(s_backlight.last_enabled);

  s_backlight.result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_ili9488_set_backlight(&lcd, false));
  TEST_ASSERT_EQUAL_UINT32(2u, s_backlight.call_count);
  TEST_ASSERT_TRUE(s_backlight.last_enabled);
}

void test_backlight_reports_not_supported_when_callback_missing(void) {
  esy_lcd_t lcd;

  init_lcd(&lcd);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_ili9488_set_backlight(&lcd, true));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_ili9488_set_backlight(NULL, true));
}

void test_ili9488_device_transitions_owned_state(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_device_t spi_device = {0};
  esy_device_t *dependencies[] = {&spi_device};
  esy_lcd_device_config_t device_config = {
      .registration = {
          .name = "ili9488",
          .dependencies = dependencies,
          .dependency_count = 1u,
          .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
      },
  };
  esy_lcd_device_binding_t binding = {0};
  esy_ili9488_config_t config;
  esy_lcd_t lcd = {0};
  const esy_gpio_state_t cs_sleep_state = {
      .mode = ESY_GPIO_MODE_INPUT_PULLDOWN,
  };
  const esy_gpio_pm_pin_t pins[] = {
      {.gpio = &s_cs_gpio, .sleep_state = &cs_sleep_state},
  };
  const esy_device_gpio_pm_profile_t gpio_pm_profile = {
      .pins = pins,
      .pin_count = sizeof(pins) / sizeof(pins[0]),
  };

  device_config.gpio_pm_profile = &gpio_pm_profile;
  fill_default_config(&config);
  config.backlight_initially_enabled = true;
  config.backlight_set_cb = test_backlight_set;
  config.backlight_set_cb_arg = &s_backlight;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_init(&lcd, &config));
  fake_esy_hal_spi_set_device(&s_spi, &spi_device);
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_ili9488_register_device(&binding, &lcd, &registry, &device_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, binding.device.ops->pm_action(&binding.device,
                                            ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND,
                                            ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_FALSE(s_backlight.last_enabled);
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_INPUT_PULLDOWN,
                        fake_esy_hal_gpio_get_current_state(&s_cs_gpio).mode);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, binding.device.ops->pm_action(&binding.device,
                                            ESY_DEVICE_PM_ACTION_SYSTEM_RESUME,
                                            ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_TRUE(s_backlight.last_enabled);
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_OUTPUT, fake_esy_hal_gpio_get_current_state(&s_cs_gpio).mode);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_current_state(&s_cs_gpio).output_value);
}

void test_flush_prepares_inclusive_window_as_one_transaction(void) {
  esy_lcd_t lcd;
  uint8_t actual[4];
  const uint8_t columns[] = {0x01u, 0x02u, 0x01u, 0x03u};
  const uint8_t rows[] = {0x01u, 0x04u, 0x01u, 0x05u};
  init_lcd(&lcd);
  size_t base = fake_esy_hal_spi_get_tx_count(&s_spi);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_lcd_flush(&lcd, 258u, 260u, 259u, 261u, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_UINT32(1u, s_spi_async.submit_count);
  TEST_ASSERT_EQUAL_UINT32(8u, s_spi_async.len[0]);
  assert_tx_byte_at(base, 0x2Au);
  TEST_ASSERT_EQUAL_size_t(4u, fake_esy_hal_spi_copy_tx_at(&s_spi, base + 1u, actual, sizeof(actual)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(columns, actual, 4u);
  assert_tx_byte_at(base + 2u, 0x2Bu);
  TEST_ASSERT_EQUAL_size_t(4u, fake_esy_hal_spi_copy_tx_at(&s_spi, base + 3u, actual, sizeof(actual)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rows, actual, 4u);
  assert_tx_byte_at(base + 4u, 0x2Cu);
}

void test_each_window_write_failure_skips_pixels_and_releases_callback(void) {
  esy_lcd_t lcd;
  test_done_t done = {0};
  init_lcd(&lcd);
  for (size_t i = 0u; i < 5u; ++i) {
    fake_esy_hal_spi_fail_write_after(i, ESY_ERR_IO);
    TEST_ASSERT_EQUAL_INT32(ESY_OK,
        esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, s_spi_async.last_result);
    TEST_ASSERT_EQUAL_UINT32(0u, s_spi_async.payload_count);
    TEST_ASSERT_EQUAL_UINT32(i + 1u, done.done_count);
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  }
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_UINT32(1u, s_spi_async.payload_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_lcd_detach(&lcd));
}

void test_gpio_or_pixel_failure_completes_and_releases_cs(void) {
  esy_lcd_t lcd;
  test_done_t done = {0};
  init_lcd(&lcd);
  fake_esy_hal_gpio_fail_write_after(0u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, s_spi_async.last_result);
  TEST_ASSERT_EQUAL_UINT32(0u, s_spi_async.payload_count);
  s_spi_async.transfer_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_UINT32(2u, done.done_count);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
}

void test_init_failure_repeated_init_and_reinit_follow_lifecycle(void) {
  esy_lcd_t lcd = {0};
  esy_ili9488_config_t cfg;
  fill_default_config(&cfg);
  fake_esy_hal_spi_fail_write_after(47u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_ili9488_init(&lcd, &cfg));
  TEST_ASSERT_FALSE(esy_lcd_is_attached(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_init(&lcd, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_ili9488_init(&lcd, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_init(&lcd, &cfg));
}

void test_reinit_cannot_overwrite_an_accepted_flush(void) {
  esy_lcd_t lcd = {0};
  esy_ili9488_config_t cfg;
  test_done_t done = {0};
  fill_default_config(&cfg);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_init(&lcd, &cfg));
  s_spi_async.defer_callbacks = true;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_ili9488_init(&lcd, &cfg));
  complete_transfer(0u);
  TEST_ASSERT_EQUAL_UINT32(1u, done.done_count);
}


extern void esy_spi_scheduler_test_iteration(esy_spi_scheduler_t *gw);

void test_real_gateway_owns_one_flush_until_task_completion(void) {
  ESY_SPI_SCHEDULER_STATIC_STORAGE(resources, 2u, 1u, 1u, 256u);
  esy_spi_scheduler_storage_t storage = {0};
  esy_spi_scheduler_config_t gateway_cfg;
  esy_spi_scheduler_t *gateway = NULL;
  esy_spi_scheduler_default_cfg(&gateway_cfg);
  gateway_cfg.block_count = 2u;
  gateway_cfg.task_stack_size = 256u;
  gateway_cfg.static_resources.request_storage = resources.request_storage;
  gateway_cfg.static_resources.request_storage_count = 2u;
  gateway_cfg.static_resources.task_stack_buffer = resources.task_stack_buffer;
  gateway_cfg.static_resources.task_stack_buffer_size = sizeof(resources.task_stack_buffer);
  gateway_cfg.static_resources.task_control_storage = &resources.task_control_storage;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_spi_scheduler_create(&s_spi, &gateway_cfg, &storage, &gateway));

  esy_ili9488_config_t cfg;
  esy_lcd_t lcd = {0};
  fill_default_config(&cfg);
  cfg.spi_submit_cb = esy_spi_scheduler_submit_prepared_with_arg;
  cfg.spi_submit_cb_arg = gateway;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_init(&lcd, &cfg));
  test_done_t done = {.lcd = &lcd};
  fake_esy_hal_spi_set_transfer_dma_async(true);

  for (unsigned cycle = 0; cycle < 3u; ++cycle) {
    TEST_ASSERT_EQUAL_INT(ESY_OK,
        esy_lcd_flush(&lcd, 0, 0, 3, 0, s_color_buf, done_cb, &done));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_lcd_detach(&lcd));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_spi_scheduler_quiesce(gateway));
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gateway));
    TEST_ASSERT_EQUAL_UINT(cycle, done.done_count);
    esy_spi_scheduler_test_iteration(gateway);
    TEST_ASSERT_FALSE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
    fake_esy_hal_spi_complete_last_dma(&s_spi);
    TEST_ASSERT_EQUAL_UINT(cycle, done.done_count);
    TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_lcd_detach(&lcd));
    esy_spi_scheduler_test_iteration(gateway);
    TEST_ASSERT_EQUAL_UINT(cycle + 1u, done.done_count);
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  }
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_quiesce(gateway));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_resume(gateway));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gateway));
  esy_os_task_t *caller = fake_esy_os_task_get_current();
  fake_esy_os_task_set_current(fake_esy_os_task_get_last_created()->task_handle);
  esy_spi_scheduler_test_iteration(gateway);
  fake_esy_os_task_set_current(caller);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_spi_scheduler_destroy(gateway));
}

void test_cs_cleanup_fault_blocks_io_and_deinit_retries(void) {
  esy_lcd_t lcd; test_done_t done = {0}; init_lcd(&lcd);
  s_spi_async.defer_callbacks = true;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_flush(&lcd,0,0,1,0,s_color_buf,done_cb,&done));
  TEST_ASSERT_EQUAL_INT(ESY_OK, s_spi_async.prepare[0](s_spi_async.callback_arg[0]));
  TEST_ASSERT_FALSE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  fake_esy_hal_gpio_fail_write_after(0, ESY_ERR_IO);
  s_spi_async.done[0](ESY_OK,s_spi_async.callback_arg[0]);
  TEST_ASSERT_EQUAL_UINT(1,done.done_count);
  TEST_ASSERT_FALSE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ili9488_get_status(&lcd));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ili9488_display_on(&lcd));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ili9488_set_direction(&lcd, ESY_LCD_DIR_L2R_U2D));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_lcd_flush(&lcd, 0, 0, 0, 0, s_color_buf, NULL, NULL));
  fake_esy_hal_gpio_fail_write_after(0, ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_lcd_detach(&lcd));
  TEST_ASSERT_TRUE(esy_lcd_is_attached(&lcd));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
}
void test_rotation_exchanges_logical_bounds(void) {
  esy_lcd_t lcd; init_lcd(&lcd);
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_ili9488_set_direction(&lcd,ESY_LCD_DIR_D2U_L2R));
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_lcd_flush(&lcd,479,0,479,0,s_color_buf,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_lcd_flush(&lcd,0,479,0,479,s_color_buf,NULL,NULL));
}
void test_bgr_true_sets_bgr_register_bit(void) {
  esy_lcd_t lcd={0}; esy_ili9488_config_t cfg; fill_default_config(&cfg);cfg.bgr=true;
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_ili9488_init(&lcd,&cfg));
  uint8_t value=255;
  TEST_ASSERT_EQUAL_UINT(1,fake_esy_hal_spi_copy_last_tx(&s_spi,&value,1));
  TEST_ASSERT_EQUAL_HEX8(0x08,value & 0x08);
}
void test_sleep_in_waits_before_next_command(void) {
  esy_lcd_t lcd; init_lcd(&lcd);
  uint32_t tick=fake_esy_hal_system_get_ctx()->tick_ms;
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_ili9488_sleep_in(&lcd));
  TEST_ASSERT_EQUAL_UINT(tick+120,fake_esy_hal_system_get_ctx()->tick_ms);
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_ili9488_sleep_out(&lcd));
  TEST_ASSERT_EQUAL_UINT(tick+240,fake_esy_hal_system_get_ctx()->tick_ms);
}

void test_pm_resume_preserves_explicit_backlight_off(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_device_t spi_device = {0};
  esy_device_t *dependencies[] = {&spi_device};
  esy_lcd_device_config_t device_config = {
      .registration = {
          .name = "ili9488",
          .dependencies = dependencies,
          .dependency_count = 1u,
          .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
      },
  };
  esy_lcd_device_binding_t binding = {0};
  esy_ili9488_config_t config;
  esy_lcd_t lcd = {0};
  const esy_gpio_state_t cs_sleep_state = {
      .mode = ESY_GPIO_MODE_INPUT_PULLDOWN,
  };
  const esy_gpio_pm_pin_t pins[] = {
      {.gpio = &s_cs_gpio, .sleep_state = &cs_sleep_state},
  };
  const esy_device_gpio_pm_profile_t gpio_pm_profile = {
      .pins = pins,
      .pin_count = sizeof(pins) / sizeof(pins[0]),
  };

  device_config.gpio_pm_profile = &gpio_pm_profile;
  fill_default_config(&config);
  config.backlight_initially_enabled = true;
  config.backlight_set_cb = test_backlight_set;
  config.backlight_set_cb_arg = &s_backlight;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_ili9488_init(&lcd, &config));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_set_backlight(&lcd, false));
  fake_esy_hal_spi_set_device(&s_spi, &spi_device);
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_ili9488_register_device(&binding, &lcd, &registry, &device_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, binding.device.ops->pm_action(&binding.device,
                                            ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND,
                                            ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_FALSE(s_backlight.last_enabled);
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_INPUT_PULLDOWN,
                        fake_esy_hal_gpio_get_current_state(&s_cs_gpio).mode);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, binding.device.ops->pm_action(&binding.device,
                                            ESY_DEVICE_PM_ACTION_SYSTEM_RESUME,
                                            ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_FALSE(s_backlight.last_enabled);
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_OUTPUT, fake_esy_hal_gpio_get_current_state(&s_cs_gpio).mode);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_current_state(&s_cs_gpio).output_value);
}

void test_null_dependency_array_is_rejected(void) {
  esy_lcd_t lcd; init_lcd(&lcd); esy_device_t spi_device={0};fake_esy_hal_spi_set_device(&s_spi, &spi_device);
  esy_lcd_device_config_t config={.registration={.dependency_count=1,.dependencies=NULL}};
  esy_lcd_device_binding_t binding={0};esy_device_registry_t registry={0};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
      esy_ili9488_register_device(&binding, &lcd, &registry, &config));
  TEST_ASSERT_NULL(binding.lcd);
}
static esy_err_t test_pm_noop(esy_device_t *device, esy_device_pm_action_t action, esy_power_state_t state) {
  (void)device;(void)action;(void)state;return ESY_OK;
}
static esy_err_t test_backlight_restore_fails(bool enabled, void *arg) {
  (void)arg;
  if (enabled && s_backlight.result != ESY_OK) return s_backlight.result;
  s_backlight.last_enabled = enabled;
  if (!enabled) fake_esy_hal_gpio_fail_write_after(0, ESY_ERR_IO);
  return ESY_OK;
}
static void exercise_suspend_rollback(bool fail_restore) {
  s_backlight.result = fail_restore ? ESY_ERR_IO : ESY_OK;
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage,2);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources,storage);
  esy_device_registry_config_t rc={.static_resources=&resources};
  esy_device_registry_t registry={0};
  static const esy_device_ops_t k_ops={.pm_action=test_pm_noop};
  esy_device_config_t spi_cfg={.name="spi",.ops=&k_ops,.supported_state_mask=ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)|ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)};
  esy_device_t spi_device={0}; esy_device_t *deps[]={&spi_device};
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_device_registry_init(&registry,&rc));
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_device_register(&registry,&spi_device,&spi_cfg));
  esy_lcd_t lcd={0};esy_ili9488_config_t cfg; fill_default_config(&cfg);
  cfg.backlight_set_cb=test_backlight_restore_fails;
  cfg.backlight_initially_enabled = true;
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_ili9488_init(&lcd,&cfg));fake_esy_hal_spi_set_device(&s_spi, &spi_device);
  const esy_gpio_state_t sleep={.mode=ESY_GPIO_MODE_OUTPUT,.output_value=false};
  const esy_gpio_pm_pin_t pin={.gpio=&s_cs_gpio,.sleep_state=&sleep};
  const esy_device_gpio_pm_profile_t profile={.pins=&pin,.pin_count=1};
  esy_lcd_device_config_t dc={.registration={.name="lcd",.dependencies=deps,.dependency_count=1,.supported_state_mask=spi_cfg.supported_state_mask},.gpio_pm_profile=&profile};
  esy_lcd_device_binding_t binding={0};
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_ili9488_register_device(&binding,&lcd,&registry,&dc));
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_device_registry_start(&registry));s_backlight.last_enabled=true;
  TEST_ASSERT_EQUAL_INT(fail_restore ? ESY_ERR_STATE : ESY_ERR_IO,
      esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL(!fail_restore, s_backlight.last_enabled);
  TEST_ASSERT_EQUAL(fail_restore, esy_device_registry_is_faulted(&registry));
  TEST_ASSERT_EQUAL(!fail_restore, esy_device_registry_is_ready(&registry));
}

void test_pm_rejects_reset_assertion_and_accepts_retaining_profile(void) {
  esy_lcd_t lcd; init_lcd(&lcd);
  esy_device_t spi_device = {0}; fake_esy_hal_spi_set_device(&s_spi, &spi_device);
  esy_device_t *deps[] = {&spi_device};
  esy_lcd_device_binding_t binding = {0};
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  esy_device_registry_config_t rc = {.static_resources = &resources};
  esy_device_registry_t registry = {0};
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_device_registry_init(&registry, &rc));
  esy_gpio_state_t reset_state = {.mode = ESY_GPIO_MODE_OUTPUT, .output_value = false};
  esy_gpio_pm_pin_t pin = {.gpio = &s_rst_gpio, .sleep_state = &reset_state};
  esy_device_gpio_pm_profile_t profile = {.pins = &pin, .pin_count = 1u};
  esy_lcd_device_config_t cfg = {.registration = {.name = "lcd", .dependencies = deps,
      .dependency_count = 1u, .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)},
      .gpio_pm_profile = &profile};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_NOT_SUPPORTED, esy_ili9488_register_device(&binding, &lcd, &registry, &cfg));
  TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_rst_gpio));
  reset_state.output_value = true;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_register_device(&binding, &lcd, &registry, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_unregister_device(&binding));
}

void test_initial_rotation_uses_physical_dimensions_and_can_rotate_back(void) {
  esy_ili9488_config_t cfg;
  esy_lcd_t lcd = {0};
  fill_default_config(&cfg);
  cfg.dir = ESY_LCD_DIR_U2D_R2L;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_init(&lcd, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_flush(&lcd, 479, 319, 479, 319, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_lcd_flush(&lcd, 0, 320, 0, 320, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_set_direction(&lcd, ESY_LCD_DIR_R2L_D2U));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_flush(&lcd, 319, 479, 319, 479, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_lcd_flush(&lcd, 320, 0, 320, 0, s_color_buf, NULL, NULL));
}

void test_init_cs_cleanup_failure_retains_handle_until_deinit(void) {
  esy_ili9488_config_t cfg;
  esy_lcd_t lcd = {0};
  fill_default_config(&cfg);
  /* Two reset writes, DC and CS-low precede the first CS-high. */
  fake_esy_hal_gpio_fail_write_after(4u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, esy_ili9488_init(&lcd, &cfg));
  TEST_ASSERT_TRUE(esy_lcd_is_attached(&lcd));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ili9488_get_status(&lcd));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ili9488_init(&lcd, &cfg));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_detach(&lcd));
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_init(&lcd, &cfg));
}

void test_readiness_reports_detached_idle_and_accepted_transfer(void) {
  esy_lcd_t lcd = {0};
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_ili9488_get_status(NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_STATE, esy_ili9488_get_status(&lcd));
  init_lcd(&lcd);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_get_status(&lcd));
  s_spi_async.defer_callbacks = true;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_flush(&lcd, 0, 0, 0, 0, s_color_buf, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ili9488_get_status(&lcd));
  complete_transfer(0u);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_get_status(&lcd));
}

void test_prepare_cleanup_error_retains_callback_ownership_until_completion(void) {
  esy_lcd_t lcd;
  init_lcd(&lcd);
  s_spi_async.defer_callbacks = true;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_flush(&lcd, 0, 0, 0, 0, s_color_buf, NULL, NULL));
  fake_esy_hal_gpio_fail_write_after(2u, ESY_ERR_IO);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_IO, s_spi_async.prepare[0](s_spi_async.callback_arg[0]));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_ili9488_get_status(&lcd));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_BUSY, esy_lcd_detach(&lcd));
  s_spi_async.done[0](ESY_ERR_IO, s_spi_async.callback_arg[0]);
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_ili9488_get_status(&lcd));
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_lcd_detach(&lcd));
}

void test_failed_suspend_rollback_faults_registry(void) {
  exercise_suspend_rollback(true);
}

void test_successful_suspend_rollback_restores_state_and_keeps_registry_ready(void) {
  exercise_suspend_rollback(false);
}

static void assert_padded_parameter(size_t index, uint8_t value) {
  uint8_t actual[3] = {0};
  const uint8_t expected[] = {0u, value};
  TEST_ASSERT_EQUAL_size_t(2u,
      fake_esy_hal_spi_copy_tx_at(&s_spi, index, actual, sizeof(actual)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, sizeof(expected));
}

void test_parameter_encoding_defaults_and_invalid_value(void) {
  esy_lcd_t lcd = {0};
  esy_ili9488_config_t cfg;
  fill_default_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_ILI9488_PARAMETER_U8, cfg.parameter_format);
  cfg.parameter_format = (esy_ili9488_parameter_format_t)99;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_ili9488_init(&lcd, &cfg));
  TEST_ASSERT_EQUAL_size_t(0u, fake_esy_hal_spi_get_tx_count(&s_spi));
  TEST_ASSERT_FALSE(esy_lcd_is_attached(&lcd));
}

void test_padded_init_direction_and_window_leave_pixel_payload_unchanged(void) {
  esy_lcd_t lcd = {0};
  esy_ili9488_config_t cfg;
  const uint8_t data[] = {0x55u, 0xA7u};
  const esy_ili9488_command_t commands[] = {{0x3Au, data, sizeof(data), 0u}};
  const esy_ili9488_panel_profile_t profile = {commands, 1u};
  fill_default_config(&cfg);
  cfg.parameter_format = ESY_ILI9488_PARAMETER_U16_BE;
  cfg.panel_profile = &profile;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ili9488_init(&lcd, &cfg));
  assert_tx_byte_at(0u, 0x3Au);
  assert_padded_parameter(1u, 0x55u);
  assert_padded_parameter(2u, 0xA7u);
  assert_tx_byte_at(3u, 0xB6u);
  assert_padded_parameter(4u, 0u);
  assert_padded_parameter(5u, 0x22u);
  assert_tx_byte_at(6u, 0x36u);
  assert_padded_parameter(7u, 0u);

  size_t base = fake_esy_hal_spi_get_tx_count(&s_spi);
  test_done_t done = {0};
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lcd_flush(&lcd, 258u, 260u, 259u, 261u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_size_t(base + 11u, fake_esy_hal_spi_get_tx_count(&s_spi));
  assert_tx_byte_at(base, 0x2Au);
  assert_padded_parameter(base + 1u, 1u);
  assert_padded_parameter(base + 2u, 2u);
  assert_padded_parameter(base + 3u, 1u);
  assert_padded_parameter(base + 4u, 3u);
  assert_tx_byte_at(base + 5u, 0x2Bu);
  assert_padded_parameter(base + 6u, 1u);
  assert_padded_parameter(base + 7u, 4u);
  assert_padded_parameter(base + 8u, 1u);
  assert_padded_parameter(base + 9u, 5u);
  assert_tx_byte_at(base + 10u, 0x2Cu);
  TEST_ASSERT_EQUAL_PTR(s_color_buf, s_spi_async.tx_ptr[0]);
  TEST_ASSERT_EQUAL_UINT32(8u, s_spi_async.len[0]);
  TEST_ASSERT_EQUAL_UINT32(1u, s_spi_async.payload_count);
  TEST_ASSERT_EQUAL_UINT32(1u, done.done_count);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  TEST_ASSERT_EQUAL(ESY_OK, esy_lcd_detach(&lcd));
}

void test_each_padded_window_write_failure_skips_pixels_and_completes_once(void) {
  esy_lcd_t lcd = {0};
  esy_ili9488_config_t cfg;
  test_done_t done = {0};
  fill_default_config(&cfg);
  cfg.parameter_format = ESY_ILI9488_PARAMETER_U16_BE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_ili9488_init(&lcd, &cfg));
  for (size_t i = 0u; i < 11u; ++i) {
    fake_esy_hal_spi_fail_write_after(i, ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
    TEST_ASSERT_EQUAL(ESY_ERR_IO, s_spi_async.last_result);
    TEST_ASSERT_EQUAL_UINT32(0u, s_spi_async.payload_count);
    TEST_ASSERT_EQUAL_UINT32(i + 1u, done.done_count);
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  }
  /* A short two-byte parameter write must also abort pixel submission. */
  fake_esy_hal_spi_fail_write_after(1u, 1);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, s_spi_async.last_result);
  TEST_ASSERT_EQUAL_UINT32(0u, s_spi_async.payload_count);
  TEST_ASSERT_EQUAL_UINT32(12u, done.done_count);
  TEST_ASSERT_EQUAL(ESY_OK,
      esy_lcd_flush(&lcd, 0u, 0u, 1u, 0u, s_color_buf, done_cb, &done));
  TEST_ASSERT_EQUAL_UINT32(1u, s_spi_async.payload_count);
  TEST_ASSERT_EQUAL_UINT32(13u, done.done_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_lcd_detach(&lcd));
}
