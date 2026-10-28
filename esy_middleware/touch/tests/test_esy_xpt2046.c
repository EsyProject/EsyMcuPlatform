/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_spi_scheduler.h"
#include "fake_esy_os.h"
#include "fake_esy_os_task.h"
#include "fake_esy_hal_spi.h"
extern void esy_spi_scheduler_test_iteration(esy_spi_scheduler_t *gw);

#include "esy_middleware/touch/esy_xpt2046.h"
#include "esy_middleware/touch/esy_xpt2046_device.h"

#include <string.h>

#include "esy_middleware/lcd/esy_lcd.h"
#include "esy_middleware/touch/esy_touch.h"
#include "fake_esy_hal_gpio.h"
#include "esy_hal/esy_hal_gpio_pm.h"
#include "unity.h"

#define TEST_CS_PIN                5u
#define TEST_IRQ_PIN               6u
#define TEST_WIDTH                 320u
#define TEST_HEIGHT                240u
#define TEST_SPI_TRANSFER_CAPACITY 8u

typedef struct {
  uint8_t tx[TEST_SPI_TRANSFER_CAPACITY][3];
  uint8_t rx[TEST_SPI_TRANSFER_CAPACITY][3];
  uint16_t raw_x;
  uint16_t raw_y;
  uint16_t raw_z1;
  uint16_t raw_z2;
  uint32_t get_count;
  uint32_t submit_count;
  uint8_t submitted_cmd[TEST_SPI_TRANSFER_CAPACITY];
  void (*pre_transfer_cb[TEST_SPI_TRANSFER_CAPACITY])(void *arg);
  void *pre_transfer_cb_arg[TEST_SPI_TRANSFER_CAPACITY];
  void (*post_transfer_cb[TEST_SPI_TRANSFER_CAPACITY])(esy_err_t result, uint8_t *rx_buf,
                                                       void *arg);
  void *post_transfer_cb_arg[TEST_SPI_TRANSFER_CAPACITY];
  uint8_t *pending_rx_buf[TEST_SPI_TRANSFER_CAPACITY];
  esy_err_t get_result;
  esy_err_t submit_result;
  int32_t fail_submit_index;
  esy_touch_t *reenter_touch;
  esy_err_t reenter_result;
  uint32_t reenter_count;
  bool defer_callbacks;
  bool reenter_after_post;
  bool return_null_tx;
  bool return_null_rx;
} test_spi_t;

typedef struct {
  uint32_t flush_count;
  uint16_t x0;
  uint16_t y0;
  uint16_t x1;
  uint16_t y1;
  const uint8_t *color_buf;
  void *done_arg;
} test_lcd_backend_t;

static test_spi_t s_spi;
static test_lcd_backend_t s_lcd_backend;
static esy_gpio_t s_cs_gpio;
static esy_gpio_t s_irq_gpio;
static uint8_t s_lcd_color_buf[4];

static void init_gpio(esy_gpio_t *gpio, uint8_t pin, esy_gpio_mode_t mode,
                      bool value) {
  esy_gpio_config_t config;

  if (esy_hal_gpio_is_initialized(gpio))
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(gpio));
  esy_hal_gpio_default_cfg(&config);
  config.pin = pin;
  config.mode = mode;
  config.output_value = value;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_hal_gpio_init(gpio, &config));
}

static void encode_adc(uint8_t *rx_buf, uint16_t value) {
  const uint16_t wire_value = (uint16_t)(value << 3);

  rx_buf[0] = 0u;
  rx_buf[1] = (uint8_t)(wire_value >> 8);
  rx_buf[2] = (uint8_t)wire_value;
}

static esy_err_t test_spi_get_write_buffer(uint8_t **tx_buf, uint8_t **rx_buf,
                                           size_t minimum_size, void *arg) {
  test_spi_t *spi = (test_spi_t *)arg;
  const uint32_t index = spi->get_count;

  TEST_ASSERT_EQUAL_UINT32(3u, minimum_size);

  if (spi->get_result != ESY_OK) {
    return spi->get_result;
  }

  if (index >= TEST_SPI_TRANSFER_CAPACITY) {
    return ESY_ERR_BUSY;
  }

  *tx_buf = spi->return_null_tx ? NULL : spi->tx[index];
  *rx_buf = spi->return_null_rx ? NULL : spi->rx[index];
  spi->get_count++;
  return ESY_OK;
}

static esy_err_t test_spi_submit(uint8_t *tx_buf, uint8_t *rx_buf, uint32_t len,
                                 void (*pre_transfer_cb)(void *arg),
                                 void *pre_transfer_cb_arg,
                                 void (*post_transfer_cb)(esy_err_t result, uint8_t *rx_buf,
                                                          void *arg),
                                 void *post_transfer_cb_arg, void *arg) {
  test_spi_t *spi = (test_spi_t *)arg;
  const uint32_t index = spi->submit_count;

  if ((spi->submit_result != ESY_OK) &&
      ((spi->fail_submit_index < 0) ||
       (spi->fail_submit_index == (int32_t)index))) {
    return spi->submit_result;
  }

  TEST_ASSERT_EQUAL_UINT32(3u, len);
  TEST_ASSERT_TRUE(index < TEST_SPI_TRANSFER_CAPACITY);
  spi->submitted_cmd[index] = tx_buf[0];
  spi->submit_count++;

  if (tx_buf[0] == 0xD0u) {
    encode_adc(rx_buf, spi->raw_x);
  } else if (tx_buf[0] == 0x90u) {
    encode_adc(rx_buf, spi->raw_y);
  } else if (tx_buf[0] == 0xB0u) {
    encode_adc(rx_buf, spi->raw_z1);
  } else if (tx_buf[0] == 0xC0u) {
    encode_adc(rx_buf, spi->raw_z2);
  } else {
    TEST_FAIL_MESSAGE("unexpected XPT2046 command");
  }

  if (spi->defer_callbacks) {
    spi->pre_transfer_cb[index] = pre_transfer_cb;
    spi->pre_transfer_cb_arg[index] = pre_transfer_cb_arg;
    spi->post_transfer_cb[index] = post_transfer_cb;
    spi->post_transfer_cb_arg[index] = post_transfer_cb_arg;
    spi->pending_rx_buf[index] = rx_buf;
    return ESY_OK;
  }

  if (pre_transfer_cb != NULL) {
    pre_transfer_cb(pre_transfer_cb_arg);
  }

  if (post_transfer_cb != NULL) {
    post_transfer_cb(ESY_OK, rx_buf, post_transfer_cb_arg);
  }

  if (spi->reenter_after_post && (spi->reenter_touch != NULL) &&
      (tx_buf[0] == 0x90u) && (spi->reenter_count == 0u)) {
    uint16_t x = 0u;
    uint16_t y = 0u;
    bool pressed = false;

    spi->reenter_count++;
    spi->reenter_result =
        esy_touch_read_input(spi->reenter_touch, &x, &y, &pressed);
  }

  return ESY_OK;
}

static void test_complete_deferred_transfer(uint32_t index) {
  TEST_ASSERT_TRUE(index < s_spi.submit_count);

  if (s_spi.pre_transfer_cb[index] != NULL) {
    s_spi.pre_transfer_cb[index](s_spi.pre_transfer_cb_arg[index]);
  }

  if (s_spi.post_transfer_cb[index] != NULL) {
    s_spi.post_transfer_cb[index](ESY_OK, s_spi.pending_rx_buf[index],
                                  s_spi.post_transfer_cb_arg[index]);
  }
}

static esy_err_t test_lcd_flush(uint16_t x0, uint16_t y0, uint16_t x1,
                                uint16_t y1, const uint8_t *color_buf,
                                void (*done_cb)(void *), void *cb_arg,
                                void *arg) {
  esy_lcd_t *lcd = (esy_lcd_t *)arg;
  test_lcd_backend_t *backend = (test_lcd_backend_t *)lcd->ctx;

  backend->flush_count++;
  backend->x0 = x0;
  backend->y0 = y0;
  backend->x1 = x1;
  backend->y1 = y1;
  backend->color_buf = color_buf;
  backend->done_arg = cb_arg;
  if (done_cb != NULL) {
    done_cb(cb_arg);
  }
  return ESY_OK;
}

static void test_bool_done_cb(void *arg) {
  bool *done = (bool *)arg;

  *done = true;
}

static void fill_attached_lcd(esy_lcd_t *lcd) {
  memset(lcd, 0, sizeof(*lcd));
  lcd->ctx = &s_lcd_backend;
  lcd->flush = test_lcd_flush;
}

static void fill_default_xpt2046_config(esy_xpt2046_config_t *cfg) {
  esy_xpt2046_default_config(cfg);
  init_gpio(&s_cs_gpio, TEST_CS_PIN, ESY_GPIO_MODE_OUTPUT, true);
  cfg->cs_gpio = &s_cs_gpio;
  cfg->irq_gpio = NULL;
  cfg->width = TEST_WIDTH;
  cfg->height = TEST_HEIGHT;
  cfg->dir = ESY_LCD_DIR_R2L_D2U;
  cfg->calibration.mode = ESY_XPT2046_CALIBRATION_LINEAR;
  cfg->calibration.linear.x_scale_q16_16 = ESY_XPT2046_Q16_16_ONE;
  cfg->calibration.linear.y_scale_q16_16 = ESY_XPT2046_Q16_16_ONE;
  cfg->spi_get_write_buffer_cb = test_spi_get_write_buffer;
  cfg->spi_get_write_buffer_cb_arg = &s_spi;
  cfg->spi_submit_result_cb = test_spi_submit;
  cfg->spi_submit_cb_arg = &s_spi;
}

void setUp(void) {
  memset(&s_spi, 0, sizeof(s_spi));
  memset(&s_lcd_backend, 0, sizeof(s_lcd_backend));
  memset(&s_cs_gpio, 0, sizeof(s_cs_gpio));
  memset(&s_irq_gpio, 0, sizeof(s_irq_gpio));
  memset(s_lcd_color_buf, 0x5A, sizeof(s_lcd_color_buf));
  s_spi.raw_x = 100u;
  s_spi.raw_y = 50u;
  s_spi.raw_z1 = 1000u;
  s_spi.raw_z2 = 1200u;
  s_spi.get_result = ESY_OK;
  s_spi.submit_result = ESY_OK;
  s_spi.reenter_result = ESY_OK;
  s_spi.fail_submit_index = -1;
  fake_esy_hal_gpio_reset();
}

void tearDown(void) {}

void test_xpt2046_device_selects_runtime_idle_gpio_pm_state(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_config_t registry_config;
  esy_device_registry_t registry = {0};
  esy_device_t spi_device = {0};
  esy_device_t *dependencies[] = {&spi_device};
  esy_touch_device_config_t device_config = {
      .registration = {
          .name = "xpt2046",
          .dependencies = dependencies,
          .dependency_count = 1u,
          .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION),
      },
  };
  esy_touch_device_binding_t binding = {0};
  esy_xpt2046_config_t config;
  esy_touch_t touch = {0};
  const esy_gpio_state_t cs_idle_state = {
      .mode = ESY_GPIO_MODE_INPUT_PULLUP,
  };
  const esy_gpio_pm_pin_t pins[] = {
      {.gpio = &s_cs_gpio, .idle_state = &cs_idle_state},
  };
  const esy_device_gpio_pm_profile_t gpio_pm_profile = {
      .pins = pins,
      .pin_count = sizeof(pins) / sizeof(pins[0]),
  };

  device_config.gpio_pm_profile = &gpio_pm_profile;
  fill_default_xpt2046_config(&config);
  config.spi_device = &spi_device;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &config));
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  registry_config.static_resources = &resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_xpt2046_register_device(&binding, &touch, &registry, &device_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, binding.device.ops->pm_action(&binding.device,
                                            ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND,
                                            ESY_POWER_STATE_ACTIVE));
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_INPUT_PULLUP,
                        fake_esy_hal_gpio_get_current_state(&s_cs_gpio).mode);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, binding.device.ops->pm_action(&binding.device,
                                            ESY_DEVICE_PM_ACTION_RUNTIME_RESUME,
                                            ESY_POWER_STATE_ACTIVE));
  TEST_ASSERT_EQUAL_INT(ESY_GPIO_MODE_OUTPUT, fake_esy_hal_gpio_get_current_state(&s_cs_gpio).mode);
}

void test_xpt2046_registration_rejects_wrong_backend_and_missing_dependency_array(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  esy_touch_device_binding_t binding = {0};
  esy_device_registry_t registry = {0};
  esy_device_t spi_device = {0};
  esy_touch_device_config_t registration = {
      .registration = {.name = "xpt-invalid-dependencies",
          .dependency_count = 1u, .supported_state_mask =
              ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)}};

  fill_default_xpt2046_config(&cfg);
  cfg.spi_device = &spi_device;
  TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_xpt2046_register_device(
      &binding, &touch, &registry, &registration));
  TEST_ASSERT_NULL(binding.touch);
  TEST_ASSERT_NULL(touch.device);
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
  touch.ctx = &s_spi;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_xpt2046_register_device(
      &binding, &touch, &registry, &registration));
  TEST_ASSERT_NULL(binding.touch);
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
}

static esy_err_t foreign_touch_poll(void *arg) {
  (void)arg;
  TEST_FAIL_MESSAGE("Registration must not invoke a foreign touch provider");
  return ESY_ERR_STATE;
}

static esy_err_t foreign_touch_get_sample(esy_touch_data_t *sample, void *arg) {
  (void)sample;
  (void)arg;
  TEST_FAIL_MESSAGE("Registration must not invoke a foreign touch provider");
  return ESY_ERR_STATE;
}

static void assert_foreign_touch_registration_rejected(void *foreign_context) {
  esy_touch_t touch = {.ctx = foreign_context, .poll = foreign_touch_poll,
                       .get_sample = foreign_touch_get_sample};
  esy_touch_device_binding_t binding = {0};
  esy_device_registry_t registry = {0};
  esy_touch_device_config_t registration = {0};

  TEST_ASSERT_TRUE(esy_touch_is_attached(&touch));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_xpt2046_register_device(
      &binding, &touch, &registry, &registration));
  TEST_ASSERT_NULL(binding.touch);
  TEST_ASSERT_NULL(binding.device.registry);
  TEST_ASSERT_NULL(touch.device);
  TEST_ASSERT_EQUAL_PTR(foreign_context, touch.ctx);
}

void test_xpt2046_registration_rejects_attached_foreign_provider_with_one_byte_context(void) {
  uint8_t foreign_context = 0u;

  assert_foreign_touch_registration_rejected(&foreign_context);
}

void test_xpt2046_registration_rejects_attached_foreign_provider_with_unaligned_context(void) {
  _Alignas(uint32_t) uint8_t foreign_context[sizeof(uint32_t) + 1u] = {0};

  assert_foreign_touch_registration_rejected(&foreign_context[1]);
}

void test_default_config_uses_unused_pins_and_null_callbacks(void) {
  esy_xpt2046_config_t cfg;

  memset(&cfg, 0xA5, sizeof(cfg));
  esy_xpt2046_default_config(&cfg);

  TEST_ASSERT_NULL(cfg.cs_gpio);
  TEST_ASSERT_NULL(cfg.irq_gpio);
  TEST_ASSERT_EQUAL_INT(ESY_XPT2046_CALIBRATION_LINEAR, cfg.calibration.mode);
  TEST_ASSERT_EQUAL_INT32(ESY_XPT2046_Q16_16_ONE,
                          cfg.calibration.linear.x_scale_q16_16);
  TEST_ASSERT_EQUAL_INT32(ESY_XPT2046_Q16_16_ONE,
                          cfg.calibration.linear.y_scale_q16_16);
  TEST_ASSERT_EQUAL_INT(ESY_XPT2046_TOUCH_DETECT_ALWAYS, cfg.touch_detect.mode);
  TEST_ASSERT_NULL(cfg.spi_get_write_buffer_cb);
  TEST_ASSERT_NULL(cfg.spi_submit_result_cb);
}

void test_init_rejects_invalid_required_config(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;

  fill_default_xpt2046_config(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(NULL, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, NULL));

  cfg.cs_gpio = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.width = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.linear.x_scale_q16_16 = 0;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.spi_get_write_buffer_cb = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.spi_submit_result_cb = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.height = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.linear.y_scale_q16_16 = 0;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.dir = (esy_lcd_dir_t)99;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = (esy_xpt2046_calibration_mode_t)99;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = ESY_XPT2046_CALIBRATION_RANGE;
  cfg.calibration.range.raw_x_min = 100u;
  cfg.calibration.range.raw_x_max = 100u;
  cfg.calibration.range.raw_y_min = 200u;
  cfg.calibration.range.raw_y_max = 300u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = (esy_xpt2046_touch_detect_mode_t)99;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_IRQ;
  cfg.irq_gpio = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD;
  cfg.touch_detect.x_plate_ohms = 0u;
  cfg.touch_detect.pressure_max_ohms = 10u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD;
  cfg.touch_detect.x_plate_ohms = 300u;
  cfg.touch_detect.pressure_max_ohms = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));
}

void test_init_failure_preserves_first_handle_and_allows_retry(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint8_t before[sizeof(touch)];

  fill_default_xpt2046_config(&cfg);
  cfg.width = 0u;
  memcpy(before, &touch, sizeof(touch));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_MEMORY(before, &touch, sizeof(touch));
  cfg.width = TEST_WIDTH;
  TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_TRUE(esy_touch_is_attached(&touch));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
}

void test_repeat_init_preserves_completed_sample_until_detach_and_reuse(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  esy_touch_data_t sample;
  uint8_t before[sizeof(touch)];

  fill_default_xpt2046_config(&cfg);
  TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_poll(&touch));
  memcpy(before, &touch, sizeof(touch));
  cfg.width = 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_MEMORY(before, &touch, sizeof(touch));
  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.submit_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_get_sample(&touch, &sample));
  TEST_ASSERT_EQUAL_UINT16(100u, sample.x);
  TEST_ASSERT_EQUAL_UINT16(50u, sample.y);
  TEST_ASSERT_TRUE(sample.is_pressed);
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
  TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_get_sample(&touch, &sample));
  TEST_ASSERT_FALSE(sample.is_pressed);
  TEST_ASSERT_EQUAL_UINT16(0u, sample.x);
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
}

void test_repeat_init_preserves_registered_handle_and_pending_callbacks(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry = {0};
  esy_device_t spi_device = {0};
  esy_device_t *dependencies[] = {&spi_device};
  const esy_touch_device_config_t registration = {
      .registration = {.name = "xpt-repeat-init",
          .dependencies = dependencies, .dependency_count = 1u,
          .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)}};
  const esy_device_config_t spi_registration = {
      .name = "spi", .supported_state_mask =
          ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)};
  esy_touch_device_binding_t binding = {0};
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  esy_touch_data_t sample;
  uint8_t before[sizeof(touch)];

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  const esy_device_registry_config_t graph = {.static_resources = &resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &graph));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_register(
      &registry, &spi_device, &spi_registration));
  fill_default_xpt2046_config(&cfg);
  cfg.spi_device = &spi_device;
  TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_register_device(
      &binding, &touch, &registry, &registration));
  memcpy(before, &touch, sizeof(touch));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_MEMORY(before, &touch, sizeof(touch));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
  s_spi.defer_callbacks = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_poll(&touch));
  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.submit_count);
  memcpy(before, &touch, sizeof(touch));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_MEMORY(before, &touch, sizeof(touch));
  TEST_ASSERT_EQUAL_PTR(&binding.device, touch.device);
  TEST_ASSERT_EQUAL_UINT32(2u, registry.device_count);
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_get_sample(&touch, &sample));
  TEST_ASSERT_FALSE(sample.is_pressed);
  test_complete_deferred_transfer(0u);
  test_complete_deferred_transfer(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_get_sample(&touch, &sample));
  TEST_ASSERT_TRUE(sample.is_pressed);
  TEST_ASSERT_EQUAL_UINT16(100u, sample.x);
  TEST_ASSERT_EQUAL_UINT16(50u, sample.y);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_unregister_device(&binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
  TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
}

void test_read_input_rejects_invalid_arguments_and_uninitialized_handle(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  memset(&touch, 0, sizeof(touch));
  fill_default_xpt2046_config(&cfg);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(&touch, NULL, &y, &pressed));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(&touch, &x, NULL, &pressed));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(&touch, &x, &y, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(NULL, &x, &y, &pressed));

  touch.ctx = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
}

void test_xpt2046_read_after_detach_returns_state_error_without_spi_io(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_TRUE(esy_touch_is_attached(&touch));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_detach(&touch));

  TEST_ASSERT_FALSE(esy_touch_is_attached(&touch));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(0u, s_spi.get_count);
  TEST_ASSERT_EQUAL_UINT32(0u, s_spi.submit_count);
}

void test_read_input_submits_x_then_y_and_returns_transformed_pressed_sample(
    void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.linear.x_offset = 2;
  cfg.calibration.linear.y_offset = -3;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.submit_count);
  TEST_ASSERT_EQUAL_UINT8(0xD0u, s_spi.submitted_cmd[0]);
  TEST_ASSERT_EQUAL_UINT8(0x90u, s_spi.submitted_cmd[1]);
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_has_written(TEST_CS_PIN));
  TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(TEST_CS_PIN));
  TEST_ASSERT_EQUAL_UINT16(102u, x);
  TEST_ASSERT_EQUAL_UINT16(47u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_applies_q16_16_fractional_calibration(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.linear.x_scale_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(1, 2);
  cfg.calibration.linear.y_scale_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(3, 2);
  cfg.calibration.linear.x_offset = 1;
  cfg.calibration.linear.y_offset = -5;
  s_spi.raw_x = 101u;
  s_spi.raw_y = 20u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(51u, x);
  TEST_ASSERT_EQUAL_UINT16(25u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_lcd_facade_and_xpt2046_share_orientation_contract(void) {
  esy_lcd_t lcd;
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  const esy_lcd_dir_t shared_dir = ESY_LCD_DIR_U2D_R2L;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;
  bool flush_done = false;

  fill_attached_lcd(&lcd);
  TEST_ASSERT_TRUE(esy_lcd_is_attached(&lcd));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_lcd_flush(&lcd, 1u, 2u, 2u, 2u, s_lcd_color_buf,
                                        test_bool_done_cb, &flush_done));
  TEST_ASSERT_TRUE(flush_done);
  TEST_ASSERT_EQUAL_UINT32(1u, s_lcd_backend.flush_count);
  TEST_ASSERT_EQUAL_UINT16(1u, s_lcd_backend.x0);
  TEST_ASSERT_EQUAL_UINT16(2u, s_lcd_backend.y0);
  TEST_ASSERT_EQUAL_UINT16(2u, s_lcd_backend.x1);
  TEST_ASSERT_EQUAL_UINT16(2u, s_lcd_backend.y1);
  TEST_ASSERT_EQUAL_PTR(s_lcd_color_buf, s_lcd_backend.color_buf);

  fill_default_xpt2046_config(&cfg);
  cfg.dir = shared_dir;
  s_spi.raw_x = 70u;
  s_spi.raw_y = 120u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(120u, x);
  TEST_ASSERT_EQUAL_UINT16(70u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_applies_raw_range_calibration_with_flags(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = ESY_XPT2046_CALIBRATION_RANGE;
  cfg.calibration.flags = ESY_XPT2046_CALIBRATION_FLAG_SWAP_XY |
                          ESY_XPT2046_CALIBRATION_FLAG_INVERT_X;
  cfg.calibration.range.raw_x_min = 100u;
  cfg.calibration.range.raw_x_max = 300u;
  cfg.calibration.range.raw_y_min = 800u;
  cfg.calibration.range.raw_y_max = 1000u;
  s_spi.raw_x = 100u;
  s_spi.raw_y = 1000u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(0u, x);
  TEST_ASSERT_EQUAL_UINT16(0u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_range_calibration_maps_exact_min_mid_and_max(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = ESY_XPT2046_CALIBRATION_RANGE;
  cfg.calibration.range.raw_x_min = 100u;
  cfg.calibration.range.raw_x_max = 500u;
  cfg.calibration.range.raw_y_min = 200u;
  cfg.calibration.range.raw_y_max = 1000u;

  s_spi.raw_x = 100u;
  s_spi.raw_y = 200u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT16(0u, x);
  TEST_ASSERT_EQUAL_UINT16(0u, y);
  TEST_ASSERT_TRUE(pressed);

  s_spi.raw_x = 300u;
  s_spi.raw_y = 600u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT16((TEST_WIDTH - 1u) / 2u, x);
  TEST_ASSERT_EQUAL_UINT16((TEST_HEIGHT - 1u) / 2u, y);
  TEST_ASSERT_TRUE(pressed);

  s_spi.raw_x = 500u;
  s_spi.raw_y = 1000u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT16(TEST_WIDTH - 1u, x);
  TEST_ASSERT_EQUAL_UINT16(TEST_HEIGHT - 1u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_range_calibration_reaches_all_rectangular_edges_with_every_flag(void) {
  static const uint16_t k_sizes[][2] = {
      {TEST_WIDTH, TEST_HEIGHT}, {TEST_HEIGHT, TEST_WIDTH},
      {1u, TEST_HEIGHT}, {TEST_WIDTH, 1u}};
  static const uint16_t k_raw_corners[][2] = {
      {100u, 200u}, {500u, 200u}, {100u, 1000u}, {500u, 1000u},
      {0u, 0u}, {4095u, 4095u}};

  for (size_t size = 0u; size < sizeof(k_sizes) / sizeof(k_sizes[0]); ++size) {
    for (uint32_t flags = 0u; flags < 8u; ++flags) {
      for (uint32_t reversed = 0u; reversed < 4u; ++reversed) {
        for (size_t corner = 0u;
             corner < sizeof(k_raw_corners) / sizeof(k_raw_corners[0]); ++corner) {
          esy_touch_t touch = {0};
          esy_xpt2046_config_t cfg;
          uint16_t x = UINT16_MAX, y = UINT16_MAX;
          bool pressed = false;
          setUp();
          fill_default_xpt2046_config(&cfg);
          cfg.width = k_sizes[size][0];
          cfg.height = k_sizes[size][1];
          cfg.calibration.mode = ESY_XPT2046_CALIBRATION_RANGE;
          cfg.calibration.flags = flags;
          cfg.calibration.range.raw_x_min = (reversed & 1u) ? 500u : 100u;
          cfg.calibration.range.raw_x_max = (reversed & 1u) ? 100u : 500u;
          cfg.calibration.range.raw_y_min = (reversed & 2u) ? 1000u : 200u;
          cfg.calibration.range.raw_y_max = (reversed & 2u) ? 200u : 1000u;
          s_spi.raw_x = k_raw_corners[corner][0];
          s_spi.raw_y = k_raw_corners[corner][1];

          /* Corners and out-of-range samples must land on exact screen edges. */
          bool right = (s_spi.raw_x >= 500u) != ((reversed & 1u) != 0u);
          bool bottom = (s_spi.raw_y >= 1000u) != ((reversed & 2u) != 0u);
          if ((flags & ESY_XPT2046_CALIBRATION_FLAG_SWAP_XY) != 0u) {
            const bool previous_right = right;
            right = bottom;
            bottom = previous_right;
          }
          right = right != ((flags & ESY_XPT2046_CALIBRATION_FLAG_INVERT_X) != 0u);
          bottom = bottom != ((flags & ESY_XPT2046_CALIBRATION_FLAG_INVERT_Y) != 0u);
          TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
          TEST_ASSERT_EQUAL(ESY_OK, esy_touch_read_input(&touch, &x, &y, &pressed));
          TEST_ASSERT_EQUAL_UINT16(right ? cfg.width - 1u : 0u, x);
          TEST_ASSERT_EQUAL_UINT16(bottom ? cfg.height - 1u : 0u, y);
          TEST_ASSERT_TRUE(pressed);
          TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
        }
      }
    }
  }
}

void test_swapped_range_calibration_preserves_non_square_interior_coordinates(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u, y = 0u;
  bool pressed = false;
  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = ESY_XPT2046_CALIBRATION_RANGE;
  cfg.calibration.flags = ESY_XPT2046_CALIBRATION_FLAG_SWAP_XY;
  cfg.calibration.range.raw_x_min = 100u;
  cfg.calibration.range.raw_x_max = 500u;
  cfg.calibration.range.raw_y_min = 200u;
  cfg.calibration.range.raw_y_max = 1000u;
  s_spi.raw_x = 200u;
  s_spi.raw_y = 800u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL(ESY_OK, esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT16(239u, x);
  TEST_ASSERT_EQUAL_UINT16(59u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_range_calibration_applies_all_post_flags(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = ESY_XPT2046_CALIBRATION_RANGE;
  cfg.calibration.flags = ESY_XPT2046_CALIBRATION_FLAG_SWAP_XY |
                          ESY_XPT2046_CALIBRATION_FLAG_INVERT_X |
                          ESY_XPT2046_CALIBRATION_FLAG_INVERT_Y;
  cfg.calibration.range.raw_x_min = 100u;
  cfg.calibration.range.raw_x_max = 500u;
  cfg.calibration.range.raw_y_min = 200u;
  cfg.calibration.range.raw_y_max = 1000u;
  s_spi.raw_x = 500u;
  s_spi.raw_y = 200u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(TEST_WIDTH - 1u, x);
  TEST_ASSERT_EQUAL_UINT16(0u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_clamps_reversed_range_calibration_edges(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = ESY_XPT2046_CALIBRATION_RANGE;
  cfg.calibration.range.raw_x_min = 300u;
  cfg.calibration.range.raw_x_max = 100u;
  cfg.calibration.range.raw_y_min = 1000u;
  cfg.calibration.range.raw_y_max = 800u;
  s_spi.raw_x = 50u;
  s_spi.raw_y = 1200u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(TEST_WIDTH - 1u, x);
  TEST_ASSERT_EQUAL_UINT16(0u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_applies_affine_calibration(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = ESY_XPT2046_CALIBRATION_AFFINE;
  cfg.calibration.affine.ax_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(2, 1);
  cfg.calibration.affine.bx_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(1, 2);
  cfg.calibration.affine.cx_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(3, 1);
  cfg.calibration.affine.ay_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(-1, 1);
  cfg.calibration.affine.by_q16_16 = ESY_XPT2046_Q16_16_ONE;
  cfg.calibration.affine.cy_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(5, 1);
  s_spi.raw_x = 10u;
  s_spi.raw_y = 20u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(33u, x);
  TEST_ASSERT_EQUAL_UINT16(15u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_clamps_affine_calibration_out_of_bounds(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.calibration.mode = ESY_XPT2046_CALIBRATION_AFFINE;
  cfg.calibration.affine.ax_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(-10, 1);
  cfg.calibration.affine.by_q16_16 = ESY_XPT2046_Q16_16_FROM_RATIO(20, 1);
  s_spi.raw_x = 10u;
  s_spi.raw_y = 20u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(0u, x);
  TEST_ASSERT_EQUAL_UINT16(TEST_HEIGHT - 1u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_calibration_blob_round_trips_and_rejects_bad_crc(void) {
  esy_xpt2046_calibration_t calibration;
  esy_xpt2046_calibration_t imported;
  esy_xpt2046_calibration_blob_t blob;

  memset(&calibration, 0, sizeof(calibration));
  calibration.mode = ESY_XPT2046_CALIBRATION_RANGE;
  calibration.flags = ESY_XPT2046_CALIBRATION_FLAG_INVERT_Y;
  calibration.range.raw_x_min = 10u;
  calibration.range.raw_x_max = 200u;
  calibration.range.raw_y_min = 30u;
  calibration.range.raw_y_max = 220u;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_xpt2046_calibration_export(NULL, &blob));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_xpt2046_calibration_export(&calibration, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_xpt2046_calibration_export(&calibration, &blob));

  TEST_ASSERT_EQUAL_UINT32(ESY_XPT2046_CALIBRATION_BLOB_MAGIC, blob.magic);
  TEST_ASSERT_EQUAL_UINT16(ESY_XPT2046_CALIBRATION_BLOB_VERSION, blob.version);
  TEST_ASSERT_EQUAL_UINT16(sizeof(blob), blob.size);

  memset(&imported, 0, sizeof(imported));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_xpt2046_calibration_import(NULL, &imported));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_xpt2046_calibration_import(&blob, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_xpt2046_calibration_import(&blob, &imported));
  TEST_ASSERT_EQUAL_INT(calibration.mode, imported.mode);
  TEST_ASSERT_EQUAL_UINT32(calibration.flags, imported.flags);
  TEST_ASSERT_EQUAL_UINT16(calibration.range.raw_x_min,
                           imported.range.raw_x_min);
  TEST_ASSERT_EQUAL_UINT16(calibration.range.raw_y_max,
                           imported.range.raw_y_max);

  blob.calibration.range.raw_x_min++;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_CRC_FAIL,
                          esy_xpt2046_calibration_import(&blob, &imported));
}

void test_split_facade_poll_and_get_sample_work_with_xpt2046_backend(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  esy_touch_data_t sample;

  fill_default_xpt2046_config(&cfg);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_poll(&touch));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_get_sample(&touch, &sample));

  TEST_ASSERT_EQUAL_UINT16(100u, sample.x);
  TEST_ASSERT_EQUAL_UINT16(50u, sample.y);
  TEST_ASSERT_TRUE(sample.is_pressed);
}

void test_facade_read_input_uses_xpt2046_split_backend_path(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  s_spi.raw_x = 123u;
  s_spi.raw_y = 45u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.submit_count);
  TEST_ASSERT_EQUAL_UINT16(123u, x);
  TEST_ASSERT_EQUAL_UINT16(45u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_uses_irq_pin_when_configured(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = true;

  fill_default_xpt2046_config(&cfg);
  init_gpio(&s_irq_gpio, TEST_IRQ_PIN, ESY_GPIO_MODE_INPUT, false);
  cfg.irq_gpio = &s_irq_gpio;
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_IRQ;
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_IRQ_PIN, true);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_FALSE(pressed);
}

void test_read_input_always_pressed_mode_ignores_irq_level(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  init_gpio(&s_irq_gpio, TEST_IRQ_PIN, ESY_GPIO_MODE_INPUT, false);
  cfg.irq_gpio = &s_irq_gpio;
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_ALWAYS;
  fake_esy_hal_gpio_set_read_value_for_pin(TEST_IRQ_PIN, true);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.submit_count);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_pressure_threshold_mode_reads_z_and_accepts_low_resistance(
    void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD;
  cfg.touch_detect.x_plate_ohms = 300u;
  cfg.touch_detect.pressure_max_ohms = 10u;
  s_spi.raw_x = 100u;
  s_spi.raw_y = 50u;
  s_spi.raw_z1 = 1000u;
  s_spi.raw_z2 = 1200u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(4u, s_spi.submit_count);
  TEST_ASSERT_EQUAL_UINT8(0xD0u, s_spi.submitted_cmd[0]);
  TEST_ASSERT_EQUAL_UINT8(0x90u, s_spi.submitted_cmd[1]);
  TEST_ASSERT_EQUAL_UINT8(0xB0u, s_spi.submitted_cmd[2]);
  TEST_ASSERT_EQUAL_UINT8(0xC0u, s_spi.submitted_cmd[3]);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_pressure_threshold_mode_rejects_high_resistance(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = true;

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD;
  cfg.touch_detect.x_plate_ohms = 300u;
  cfg.touch_detect.pressure_max_ohms = 10u;
  s_spi.raw_x = 100u;
  s_spi.raw_y = 50u;
  s_spi.raw_z1 = 1000u;
  s_spi.raw_z2 = 4095u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_FALSE(pressed);
}

void test_read_input_pressure_threshold_mode_rejects_invalid_z_sample(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = true;

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD;
  cfg.touch_detect.x_plate_ohms = 300u;
  cfg.touch_detect.pressure_max_ohms = 10u;
  s_spi.raw_z1 = 0u;
  s_spi.raw_z2 = 1200u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_FALSE(pressed);
}

void test_read_input_pressure_threshold_rejects_equal_z_samples(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = true;

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD;
  cfg.touch_detect.x_plate_ohms = 300u;
  cfg.touch_detect.pressure_max_ohms = 10u;
  s_spi.raw_z1 = 1000u;
  s_spi.raw_z2 = 1000u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_FALSE(pressed);
}

void test_read_input_pressure_threshold_rejects_decreasing_z_samples(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = true;

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD;
  cfg.touch_detect.x_plate_ohms = 300u;
  cfg.touch_detect.pressure_max_ohms = 10u;
  s_spi.raw_z1 = 1200u;
  s_spi.raw_z2 = 1000u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_FALSE(pressed);
}

void test_read_input_pressure_threshold_accepts_exact_threshold(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_PRESSURE_THRESHOLD;
  cfg.touch_detect.x_plate_ohms = 4096u;
  cfg.touch_detect.pressure_max_ohms = 100u;
  s_spi.raw_x = 100u;
  s_spi.raw_y = 50u;
  s_spi.raw_z1 = 1000u;
  s_spi.raw_z2 = 2000u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_clamps_transformed_coordinates_to_screen_bounds(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  cfg.dir = ESY_LCD_DIR_L2R_U2D;
  s_spi.raw_x = 0u;
  s_spi.raw_y = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(TEST_WIDTH - 1u, x);
  TEST_ASSERT_EQUAL_UINT16(TEST_HEIGHT - 1u, y);
}

void test_read_input_transforms_all_supported_orientations(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  s_spi.raw_x = 10u;
  s_spi.raw_y = 20u;

  cfg.dir = ESY_LCD_DIR_R2L_D2U;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT16(10u, x);
  TEST_ASSERT_EQUAL_UINT16(20u, y);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_detach(&touch));
  cfg.dir = ESY_LCD_DIR_L2R_U2D;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT16(TEST_WIDTH - 10u, x);
  TEST_ASSERT_EQUAL_UINT16(TEST_HEIGHT - 20u, y);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_detach(&touch));
  cfg.dir = ESY_LCD_DIR_U2D_R2L;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT16(20u, x);
  TEST_ASSERT_EQUAL_UINT16(10u, y);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_touch_detach(&touch));
  cfg.dir = ESY_LCD_DIR_D2U_L2R;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT16(TEST_WIDTH - 20u, x);
  TEST_ASSERT_EQUAL_UINT16(TEST_HEIGHT - 10u, y);
}

void test_read_input_does_not_submit_overlapping_sample_when_async_callbacks_are_pending(
    void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = true;

  fill_default_xpt2046_config(&cfg);
  s_spi.defer_callbacks = true;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.submit_count);
  TEST_ASSERT_EQUAL_UINT16(0u, x);
  TEST_ASSERT_EQUAL_UINT16(0u, y);
  TEST_ASSERT_FALSE(pressed);

  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.submit_count);

  test_complete_deferred_transfer(0u);
  test_complete_deferred_transfer(1u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(4u, s_spi.submit_count);
  TEST_ASSERT_EQUAL_UINT16(100u, x);
  TEST_ASSERT_EQUAL_UINT16(50u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_reports_spi_errors(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));

  s_spi.get_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  s_spi.get_result = ESY_OK;
  s_spi.submit_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
}

void test_read_input_reports_null_spi_buffers(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));

  s_spi.return_null_tx = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  s_spi.return_null_tx = false;
  s_spi.return_null_rx = true;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
}

void test_read_input_can_retry_after_y_submit_failure(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  s_spi.submit_result = ESY_ERR_IO;
  s_spi.fail_submit_index = 1;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.get_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_spi.submit_count);

  s_spi.submit_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(3u, s_spi.submit_count);
  TEST_ASSERT_EQUAL_UINT16(100u, x);
  TEST_ASSERT_EQUAL_UINT16(50u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_read_input_waits_for_pending_x_callback_before_retry_after_y_submit_failure(
    void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  s_spi.defer_callbacks = true;
  s_spi.submit_result = ESY_ERR_IO;
  s_spi.fail_submit_index = 1;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(2u, s_spi.get_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_spi.submit_count);

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(1u, s_spi.submit_count);

  test_complete_deferred_transfer(0u);
  s_spi.defer_callbacks = false;
  s_spi.submit_result = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(3u, s_spi.submit_count);
  TEST_ASSERT_EQUAL_UINT16(100u, x);
  TEST_ASSERT_EQUAL_UINT16(50u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_late_callback_from_previous_generation_does_not_corrupt_active_sample(
    void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  s_spi.defer_callbacks = true;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  test_complete_deferred_transfer(0u);
  test_complete_deferred_transfer(1u);

  s_spi.raw_x = 150u;
  s_spi.raw_y = 75u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(4u, s_spi.submit_count);

  encode_adc(s_spi.pending_rx_buf[0], 300u);
  s_spi.post_transfer_cb[0](ESY_OK, s_spi.pending_rx_buf[0],
                            s_spi.post_transfer_cb_arg[0]);

  test_complete_deferred_transfer(2u);
  test_complete_deferred_transfer(3u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(150u, x);
  TEST_ASSERT_EQUAL_UINT16(75u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_late_null_rx_callback_from_previous_generation_is_ignored(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  s_spi.defer_callbacks = true;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  test_complete_deferred_transfer(0u);
  test_complete_deferred_transfer(1u);

  s_spi.raw_x = 160u;
  s_spi.raw_y = 80u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));
  TEST_ASSERT_EQUAL_UINT32(4u, s_spi.submit_count);

  s_spi.post_transfer_cb[0](ESY_ERR_IO, NULL, s_spi.post_transfer_cb_arg[0]);

  test_complete_deferred_transfer(2u);
  test_complete_deferred_transfer(3u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT16(160u, x);
  TEST_ASSERT_EQUAL_UINT16(80u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_post_callback_reentrant_read_does_not_corrupt_completed_sample(void) {
  esy_touch_t touch = {0};
  esy_xpt2046_config_t cfg;
  uint16_t x = 0u;
  uint16_t y = 0u;
  bool pressed = false;

  fill_default_xpt2046_config(&cfg);
  s_spi.reenter_touch = &touch;
  s_spi.reenter_after_post = true;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_xpt2046_init(&touch, &cfg));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_touch_read_input(&touch, &x, &y, &pressed));

  TEST_ASSERT_EQUAL_UINT32(1u, s_spi.reenter_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, s_spi.reenter_result);
  TEST_ASSERT_EQUAL_UINT16(100u, x);
  TEST_ASSERT_EQUAL_UINT16(50u, y);
  TEST_ASSERT_TRUE(pressed);
}

void test_real_gateway_io_failure_does_not_publish_sample_and_can_retry(void) {
  {
    fake_esy_os_reset(); fake_esy_os_task_reset(); fake_esy_hal_spi_reset();
    esy_spi_t spi = {0};
    ESY_SPI_SCHEDULER_STATIC_STORAGE(resources, 4u, 12u, 12u, 512u);
    esy_spi_scheduler_storage_t storage = {0};
    esy_spi_scheduler_config_t gc;
    esy_spi_scheduler_default_cfg(&gc);
    gc.tx_alloc_size = gc.rx_alloc_size = 3u;
    gc.static_resources.request_storage = resources.request_storage;
    gc.static_resources.request_storage_count = 4u;
    gc.static_resources.tx_buffer_storage = resources.tx_buffer_storage;
    gc.static_resources.tx_buffer_storage_size = 12u;
    gc.static_resources.rx_buffer_storage = resources.rx_buffer_storage;
    gc.static_resources.rx_buffer_storage_size = 12u;
    gc.static_resources.task_stack_buffer = resources.task_stack_buffer;
    gc.static_resources.task_stack_buffer_size = 512u;
    gc.static_resources.task_control_storage = &resources.task_control_storage;
    esy_spi_scheduler_t *gw = NULL;
    TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_create(&spi, &gc, &storage, &gw));
    esy_xpt2046_config_t cfg;
    fill_default_xpt2046_config(&cfg);
    cfg.spi_get_write_buffer_cb = esy_spi_scheduler_get_read_write_buffer_with_arg;
    cfg.spi_get_write_buffer_cb_arg = gw;
    cfg.spi_submit_result_cb = esy_spi_scheduler_submit_result_with_arg;
    cfg.spi_submit_cb_arg = gw;
    esy_touch_t touch = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_poll(&touch));
    for (unsigned i = 0; i < 2; ++i) {
      fake_esy_hal_spi_fail_read_after(0, ESY_ERR_IO);
      esy_spi_scheduler_test_iteration(gw);
    }
    esy_touch_data_t sample = {0};
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_touch_get_sample(&touch, &sample));
    TEST_ASSERT_FALSE(sample.is_pressed);
    fake_esy_hal_spi_reset();
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_poll(&touch));
    esy_spi_scheduler_test_iteration(gw); esy_spi_scheduler_test_iteration(gw);
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_get_sample(&touch, &sample));
    TEST_ASSERT_TRUE(sample.is_pressed);
    esy_os_task_t *caller = fake_esy_os_task_get_current();
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_spi_scheduler_destroy(gw));
    fake_esy_os_task_set_current(fake_esy_os_task_get_last_created()->task_handle);
    esy_spi_scheduler_test_iteration(gw);
    fake_esy_os_task_set_current(caller);
    TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
  }
}

void test_real_gateway_short_buffers_fail_before_writes_or_reservation(void) {
  for (uint32_t tx_size = 1u; tx_size <= 3u; ++tx_size) {
    for (uint32_t rx_size = 1u; rx_size <= 3u; ++rx_size) {
      if (tx_size == 3u && rx_size == 3u) continue;
      fake_esy_hal_spi_reset();
      esy_spi_t spi = {0};
      esy_spi_scheduler_storage_t storage = {0};
      esy_spi_scheduler_request_t requests[2];
      uint8_t tx[6], rx[6];
      memset(tx, 0xA5, sizeof(tx));
      memset(rx, 0x5A, sizeof(rx));
      esy_spi_scheduler_core_config_t gc;
      esy_spi_scheduler_core_default_cfg(&gc);
      gc.block_count = 2u;
      gc.tx_alloc_size = tx_size;
      gc.rx_alloc_size = rx_size;
      gc.static_resources.request_storage = requests;
      gc.static_resources.request_storage_count = 2u;
      gc.static_resources.tx_buffer_storage = tx;
      gc.static_resources.tx_buffer_storage_size = tx_size * 2u;
      gc.static_resources.rx_buffer_storage = rx;
      gc.static_resources.rx_buffer_storage_size = rx_size * 2u;
      esy_spi_scheduler_t *gw = NULL;
      TEST_ASSERT_EQUAL(ESY_OK,
          esy_spi_scheduler_core_create(&spi, &gc, &storage, &gw));
      esy_xpt2046_config_t cfg;
      fill_default_xpt2046_config(&cfg);
      cfg.spi_get_write_buffer_cb = esy_spi_scheduler_get_read_write_buffer_with_arg;
      cfg.spi_get_write_buffer_cb_arg = gw;
      cfg.spi_submit_result_cb = esy_spi_scheduler_submit_result_with_arg;
      cfg.spi_submit_cb_arg = gw;
      esy_touch_t touch = {0};
      TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
      TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_touch_poll(&touch));
      TEST_ASSERT_EACH_EQUAL_HEX8(0xA5u, tx, sizeof(tx));
      TEST_ASSERT_EACH_EQUAL_HEX8(0x5Au, rx, sizeof(rx));
      TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_hal_spi_get_tx_count(&spi));
      TEST_ASSERT_NOT_NULL(esy_spi_scheduler_get_write_block(gw));
      TEST_ASSERT_EQUAL(ESY_OK, esy_touch_detach(&touch));
      TEST_ASSERT_EQUAL(ESY_OK, esy_spi_scheduler_destroy(gw));
    }
  }
}

void test_linear_extreme_offsets_are_clamped_before_narrowing(void) {
  const esy_lcd_dir_t directions[] = {ESY_LCD_DIR_R2L_D2U, ESY_LCD_DIR_L2R_U2D,
      ESY_LCD_DIR_U2D_R2L, ESY_LCD_DIR_D2U_L2R};
  for (unsigned direction = 0u; direction < 4u; ++direction) {
    setUp();
    esy_touch_t touch = {0};
    esy_xpt2046_config_t cfg;
    fill_default_xpt2046_config(&cfg);
    cfg.dir = directions[direction];
    cfg.calibration.linear.x_scale_q16_16 = INT32_MAX;
    cfg.calibration.linear.y_scale_q16_16 = INT32_MIN;
    cfg.calibration.linear.x_offset = INT32_MAX;
    cfg.calibration.linear.y_offset = INT32_MIN;
    TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
    uint16_t x = 0u, y = 0u;
    bool pressed = false;
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_read_input(&touch, &x, &y, &pressed));
    bool inverted = direction == 1u || direction == 3u;
    TEST_ASSERT_EQUAL_UINT16(inverted ? 0u : TEST_WIDTH - 1u, x);
    TEST_ASSERT_EQUAL_UINT16(inverted ? TEST_HEIGHT - 1u : 0u, y);
  }
}

void test_gpio_errors_reject_the_sample_and_leave_outputs_unchanged(void) {
  for (unsigned fault = 0u; fault < 3u; ++fault) {
    setUp();
    esy_touch_t touch = {0};
    esy_xpt2046_config_t cfg;
    fill_default_xpt2046_config(&cfg);
    init_gpio(&s_irq_gpio, TEST_IRQ_PIN, ESY_GPIO_MODE_INPUT_PULLUP, false);
    cfg.irq_gpio = &s_irq_gpio;
    cfg.touch_detect.mode = ESY_XPT2046_TOUCH_DETECT_IRQ;
    TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
    if (fault == 0u) fake_esy_hal_gpio_set_read_result(ESY_ERR_IO);
    else fake_esy_hal_gpio_fail_write_after(fault - 1u, ESY_ERR_IO);
    uint16_t x = 123u, y = 234u;
    bool pressed = false;
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_touch_read_input(&touch, &x, &y, &pressed));
    TEST_ASSERT_EQUAL_UINT16(123u, x);
    TEST_ASSERT_EQUAL_UINT16(234u, y);
    TEST_ASSERT_FALSE(pressed);
    fake_esy_hal_gpio_set_read_result(ESY_OK);
    TEST_ASSERT_EQUAL(ESY_OK, esy_touch_read_input(&touch, &x, &y, &pressed));
    TEST_ASSERT_TRUE(pressed);
  }
}

static esy_err_t pm_parent_noop(esy_device_t *device,
    esy_device_pm_action_t action, esy_power_state_t state) {
  (void)device; (void)action; (void)state; return ESY_OK;
}

void test_partial_gpio_profile_failure_restores_default_or_faults_the_graph(void) {
  for (unsigned persistent = 0u; persistent < 2u; ++persistent) {
    setUp();
    ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
    esy_device_registry_static_resources_t resources;
    ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
    esy_device_registry_config_t rc = {.static_resources = &resources};
    esy_device_registry_t registry = {0};
    esy_device_t parent = {0};
    static const esy_device_ops_t k_ops = {.pm_action = pm_parent_noop};
    uint32_t mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
        ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
    esy_device_config_t pc = {.name = "spi", .ops = &k_ops, .supported_state_mask = mask};
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&registry, &rc));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_register(&registry, &parent, &pc));
    esy_xpt2046_config_t cfg;
    fill_default_xpt2046_config(&cfg);
    cfg.spi_device = &parent;
    init_gpio(&s_irq_gpio, TEST_IRQ_PIN, ESY_GPIO_MODE_OUTPUT, true);
    esy_touch_t touch = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_init(&touch, &cfg));
    const esy_gpio_state_t sleep = {.mode = ESY_GPIO_MODE_OUTPUT, .output_value = false};
    const esy_gpio_pm_pin_t pins[] = {{.gpio = &s_cs_gpio, .sleep_state = &sleep},
        {.gpio = &s_irq_gpio, .sleep_state = &sleep}};
    const esy_device_gpio_pm_profile_t profile = {.pins = pins, .pin_count = 2u};
    esy_device_t *dependencies[] = {&parent};
    esy_touch_device_config_t dc = {.registration = {.name = "touch",
        .supported_state_mask = mask, .dependencies = dependencies, .dependency_count = 1u},
        .gpio_pm_profile = &profile};
    esy_touch_device_binding_t binding = {0};
    TEST_ASSERT_EQUAL(ESY_OK, esy_xpt2046_register_device(&binding, &touch, &registry, &dc));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&registry));
    fake_esy_hal_gpio_fail_apply_after(1u, ESY_ERR_IO, persistent != 0u);
    TEST_ASSERT_EQUAL(persistent ? ESY_ERR_STATE : ESY_ERR_IO,
        esy_device_registry_system_prepare(&registry, ESY_POWER_STATE_RETENTION));
    TEST_ASSERT_EQUAL(persistent != 0u, esy_device_registry_is_faulted(&registry));
    TEST_ASSERT_EQUAL(persistent == 0u, esy_device_registry_is_ready(&registry));
    if (!persistent) TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_current_state(&s_cs_gpio).output_value);
  }
}
