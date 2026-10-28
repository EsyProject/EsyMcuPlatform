/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hal_test_fakes.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esy_common/defs/board/esy_board_defs.h"
#include "esy_common/defs/system/esy_system_defs.h"
#include "esy_hal_backend/esy_hb_flash.h"
#include "esy_hal_backend/esy_hb_boot.h"
#include "esy_hal_backend/esy_hb_low_power.h"
#include "esy_hal_backend/esy_hb_wakeup_timer.h"
#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_hal_backend/esy_hb_spi.h"
#include "esy_hal/esy_hal_adc.h"
#include "esy_hal/esy_hal_dma.h"
#include "esy_hal/esy_hal_flash.h"
#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_i2c.h"
#include "esy_hal/esy_hal_low_power.h"
#include "esy_hal/esy_hal_multicore.h"
#include "esy_hal/esy_hal_pwm.h"
#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_system.h"
#include "esy_hal/esy_hal_timer.h"
#include "esy_hal/esy_hal_timer_capture.h"
#include "esy_hal/esy_hal_uart.h"
#include "esy_hal/esy_hal_usb_dev.h"
#include "esy_hal/esy_hal_watchdog.h"
#include "esy_hal/xio/esy_hal_xio_spi.h"
#include "esy_hal/xio/esy_hal_xio_uart.h"
#include "unity.h"

static platform_test_state_t s_state;
static uint32_t s_event_seq;
static int8_t s_dma_busy_override = -1;
static bool s_adc_busy = true;
static uint8_t *s_uart_rx_stream_storage;
static size_t s_uart_rx_stream_size;
static size_t s_uart_rx_stream_peek_length;
static esy_spi_dma_complete_cb_t s_spi_dma_complete_cb;
static void *s_spi_dma_user_data;
static esy_dma_config_t s_dma_transfer;

esy_err_t esy_hb_uart_validate_config(const esy_uart_config_t *config) {
  return config == NULL ? ESY_ERR_INVALID_ARG : s_state.uart_validate_result;
}

esy_err_t esy_hb_i2c_validate_slave_config(
    const esy_i2c_slave_config_t *config) {
  return config == NULL ? ESY_ERR_INVALID_ARG : s_state.i2c_slave_validate_result;
}

void platform_test_set_uart_validate_result(esy_err_t result) {
  s_state.uart_validate_result = result;
}

void platform_test_set_i2c_slave_validate_result(esy_err_t result) {
  s_state.i2c_slave_validate_result = result;
}

static uint32_t next_event(void) {
  ++s_event_seq;
  return s_event_seq;
}

const platform_test_state_t *platform_test_state(void) { return &s_state; }

void platform_test_set_uart_stream_results(esy_err_t start_result,
                                           esy_err_t stop_result,
                                           bool retain_on_failure) {
  s_state.uart_stream_start_result = start_result;
  s_state.uart_stream_stop_result = stop_result;
  s_state.uart_stream_retain_on_failure = retain_on_failure;
}

void platform_test_set_flash_safe_execute_result(esy_err_t result) {
  s_state.driver_flash_safe_execute_result = result;
}

void platform_test_reset_order_counters(void) {
  s_event_seq = 0u;
  s_state.driver_get_index_order = 0u;
  s_state.board_reset_order = 0u;
  s_state.board_pin_state_order = 0u;
  s_state.driver_deinit_order = 0u;
  s_state.spi_release_order = 0u;
  s_state.uart_release_order = 0u;
  s_state.capture_release_order = 0u;
}

void platform_test_reset(void) {
  memset(&s_state, 0, sizeof(s_state));
  s_state.system_time_us = 456u;
  s_uart_rx_stream_storage = NULL;
  s_uart_rx_stream_size = 0u;
  s_uart_rx_stream_peek_length = 0u;
  s_spi_dma_complete_cb = NULL;
  s_spi_dma_user_data = NULL;
  memset(&s_dma_transfer, 0, sizeof(s_dma_transfer));
  s_dma_busy_override = -1;
  s_adc_busy = true;
  s_event_seq = 0u;
  s_state.configure_role = ESY_SPI_ROLE_SLAVE;
  s_state.pin_state_result = ESY_OK;
  s_state.low_power_supported_mode_mask =
      ESY_HB_LOW_POWER_MODE_MASK(ESY_HB_LOW_POWER_MODE_SLEEP) |
      ESY_HB_LOW_POWER_MODE_MASK(ESY_HB_LOW_POWER_MODE_STOP);
  s_state.always_on_supported = true;
  s_state.retention_supported = true;
  s_state.boot_runtime.update_allowed = true;
}

void platform_test_set_dma_busy(bool busy) {
  s_dma_busy_override = busy ? 1 : 0;
}

void platform_test_set_dma_owner(uint8_t resource, const void *owner) {
  TEST_ASSERT_LESS_THAN_UINT32(TEST_DMA_RESOURCE_COUNT, resource);
  s_state.dma_owners[resource] = owner;
}

void platform_test_set_dma_results(esy_err_t start, esy_err_t abort,
                                   esy_err_t control) {
  s_state.dma_start_result = start;
  s_state.dma_abort_result = abort;
  s_state.dma_control_result = control;
}

void platform_test_half_complete_dma(void) {
  TEST_ASSERT_NOT_NULL(s_dma_transfer.half_callback);
  s_dma_transfer.half_callback(s_dma_transfer.half_callback_arg);
}

void platform_test_set_dma_resource_mapping(bool uses_dreq, esy_err_t result) {
  s_state.dma_resource_uses_dreq = uses_dreq;
  s_state.dma_resource_result = result;
}

void platform_test_set_spi_submit_result(esy_err_t result) {
  s_state.spi_submit_result = result;
}

void platform_test_set_spi_claim_result(esy_err_t result) {
  s_state.spi_claim_result = result;
}

void platform_test_set_spi_cleanup_result(esy_err_t result) {
  s_state.spi_cleanup_result = result;
}

void platform_test_set_pwm_results(esy_err_t deinit, esy_err_t abort,
                                  esy_err_t dma, esy_err_t poll) {
  s_state.pwm_deinit_result = deinit;
  s_state.pwm_abort_result = abort;
  s_state.pwm_dma_result = dma;
  s_state.pwm_poll_result = poll;
}

void platform_test_set_pwm_dma_complete_inline(bool complete_inline) {
  s_state.pwm_dma_complete_inline = complete_inline;
}

void platform_test_set_pwm_available_result(esy_err_t result) {
  s_state.pwm_available_result = result;
}

void platform_test_set_uart_dma_results(esy_err_t abort, esy_err_t poll) {
  s_state.uart_abort_result = abort;
  s_state.uart_poll_result = poll;
}

void platform_test_set_uart_cleanup_result(esy_err_t result) {
  s_state.uart_cleanup_result = result;
}

void platform_test_set_uart_suspend_result(esy_err_t result) {
  s_state.uart_suspend_result = result;
}

void platform_test_set_capture_cleanup_result(esy_err_t result) {
  s_state.capture_cleanup_result = result;
}

void platform_test_set_capture_reject_pin(bool reject) {
  s_state.capture_reject_pin = reject;
}

void platform_test_set_adc_busy(bool busy) { s_adc_busy = busy; }

void platform_test_set_adc_results(esy_err_t submit, esy_err_t abort,
                                   esy_err_t deinit) {
  s_state.adc_submit_result = submit;
  s_state.adc_abort_result = abort;
  s_state.adc_deinit_result = deinit;
}


void platform_test_set_boot_layout_available(bool available) {
  s_state.boot_layout_unavailable = !available;
}

void platform_test_set_boot_runtime(esy_boot_runtime_t runtime, esy_err_t result) {
  s_state.boot_runtime = runtime;
  s_state.boot_runtime_result = result;
}

void platform_test_set_boot_layout_result(esy_err_t result) {
  s_state.boot_layout_result = result;
}

void platform_test_set_boot_action_results(esy_err_t pending, esy_err_t confirm) {
  s_state.boot_pending_result = pending;
  s_state.boot_confirm_result = confirm;
}

esy_err_t esy_hb_boot_get_runtime(esy_boot_runtime_t *runtime) {
  if (runtime == NULL) return ESY_ERR_INVALID_ARG;
  *runtime = s_state.boot_runtime;
  if (runtime->mode == ESY_BOOT_MODE_DIRECT)
    runtime->running_address = s_state.last_address;
  return s_state.boot_runtime_result;
}

esy_err_t esy_hb_boot_validate_layout(const esy_bootloader_layout_t *layout) {
  s_state.boot_layout_validate_calls++;
  return layout == NULL ? ESY_ERR_INVALID_ARG : s_state.boot_layout_result;
}

esy_err_t esy_hb_boot_set_pending_image(uint32_t address, uint32_t image_size) {
  s_state.boot_pending_calls++;
  s_state.last_address = address;
  s_state.last_length = image_size;
  return s_state.boot_pending_result;
}

esy_err_t esy_hb_boot_confirm_running_image(void) {
  s_state.boot_confirm_calls++;
  return s_state.boot_confirm_result;
}

void platform_test_set_pc(uintptr_t pc) { s_state.last_address = (uint32_t)pc; }

void platform_test_complete_spi_dma(esy_spi_t *spi,
                                    esy_spi_dma_transfer_status_t status) {
  esy_spi_dma_transfer_result_t result = {
      .status = status,
  };
  esy_spi_dma_complete_cb_t complete_cb = s_spi_dma_complete_cb;
  void *user_data = s_spi_dma_user_data;

  TEST_ASSERT_NOT_NULL(complete_cb);
  s_spi_dma_complete_cb = NULL;
  s_spi_dma_user_data = NULL;
  if (s_dma_busy_override > 0) s_dma_busy_override = 0;
  complete_cb(spi, &result, user_data);
}

void platform_test_complete_dma(bool error) {
  esy_dma_callback_t callback =
      error ? s_dma_transfer.error_callback : s_dma_transfer.callback;
  void *callback_arg =
      error ? s_dma_transfer.error_callback_arg : s_dma_transfer.callback_arg;

  TEST_ASSERT_NOT_NULL(callback);
  callback(callback_arg);
}


void platform_test_set_low_power_result(esy_err_t result) {
  s_state.low_power_result = result;
}

void platform_test_set_low_power_supported_mode_mask(uint32_t mode_mask) {
  s_state.low_power_supported_mode_mask = mode_mask;
}

void platform_test_set_retention_support(bool supported,
                                         esy_err_t restore_result) {
  s_state.retention_supported = supported;
  s_state.retention_restore_result = restore_result;
  if (supported) {
    s_state.low_power_supported_mode_mask |=
        ESY_HB_LOW_POWER_MODE_MASK(ESY_HB_LOW_POWER_MODE_STOP);
  } else {
    s_state.low_power_supported_mode_mask &=
        ~ESY_HB_LOW_POWER_MODE_MASK(ESY_HB_LOW_POWER_MODE_STOP);
  }
}

void platform_test_set_always_on_time(uint64_t now_us, uint64_t elapsed_us,
                                      bool supported) {
  s_state.always_on_time_us = now_us;
  s_state.low_power_elapsed_us = elapsed_us;
  s_state.always_on_supported = supported;
}

void platform_test_set_wakeup_timer_expired(bool expired) {
  s_state.wakeup_timer_expired = expired;
}

void platform_test_set_pin_state_result(esy_err_t result) {
  s_state.pin_state_result = result;
}


bool esy_board_get_bootloader_layout(esy_bootloader_layout_t *layout) {
  TEST_ASSERT_NOT_NULL(layout);
  if (s_state.boot_layout_unavailable) return false;

  layout->slot_addr[0] = OTA_SLOT_A_ADDR;
  layout->slot_addr[1] = OTA_SLOT_B_ADDR;
  layout->meta_addr[0] = OTA_SLOT_A_META_ADDR;
  layout->meta_addr[1] = OTA_SLOT_B_META_ADDR;
  layout->slot_size = OTA_SLOT_TOTAL_SIZE;
  layout->meta_size[0] = 0x1000u;
  layout->meta_size[1] = 0x2000u;
  return true;
}

void esy_board_spi_configure_pins(uint8_t spi_index, esy_spi_role_t role,
                                  esy_spi_inner_config_t *config) {
  (void)config;
  s_state.board_configure_order = next_event();
  s_state.configure_index = spi_index;
  s_state.configure_role = role;
}

void esy_board_spi_reset_pins(uint8_t spi_index) {
  s_state.board_reset_order = next_event();
  s_state.reset_index = spi_index;
}

esy_err_t esy_board_spi_select_pin_state(
    uint8_t spi_index, esy_board_controller_pin_state_t state) {
  s_state.board_pin_state_order = next_event();
  s_state.configure_index = spi_index;
  s_state.last_controller_pin_state = state;
  return s_state.pin_state_result;
}

bool esy_board_spi_get_dma_info(uint8_t spi_index,
                                esy_dma_inner_config_t *tx_cfg,
                                esy_dma_inner_config_t *rx_cfg) {
  TEST_ASSERT_EQUAL_UINT8(TEST_SPI_INDEX, spi_index);

  if (tx_cfg) {
    tx_cfg->direction = ESY_DMA_MEM_TO_PERIPH;
    tx_cfg->dreq = TEST_TX_DREQ;
    tx_cfg->channel = TEST_TX_CHANNEL;
  }
  if (rx_cfg) {
    rx_cfg->direction = ESY_DMA_PERIPH_TO_MEM;
    rx_cfg->dreq = TEST_RX_DREQ;
    rx_cfg->channel = TEST_RX_CHANNEL;
  }

  return true;
}

void esy_board_uart_configure_pins(uint8_t uart_index,
                                   esy_uart_inner_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);
  config->irq_priority = TEST_UART_IRQ_PRIORITY;
  s_state.board_configure_order = next_event();
  s_state.configure_index = uart_index;
}

void esy_board_uart_reset_pins(uint8_t uart_index) {
  s_state.board_reset_order = next_event();
  s_state.reset_index = uart_index;
}

esy_err_t esy_board_uart_select_pin_state(
    uint8_t uart_index, esy_board_controller_pin_state_t state) {
  s_state.board_pin_state_order = next_event();
  s_state.configure_index = uart_index;
  s_state.last_controller_pin_state = state;
  return s_state.pin_state_result;
}

bool esy_board_uart_get_dma_info(uint8_t uart_index,
                                 esy_dma_inner_config_t *cfg, bool is_tx) {
  if (s_state.reject_dma_mapping) return false;
  TEST_ASSERT_EQUAL_UINT8(TEST_UART_INDEX, uart_index);
  TEST_ASSERT_NOT_NULL(cfg);

  cfg->direction = is_tx ? ESY_DMA_MEM_TO_PERIPH : ESY_DMA_PERIPH_TO_MEM;
  cfg->dreq = is_tx ? TEST_TX_DREQ : TEST_RX_DREQ;
  cfg->channel = is_tx ? TEST_TX_CHANNEL : TEST_RX_CHANNEL;
  return true;
}

void esy_board_i2c_configure_pins(uint8_t i2c_index, esy_i2c_role_t role,
                                  esy_i2c_inner_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);
  config->sda_pin = TEST_I2C_SDA_PIN;
  config->scl_pin = TEST_I2C_SCL_PIN;
  s_state.board_configure_order = next_event();
  s_state.configure_index = i2c_index;
  s_state.configure_role = (esy_spi_role_t)role;
}

void esy_board_i2c_reset_pins(uint8_t i2c_index) {
  s_state.board_reset_order = next_event();
  s_state.reset_index = i2c_index;
}

esy_err_t esy_board_i2c_select_pin_state(
    uint8_t i2c_index, esy_board_controller_pin_state_t state) {
  s_state.board_pin_state_order = next_event();
  s_state.configure_index = i2c_index;
  s_state.last_controller_pin_state = state;
  return s_state.pin_state_result;
}

bool esy_board_i2c_get_dma_info(uint8_t i2c_index, esy_dma_inner_config_t *cfg,
                                bool is_tx) {
  if (s_state.reject_dma_mapping) return false;
  TEST_ASSERT_EQUAL_UINT8(TEST_I2C_INDEX, i2c_index);
  TEST_ASSERT_NOT_NULL(cfg);

  cfg->direction = is_tx ? ESY_DMA_MEM_TO_PERIPH : ESY_DMA_PERIPH_TO_MEM;
  cfg->dreq = is_tx ? TEST_TX_DREQ : TEST_RX_DREQ;
  cfg->channel = is_tx ? TEST_TX_CHANNEL : TEST_RX_CHANNEL;
  return true;
}

void esy_board_pwm_configure_pin(uint8_t pwm_index,
                                 esy_pwm_inner_config_t *config) {
  (void)config;
  s_state.board_configure_order = next_event();
  s_state.configure_index = pwm_index;
}

void esy_board_pwm_reset_pin(uint8_t pwm_index) {
  s_state.board_reset_order = next_event();
  s_state.reset_index = pwm_index;
}

bool esy_board_pwm_get_dma_info(uint8_t pwm_index,
                                esy_dma_inner_config_t *cfg) {
  if (s_state.reject_dma_mapping) return false;
  TEST_ASSERT_EQUAL_UINT8(TEST_PWM_INDEX, pwm_index);
  TEST_ASSERT_NOT_NULL(cfg);

  cfg->direction = ESY_DMA_MEM_TO_PERIPH;
  cfg->dreq = TEST_PWM_DREQ;
  cfg->channel = TEST_PWM_CHANNEL;
  return true;
}

uint8_t esy_board_gpio_get_irq_priority(uint32_t pin) {
  TEST_ASSERT_EQUAL_UINT32(TEST_GPIO_PIN, pin);
  return TEST_GPIO_IRQ_PRIORITY;
}

uint8_t esy_board_dma_get_irq_index(uint32_t dreq, uint8_t channel) {
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  return TEST_DMA_IRQ_INDEX;
}

uint8_t esy_board_dma_get_irq_priority(uint32_t dreq, uint8_t channel) {
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  return TEST_DMA_IRQ_PRIORITY;
}

void esy_board_adc_configure_pin(uint8_t adc_index,
                                 esy_adc_inner_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);
  s_state.board_configure_order = next_event();
  s_state.configure_index = adc_index;
  s_state.last_adc_config = *config;
}

void esy_board_adc_reset_pin(uint8_t adc_index) {
  s_state.board_reset_order = next_event();
  s_state.reset_index = adc_index;
}

bool esy_board_adc_get_dma_info(uint8_t adc_index,
                                esy_dma_inner_config_t *cfg) {
  if (s_state.reject_dma_mapping) return false;
  TEST_ASSERT_EQUAL_UINT8(TEST_ADC_INDEX, adc_index);
  TEST_ASSERT_NOT_NULL(cfg);

  cfg->direction = ESY_DMA_PERIPH_TO_MEM;
  cfg->dreq = TEST_ADC_DREQ;
  cfg->channel = TEST_ADC_CHANNEL;
  return true;
}

bool esy_board_xio_get_dma_info(uint8_t pin, esy_dma_inner_config_t *cfg) {
  if (s_state.reject_dma_mapping) return false;
  TEST_ASSERT_NOT_NULL(cfg);
  s_state.last_xio_pin = pin;
  cfg->direction =
      (pin == TEST_XIO_RX_PIN) ? ESY_DMA_PERIPH_TO_MEM : ESY_DMA_MEM_TO_PERIPH;
  cfg->dreq = 0x700u + pin;
  cfg->channel = (uint8_t)(pin & 0x0Fu);
  return pin != ESY_PIN_UNUSED;
}

void esy_board_xio_configure_pins(uint8_t pin, esy_xio_inner_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);
  s_state.board_configure_order = next_event();
  s_state.last_xio_pin = pin;
  s_state.last_xio_config = *config;
}

bool esy_board_timer_capture_configure_pin(
    uint8_t timer_capture_index, esy_timer_capture_inner_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);
  s_state.board_configure_order = next_event();
  s_state.configure_index = timer_capture_index;
  s_state.last_timer_capture_config = *config;
  return !s_state.capture_reject_pin;
}

void esy_board_timer_capture_reset_pin(uint8_t timer_capture_index) {
  s_state.board_reset_order = next_event();
  s_state.reset_index = timer_capture_index;
}

void esy_board_init(void) { s_state.board_configure_order = next_event(); }

bool esy_board_get_system_config(esy_system_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);

  config->sys_clock_khz = 250000u;
  config->enable_us_tick = true;
  return true;
}

bool esy_board_get_timer_task_inner_config(
    esy_timer_mode_t mode, esy_timer_task_inner_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);

  config->source = ESY_TIMER_SOURCE_DEFAULT;
  switch (mode) {
    case ESY_TIMER_MODE_MS:
      config->irq_priority = 3u;
      return true;
    case ESY_TIMER_MODE_US:
      config->irq_priority = 2u;
      return true;
    default:
      return false;
  }
}

uint8_t esy_board_pwm_get_irq_priority(uint32_t pwm_index) {
  TEST_ASSERT_EQUAL_UINT32(TEST_PWM_INDEX, pwm_index);
  return TEST_PWM_IRQ_PRIORITY;
}

esy_err_t esy_hb_system_init(const esy_system_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);
  s_state.last_system_config = *config;
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  return ESY_OK;
}

uint32_t esy_hb_system_clk_sys_get_hz(void) { return 125000000u; }

void platform_test_set_random_result(esy_err_t result) {
  s_state.driver_random_result = result;
}

esy_err_t esy_hb_system_random_bytes(uint8_t *buffer, size_t length) {
  ++s_state.driver_random_calls;
  s_state.driver_random_buffer = buffer;
  s_state.driver_random_length = length;
  if (s_state.driver_random_result != ESY_OK) return s_state.driver_random_result;
  for (size_t i = 0u; i < length; ++i) buffer[i] = (uint8_t)i;
  return ESY_OK;
}

void esy_hb_system_delay_ms(uint32_t ms) { s_state.last_count = ms; }

void esy_hb_system_delay_us(uint32_t us) { s_state.last_count = us; }

uint32_t esy_hb_system_get_tick(void) { return 123u; }

void esy_hb_system_tick_from_isr(void) { s_state.driver_tick_from_isr_calls++; }

uint32_t esy_hb_system_time_32_us(void) { return s_state.system_time_us; }

esy_err_t esy_hb_system_always_on_time_us(uint64_t *out_time_us) {
  if (out_time_us == NULL) return ESY_ERR_INVALID_ARG;
  if (!s_state.always_on_supported) return ESY_ERR_NOT_SUPPORTED;
  *out_time_us = s_state.always_on_time_us;
  return ESY_OK;
}

uint32_t esy_hb_system_always_on_state_mask(void) {
  return s_state.always_on_supported
             ? ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                   ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE) |
                   ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION)
             : ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE);
}

esy_err_t esy_hb_system_get_tick_partial_us(uint32_t tick_rate_hz,
                                                uint32_t *out_partial_tick_us) {
  if ((tick_rate_hz == 0u) || (out_partial_tick_us == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_partial_tick_us = 250u;
  return ESY_OK;
}

__attribute__((weak)) esy_err_t
esy_hal_system_always_on_time_us(uint64_t *out_time_us) {
  return esy_hb_system_always_on_time_us(out_time_us);
}

__attribute__((weak)) uint32_t esy_hal_system_time_32_us(void) {
  return esy_hb_system_time_32_us();
}

esy_err_t esy_hb_system_suspend_tick(uint32_t tick_rate_hz,
                                         uint32_t *out_partial_tick_us) {
  (void)tick_rate_hz;
  if (out_partial_tick_us == NULL) return ESY_ERR_INVALID_ARG;
  *out_partial_tick_us = 0u;
  return ESY_OK;
}

void esy_hb_system_resume_tick(void) {}

bool esy_hb_system_tick_is_pending(void) { return false; }

void esy_hb_system_disable_irq(void) {}

void esy_hb_system_enable_irq(void) {}

uint32_t esy_hb_system_irq_save(void) {
  s_state.driver_irq_save_calls++;
  return 0x55aau;
}

void esy_hb_system_irq_restore(uint32_t saved_status) {
  s_state.driver_irq_restore_calls++;
  s_state.last_irq_status = saved_status;
}

void esy_hb_system_memory_barrier(void) {
  s_state.driver_memory_barrier_calls++;
}

void esy_hb_system_signal_event(void) {
  s_state.driver_signal_event_calls++;
}

void esy_hb_system_wait_for_event(void) {
  s_state.driver_wait_for_event_calls++;
}

void esy_hb_boot_jump_to_app(uint32_t app_address) {
  s_state.last_address = app_address;
}

void esy_hb_system_reset(void) { s_state.last_count++; }

uintptr_t esy_hb_system_get_pc(void) { return s_state.last_address; }

uint8_t esy_hb_system_get_core_id(void) { return 1u; }

uint8_t esy_hb_system_get_core_count(void) { return 2u; }

bool esy_hb_system_is_in_isr(void) { return true; }

esy_err_t esy_hb_flash_init(void) {
  s_state.driver_init_calls++;
  return ESY_OK;
}

void esy_hb_flash_deinit(void) {
  s_state.driver_deinit_order = next_event();
}

esy_err_t esy_hb_flash_set_safe_execute(
    esy_hb_flash_safe_execute_t safe_execute) {
  s_state.driver_flash_safe_execute_calls++;
  s_state.driver_flash_safe_execute = safe_execute;
  return s_state.driver_flash_safe_execute_result;
}

esy_err_t esy_hb_flash_read(uint32_t address, void *buffer, size_t length) {
  TEST_ASSERT_NOT_NULL(buffer);
  s_state.driver_read_calls++;
  s_state.last_address = address;
  s_state.last_length = length;
  memset(buffer, 0, length);
  return ESY_OK;
}

esy_err_t esy_hb_flash_write(uint32_t address, const void *data,
                                 size_t length) {
  TEST_ASSERT_NOT_NULL(data);
  s_state.driver_write_calls++;
  s_state.last_address = address;
  s_state.last_length = length;
  return ESY_OK;
}

esy_err_t esy_hb_flash_erase(uint32_t address, size_t length) {
  s_state.driver_write_calls++;
  s_state.last_address = address;
  s_state.last_length = length;
  return ESY_OK;
}

void platform_test_set_flash_query_results(esy_err_t geometry_result,
                                            esy_err_t sector_result) {
  s_state.driver_flash_geometry_result = geometry_result;
  s_state.driver_flash_sector_result = sector_result;
}

esy_err_t esy_hb_flash_get_geometry(esy_flash_geometry_t *geometry) {
  s_state.driver_flash_geometry_calls++;
  TEST_ASSERT_NOT_NULL(geometry);
  /* Deliberately write even on failure to verify HAL output isolation. */
  *geometry = (esy_flash_geometry_t){
      .base_address = 0x1000u, .size_bytes = 4096u, .program_size = 4u};
  return s_state.driver_flash_geometry_result;
}

esy_err_t esy_hb_flash_get_sector(uint32_t address,
                                  esy_flash_sector_t *sector) {
  s_state.driver_flash_sector_calls++;
  s_state.last_address = address;
  TEST_ASSERT_NOT_NULL(sector);
  /* Deliberately write even on failure to verify HAL output isolation. */
  *sector = (esy_flash_sector_t){.start_address = 0x1000u, .size_bytes = 4096u};
  if (s_state.driver_flash_sector_result != ESY_OK) {
    return s_state.driver_flash_sector_result;
  }
  return address >= 0x1000u && address < 0x2000u ? ESY_OK : ESY_ERR_INVALID_ARG;
}

bool esy_hb_flash_is_valid_address(uint32_t address) {
  esy_flash_geometry_t geometry;
  s_state.last_address = address;
  return esy_hb_flash_get_geometry(&geometry) == ESY_OK &&
         address >= geometry.base_address &&
         address - geometry.base_address < geometry.size_bytes;
}

esy_err_t esy_hb_gpio_init(uint8_t pin, esy_gpio_mode_t mode) {
  if (s_state.gpio_irq_attached && s_state.gpio_irq_config.pin == pin)
    return ESY_ERR_BUSY;
  s_state.driver_init_calls++;
  s_state.last_pin = pin;
  s_state.last_gpio_mode = mode;
  return ESY_OK;
}

void esy_hb_gpio_deinit(uint8_t pin) {
  s_state.driver_deinit_order = next_event();
  s_state.last_pin = pin;
}

void esy_hb_gpio_write(uint8_t pin, bool value) {
  s_state.driver_write_calls++;
  s_state.last_pin = pin;
  s_state.last_gpio_value = value;
}

bool esy_hb_gpio_read(uint8_t pin) {
  s_state.driver_read_calls++;
  s_state.last_pin = pin;
  return true;
}

esy_err_t esy_hb_gpio_attach_irq(const esy_gpio_irq_config_t *config) {
  TEST_ASSERT_NOT_NULL(config);
  s_state.gpio_irq_config = *config;
  s_state.gpio_irq_attached = true;
  s_state.driver_attach_irq_calls++;
  s_state.last_pin = config->pin;
  s_state.last_gpio_irq_mode = config->irq_mode;
  s_state.last_irq_priority = config->priority;
  s_state.last_gpio_pull = config->pull;
  return ESY_OK;
}

void platform_test_set_gpio_wakeup_results(esy_err_t enable_result,
                                           esy_err_t disable_result,
                                           bool retain_on_failure) {
  s_state.gpio_wakeup_enable_result = enable_result;
  s_state.gpio_wakeup_disable_result = disable_result;
  s_state.gpio_wakeup_retain_on_failure = retain_on_failure;
}

esy_err_t esy_hb_gpio_enable_wakeup(const esy_gpio_irq_config_t *config,
                                    bool *out_cleanup_required) {
  TEST_ASSERT_NOT_NULL(config);
  TEST_ASSERT_NOT_NULL(out_cleanup_required);
  s_state.driver_attach_irq_calls++;
  s_state.last_pin = config->pin;
  s_state.last_gpio_irq_mode = config->irq_mode;
  s_state.last_irq_priority = config->priority;
  s_state.last_gpio_pull = config->pull;
  *out_cleanup_required = s_state.gpio_wakeup_enable_result == ESY_OK ||
                         s_state.gpio_wakeup_retain_on_failure;
  if (*out_cleanup_required) {
    s_state.gpio_irq_config = *config;
    s_state.gpio_irq_attached = true;
  }
  return s_state.gpio_wakeup_enable_result;
}

esy_err_t esy_hb_gpio_disable_wakeup(uint8_t pin) {
  s_state.driver_disable_wakeup_calls++;
  s_state.last_pin = pin;
  if (s_state.gpio_wakeup_disable_result == ESY_OK &&
      s_state.gpio_irq_config.pin == pin) s_state.gpio_irq_attached = false;
  return s_state.gpio_wakeup_disable_result;
}

void esy_hb_gpio_enable_irq(uint8_t pin) { s_state.last_pin = pin; }

void esy_hb_gpio_disable_irq(uint8_t pin) {
  s_state.last_pin = pin;
  if (s_state.gpio_irq_config.pin == pin) s_state.gpio_irq_attached = false;
}

void platform_test_fire_gpio_irq(void) {
  if (s_state.gpio_irq_attached && s_state.gpio_irq_config.handler != NULL)
    s_state.gpio_irq_config.handler(s_state.gpio_irq_config.pin,
                                   s_state.gpio_irq_config.arg);
}

void esy_hb_gpio_handle_irq(uint8_t irq_group) {
  s_state.last_pin = irq_group;
}

esy_err_t esy_hb_low_power_get_capabilities(
    esy_hb_low_power_capabilities_t *out_capabilities) {
  if (out_capabilities == NULL) return ESY_ERR_INVALID_ARG;
  memset(out_capabilities, 0, sizeof(*out_capabilities));
  out_capabilities->supported_mode_mask = s_state.low_power_supported_mode_mask;
  out_capabilities->entry_latency_us[ESY_HB_LOW_POWER_MODE_SLEEP] = 1u;
  out_capabilities->exit_latency_us[ESY_HB_LOW_POWER_MODE_SLEEP] = 2u;
  out_capabilities->min_residency_us[ESY_HB_LOW_POWER_MODE_SLEEP] = 3u;
  out_capabilities->entry_latency_us[ESY_HB_LOW_POWER_MODE_STOP] = 10u;
  out_capabilities->exit_latency_us[ESY_HB_LOW_POWER_MODE_STOP] = 20u;
  out_capabilities->min_residency_us[ESY_HB_LOW_POWER_MODE_STOP] = 30u;
  return ESY_OK;
}

esy_err_t esy_hb_low_power_enter(
    esy_hb_low_power_mode_t mode,
    esy_hb_low_power_result_t *out_result) {
  if (out_result == NULL) return ESY_ERR_INVALID_ARG;
  *out_result = (esy_hb_low_power_result_t){0};
  s_state.driver_start_calls++;
  s_state.last_count = (uint32_t)mode + 1u;
  s_state.always_on_time_us += s_state.low_power_elapsed_us;
  if (s_state.low_power_result != ESY_OK) return s_state.low_power_result;
  out_result->entered = true;
  if (mode == ESY_HB_LOW_POWER_MODE_STOP) {
    s_state.retention_restore_calls++;
    out_result->fatal_error = s_state.retention_restore_result;
    if (out_result->fatal_error != ESY_OK) return out_result->fatal_error;
  }
  return ESY_OK;
}

esy_err_t esy_hb_low_power_prepare(esy_hb_low_power_mode_t mode) {
  if ((uint32_t)mode >= (uint32_t)ESY_HB_LOW_POWER_MODE_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  s_state.driver_prepare_calls++;
  if (mode == ESY_HB_LOW_POWER_MODE_STOP) {
    s_state.retention_prepare_calls++;
  }
  return ESY_OK;
}

esy_err_t esy_hb_low_power_cancel(esy_hb_low_power_mode_t mode) {
  if ((uint32_t)mode >= (uint32_t)ESY_HB_LOW_POWER_MODE_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  if (mode == ESY_HB_LOW_POWER_MODE_STOP) {
    s_state.retention_cancel_calls++;
  }
  return ESY_OK;
}

esy_err_t esy_hb_shutdown(esy_hb_shutdown_mode_t mode) {
  if (mode != ESY_HB_SHUTDOWN_MODE_STANDBY) {
    return ESY_ERR_INVALID_ARG;
  }
  s_state.shutdown_calls++;
  return s_state.low_power_result;
}

esy_err_t esy_hb_wakeup_timer_init(
    esy_hb_wakeup_timer_t *timer,
    const esy_hb_wakeup_timer_config_t *config) {
  if ((timer == NULL) || (config == NULL)) return ESY_ERR_INVALID_ARG;
  memset(timer, 0, sizeof(*timer));
  timer->alarm_index = config->alarm_index;
  timer->initialized = true;
  s_state.wakeup_timer_alarm_index = config->alarm_index;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_deinit(esy_hb_wakeup_timer_t *timer) {
  if ((timer == NULL) || !timer->initialized || timer->prepared) {
    return ESY_ERR_STATE;
  }
  timer->initialized = false;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_get_supported_state_mask(
    esy_hb_wakeup_timer_t *timer, uint32_t *out_state_mask) {
  if ((timer == NULL) || (out_state_mask == NULL)) return ESY_ERR_INVALID_ARG;
  *out_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE);
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_now_atomic(esy_hb_wakeup_timer_t *timer,
                                             uint64_t *out_time_us) {
  if ((timer == NULL) || (out_time_us == NULL)) return ESY_ERR_INVALID_ARG;
  *out_time_us = s_state.always_on_time_us;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_prepare(esy_hb_wakeup_timer_t *timer,
                                          esy_power_state_t state) {
  if ((timer == NULL) || (state != ESY_POWER_STATE_CPU_IDLE)) {
    return ESY_ERR_INVALID_ARG;
  }
  timer->prepared = true;
  s_state.wakeup_timer_prepare_calls++;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_arm_absolute_atomic(
    esy_hb_wakeup_timer_t *timer, esy_power_state_t state,
    uint64_t wake_time_us) {
  if ((timer == NULL) || !timer->prepared ||
      (state != ESY_POWER_STATE_CPU_IDLE)) {
    return ESY_ERR_STATE;
  }
  timer->armed = true;
  s_state.wakeup_timer_armed_time_us = wake_time_us;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_capture_and_cancel_atomic(
    esy_hb_wakeup_timer_t *timer, bool *out_expired) {
  if ((timer == NULL) || (out_expired == NULL) || !timer->armed) {
    return ESY_ERR_STATE;
  }
  *out_expired = s_state.wakeup_timer_expired;
  timer->armed = false;
  timer->irq_masked = true;
  s_state.wakeup_timer_capture_calls++;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_finish(esy_hb_wakeup_timer_t *timer) {
  if ((timer == NULL) || !timer->prepared || !timer->irq_masked) {
    return ESY_ERR_STATE;
  }
  timer->prepared = false;
  timer->irq_masked = false;
  s_state.wakeup_timer_finish_calls++;
  return ESY_OK;
}

esy_err_t esy_hb_wakeup_timer_abort(esy_hb_wakeup_timer_t *timer) {
  if ((timer == NULL) || !timer->prepared || timer->armed) {
    return ESY_ERR_STATE;
  }
  timer->prepared = false;
  s_state.wakeup_timer_abort_calls++;
  return ESY_OK;
}

esy_err_t esy_hb_dma_get_resource_id(uint32_t dreq, uint8_t channel,
                                     uint8_t *out_id) {
  if (out_id == NULL) return ESY_ERR_INVALID_ARG;
  if (s_state.dma_resource_result != ESY_OK) return s_state.dma_resource_result;
  uint32_t id = s_state.dma_resource_uses_dreq ? dreq : channel;
  if (id >= 32u) return ESY_ERR_INVALID_ARG;
  *out_id = (uint8_t)id;
  return ESY_OK;
}

static esy_err_t dma_check_owner(const void *owner, uint32_t dreq,
                                 uint8_t channel, uint8_t *resource) {
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_dma_get_resource_id(dreq, channel, resource);
  if (result != ESY_OK) return result;
  if (s_state.dma_owners[*resource] == NULL) return ESY_ERR_STATE;
  return s_state.dma_owners[*resource] == owner ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_hb_dma_claim(const void *owner, uint32_t dreq, uint8_t channel) {
  uint8_t resource;
  if (owner == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_dma_get_resource_id(dreq, channel, &resource);
  if (result != ESY_OK) return result;
  if (s_state.dma_owners[resource] != NULL) return ESY_ERR_BUSY;
  s_state.dma_owners[resource] = owner;
  return ESY_OK;
}

esy_err_t esy_hb_dma_release(const void *owner, uint32_t dreq, uint8_t channel) {
  uint8_t resource;
  esy_err_t result = dma_check_owner(owner, dreq, channel, &resource);
  if (result == ESY_ERR_STATE) return ESY_OK;
  if (result != ESY_OK) return result;
  s_state.dma_owners[resource] = NULL;
  return ESY_OK;
}

esy_err_t esy_hb_dma_prepare(const void *owner,
                             const esy_dma_config_t *transfer) {
  TEST_ASSERT_NOT_NULL(transfer);
  esy_err_t result = esy_hb_dma_claim(owner, transfer->inner.dreq,
                                    transfer->inner.channel);
  if (result != ESY_OK) return result;
  s_state.last_dma_owner = owner;
  s_state.driver_prepare_calls++;
  s_state.last_dma_inner = transfer->inner;
  s_dma_transfer = *transfer;
  return ESY_OK;
}

esy_err_t esy_hb_dma_start(const void *owner, uint32_t channel_mask) {
  if (owner == NULL || channel_mask == 0u) return ESY_ERR_INVALID_ARG;
  for (uint8_t i = 0u; i < TEST_DMA_RESOURCE_COUNT; ++i) {
    if ((channel_mask & (1u << i)) == 0u) continue;
    if (s_state.dma_owners[i] == NULL) return ESY_ERR_STATE;
    if (s_state.dma_owners[i] != owner) return ESY_ERR_BUSY;
  }
  s_state.last_dma_owner = owner;
  s_state.driver_start_calls++;
  s_state.last_channel_mask = channel_mask;
  return s_state.dma_start_result;
}

esy_err_t esy_hb_dma_start_single(const void *owner,
                                  const esy_dma_config_t *transfer) {
  esy_err_t result = esy_hb_dma_prepare(owner, transfer);
  uint8_t resource;
  if (result != ESY_OK) return result;
  result = esy_hb_dma_get_resource_id(transfer->inner.dreq,
                                     transfer->inner.channel, &resource);
  if (result != ESY_OK) return result;
  return esy_hb_dma_start(owner, 1u << resource);
}

esy_err_t esy_hb_dma_abort(const void *owner, uint32_t dreq, uint8_t channel) {
  uint8_t resource;
  esy_err_t result = dma_check_owner(owner, dreq, channel, &resource);
  if (result == ESY_ERR_STATE) return ESY_OK;
  if (result != ESY_OK) return result;
  s_state.last_dma_owner = owner;
  s_state.dma_abort_calls++;  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  if (s_state.dma_abort_result == ESY_OK) s_state.dma_owners[resource] = NULL;
  return s_state.dma_abort_result;
}

bool esy_hb_dma_is_busy(uint32_t dreq, uint8_t channel) {
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  if (s_dma_busy_override >= 0) return s_dma_busy_override != 0;
  return (dreq == TEST_TX_DREQ) && (channel == TEST_TX_CHANNEL);
}

__attribute__((weak)) bool esy_hal_dma_is_busy(const esy_dma_t *dma,
                                                    uint32_t dreq,
                                                    uint8_t channel) {
  (void)dma;
  return esy_hb_dma_is_busy(dreq, channel);
}

esy_err_t esy_hb_dma_bind_irq_to_core(uint32_t irq_index,
                                          uint32_t core_id) {
  s_state.last_dreq = irq_index;
  s_state.last_channel = (uint8_t)core_id;
  return ESY_OK;
}

esy_err_t esy_hb_dma_set_read_addr(const void *owner, uint32_t dreq, uint8_t channel,
                                  const void *addr, bool trigger_now) {
  uint8_t resource;
  esy_err_t result = dma_check_owner(owner, dreq, channel, &resource);
  if (result != ESY_OK) return result;
  (void)addr;
  (void)trigger_now;
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  s_state.dma_control_calls++;
  return s_state.dma_control_result;
}

esy_err_t esy_hb_dma_set_write_addr(const void *owner, uint32_t dreq,
                                   uint8_t channel, void *addr,
                                   bool trigger_now) {
  uint8_t resource;
  esy_err_t result = dma_check_owner(owner, dreq, channel, &resource);
  if (result != ESY_OK) return result;
  (void)addr;
  (void)trigger_now;
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  s_state.dma_control_calls++;
  return s_state.dma_control_result;
}

esy_err_t esy_hb_dma_set_trans_count(const void *owner, uint32_t dreq, uint8_t channel,
                                    size_t count, bool trigger_now) {
  uint8_t resource;
  esy_err_t result = dma_check_owner(owner, dreq, channel, &resource);
  if (result != ESY_OK) return result;
  (void)trigger_now;
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  s_state.last_count = (uint32_t)count;
  s_state.dma_control_calls++;
  return s_state.dma_control_result;
}

esy_err_t esy_hb_dma_get_trans_count_remaining(uint32_t dreq,
                                                   uint8_t channel,
                                                   size_t *out_remaining) {
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  if (out_remaining) *out_remaining = 17u;
  return ESY_OK;
}

esy_err_t esy_hb_dma_chain_to(const void *owner, uint32_t dreq, uint8_t channel,
                             uint8_t chain_channel) {
  uint8_t resource;
  esy_err_t result = dma_check_owner(owner, dreq, channel, &resource);
  if (result != ESY_OK) return result;
  if (chain_channel >= TEST_DMA_RESOURCE_COUNT ||
      s_state.dma_owners[chain_channel] != owner) return ESY_ERR_BUSY;
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
  s_state.last_count = chain_channel;
  s_state.dma_control_calls++;
  return s_state.dma_control_result;
}

void esy_hb_dma_handle_irq(uint32_t dreq, uint8_t channel) {
  s_state.last_dreq = dreq;
  s_state.last_channel = channel;
}

esy_err_t esy_hb_timer_init(void) {
  s_state.driver_init_calls++;
  return ESY_OK;
}

void esy_hb_timer_deinit(void) {
  s_state.driver_deinit_order = next_event();
}

int32_t esy_hb_timer_start(const esy_timer_task_config_t *task_config) {
  TEST_ASSERT_NOT_NULL(task_config);
  s_state.driver_start_calls++;
  s_state.last_count = (uint32_t)task_config->interval;
  s_state.last_irq_priority = task_config->inner.irq_priority;
  s_state.last_timer_source = task_config->inner.source;
  s_state.timer_callback = task_config->callback;
  s_state.timer_callback_arg = task_config->callback_arg;
  if (s_state.timer_scripted) {
    int32_t id = s_state.timer_next_id;
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(0, id);
    TEST_ASSERT_LESS_THAN_INT32(4, id);
    s_state.timer_active_mask |= 1u << (uint32_t)id;
    if (s_state.timer_start_inline) {
      s_state.system_time_us += task_config->mode == ESY_TIMER_MODE_US
          ? (uint32_t)task_config->interval
          : (uint32_t)task_config->interval * 1000u;
      task_config->callback(task_config->callback_arg);
    }
    return id;
  }
  if (s_state.timer_immediate) {
    int32_t id = (int32_t)s_state.driver_start_calls;
    if (!task_config->repeat) task_config->callback(task_config->callback_arg);
    return id;
  }
  return 2;
}

void esy_hb_timer_cancel(int32_t timer_id) {
  s_state.driver_write_calls++;
  s_state.last_count = (uint32_t)timer_id;
  if (s_state.timer_scripted)
    s_state.timer_active_mask &= ~(1u << (uint32_t)timer_id);
}

void esy_hb_timer_cancel_all(void) { s_state.timer_active_mask = 0u; }

void esy_hb_timer_handle_ms_irq(void) { s_state.last_dreq = 1u; }

void esy_hb_timer_handle_us_irq(void) { s_state.last_dreq = 2u; }

uint8_t esy_hb_timer_get_max_timer_task(void) { return 4u; }

bool esy_hb_timer_is_active(int32_t timer_id) {
  return s_state.timer_scripted
      ? timer_id >= 0 && timer_id < 4 &&
          (s_state.timer_active_mask & (1u << (uint32_t)timer_id)) != 0u
      : timer_id == 2;
}

esy_err_t esy_hb_watchdog_init(uint32_t timeout_ms, bool pause_on_debug) {
  s_state.driver_init_calls++;
  s_state.last_count = timeout_ms;
  s_state.last_pause_on_debug = pause_on_debug;
  return ESY_OK;
}

esy_err_t esy_hb_watchdog_deinit(void) {
  s_state.driver_deinit_order = next_event();
  return ESY_ERR_NOT_SUPPORTED;
}
esy_err_t esy_hb_watchdog_get_timeout_lower_bound_us(uint64_t *out_timeout_us) {
  if (out_timeout_us == NULL) return ESY_ERR_INVALID_ARG;
  return ESY_ERR_NOT_SUPPORTED;
}

void esy_hb_watchdog_feed(void) { s_state.driver_write_calls++; }

void esy_hb_watchdog_enable(void) { s_state.driver_start_calls++; }

bool esy_hb_watchdog_was_reset(void) { return true; }

void esy_hb_watchdog_clear_reset(void) { s_state.driver_read_calls++; }

esy_err_t esy_hb_multicore_start_core(uint8_t core_id,
                                          void (*entry)(void)) {
  TEST_ASSERT_NOT_NULL(entry);
  s_state.driver_start_calls++;
  s_state.last_core_id = core_id;
  return ESY_OK;
}

esy_err_t esy_hb_multicore_queue_init(esy_multicore_queue_t *queue,
                                          uint32_t item_size, uint32_t depth,
                                          uint8_t *buf, size_t buf_size) {
  (void)buf_size;
  TEST_ASSERT_NOT_NULL(queue);
  TEST_ASSERT_NOT_NULL(buf);
  s_state.driver_init_calls++;
  s_state.last_count = item_size;
  s_state.last_length = depth;
  queue->ctx = buf;
  return ESY_OK;
}

void esy_hb_multicore_queue_deinit(esy_multicore_queue_t *queue) {
  TEST_ASSERT_NOT_NULL(queue);
  s_state.driver_deinit_order = next_event();
}

void esy_hb_multicore_queue_send_blocking(esy_multicore_queue_t *queue,
                                              const void *data) {
  TEST_ASSERT_NOT_NULL(queue);
  TEST_ASSERT_NOT_NULL(data);
  s_state.driver_write_calls++;
}

bool esy_hb_multicore_queue_try_send(esy_multicore_queue_t *queue,
                                         const void *data) {
  TEST_ASSERT_NOT_NULL(queue);
  TEST_ASSERT_NOT_NULL(data);
  s_state.driver_write_calls++;
  return true;
}

void esy_hb_multicore_queue_receive_blocking(esy_multicore_queue_t *queue,
                                                 void *data) {
  TEST_ASSERT_NOT_NULL(queue);
  TEST_ASSERT_NOT_NULL(data);
  s_state.driver_read_calls++;
}

bool esy_hb_multicore_queue_try_receive(esy_multicore_queue_t *queue,
                                            void *data) {
  TEST_ASSERT_NOT_NULL(queue);
  TEST_ASSERT_NOT_NULL(data);
  s_state.driver_read_calls++;
  return true;
}

esy_err_t esy_hb_spi_claim(esy_spi_t *handle,
                          const esy_spi_config_t *config) {
  if (handle == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
  if (s_state.spi_claim_result != ESY_OK) return s_state.spi_claim_result;
  if (handle->ctx != NULL) {
    return s_state.spi_owner == handle &&
                   handle->ctx == handle->backend.bytes &&
                   handle->backend.bytes[0] == config->phys_index &&
                   !s_state.spi_enabled
               ? ESY_OK : ESY_ERR_STATE;
  }
  if (s_state.spi_owner != NULL) return ESY_ERR_BUSY;
  s_state.spi_owner = handle;
  s_state.spi_claim_order = next_event();
  handle->backend.bytes[0] = config->phys_index;
  handle->ctx = handle->backend.bytes;
  return ESY_OK;
}

esy_err_t esy_hb_spi_release(esy_spi_t *handle) {
  TEST_ASSERT_NOT_NULL(handle);
  TEST_ASSERT_EQUAL_PTR(handle, s_state.spi_owner);
  TEST_ASSERT_FALSE(s_state.spi_enabled);
  s_state.spi_release_order = next_event();
  s_state.spi_owner = NULL;
  handle->ctx = NULL;
  return ESY_OK;
}

esy_err_t esy_hb_spi_init(esy_spi_t *handle,
                         const esy_spi_config_t *config) {
  TEST_ASSERT_NOT_NULL(handle);
  TEST_ASSERT_NOT_NULL(config);
  TEST_ASSERT_EQUAL_PTR(handle, s_state.spi_owner);
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  s_state.spi_enabled = true;
  s_state.resumed_speed_hz = config->speed_hz;
  return ESY_OK;
}

esy_err_t esy_hb_spi_deinit(esy_spi_t *handle) {
  TEST_ASSERT_NOT_NULL(handle);
  if (s_state.spi_owner != handle || handle->ctx != handle->backend.bytes)
    return ESY_ERR_STATE;
  if (s_dma_busy_override > 0) return ESY_ERR_BUSY;
  s_state.driver_deinit_order = next_event();
  /* Native retirement can partially stop hardware before reporting failure. */
  s_state.spi_enabled = false;
  return s_state.spi_cleanup_result;
}

esy_err_t esy_hb_spi_get_phys_index(const esy_spi_t *handle,
                                        uint8_t *out_index) {
  if (handle == NULL || out_index == NULL) return ESY_ERR_INVALID_ARG;
  if (s_state.spi_owner != handle || handle->ctx != handle->backend.bytes)
    return ESY_ERR_STATE;
  s_state.driver_get_index_order = next_event();
  *out_index = handle->backend.bytes[0];
  return ESY_OK;
}

int32_t esy_hb_spi_write_blocking(esy_spi_t *handle, const uint8_t *tx_buf,
                                      size_t len) {
  TEST_ASSERT_NOT_NULL(handle);
  TEST_ASSERT_NOT_NULL(tx_buf);
  s_state.driver_write_calls++;
  return (int32_t)len;
}

int32_t esy_hb_spi_read_blocking(esy_spi_t *handle, uint8_t *rx_buf,
                                     size_t len) {
  (void)handle;
  (void)rx_buf;
  return (int32_t)len;
}

int32_t esy_hb_spi_transfer_blocking(esy_spi_t *handle,
                                         const uint8_t *tx_buf, uint8_t *rx_buf,
                                         size_t len) {
  (void)handle;
  (void)tx_buf;
  (void)rx_buf;
  return (int32_t)len;
}

int32_t esy_hb_spi_write16_blocking(esy_spi_t *handle,
                                        const uint16_t *tx_buf, size_t len) {
  (void)handle;
  (void)tx_buf;
  return (int32_t)len;
}

int32_t esy_hb_spi_read16_blocking(esy_spi_t *handle, uint16_t *rx_buf,
                                       size_t len) {
  (void)handle;
  (void)rx_buf;
  return (int32_t)len;
}

int32_t esy_hb_spi_transfer16_blocking(esy_spi_t *handle,
                                           const uint16_t *tx_buf,
                                           uint16_t *rx_buf, size_t len) {
  (void)handle;
  (void)tx_buf;
  (void)rx_buf;
  return (int32_t)len;
}

esy_err_t esy_hb_spi_transfer_dma(esy_spi_t *handle,
                                      esy_dma_config_t *tx_dma_config,
                                      esy_dma_config_t *rx_dma_config,
                                      esy_spi_dma_complete_cb_t complete_cb,
                                      void *user_data) {
  TEST_ASSERT_NOT_NULL(handle);
  TEST_ASSERT_NOT_NULL(tx_dma_config);
  TEST_ASSERT_NOT_NULL(rx_dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_tx_inner = tx_dma_config->inner;
  s_state.last_rx_inner = rx_dma_config->inner;
  s_state.last_spi_tx_dma = *tx_dma_config;
  s_state.last_spi_rx_dma = *rx_dma_config;
  s_spi_dma_complete_cb = complete_cb;
  s_spi_dma_user_data = user_data;
  if (s_state.spi_submit_result != ESY_OK && s_dma_busy_override <= 0) {
    s_spi_dma_complete_cb = NULL;
    s_spi_dma_user_data = NULL;
  }
  return s_state.spi_submit_result;
}

esy_err_t esy_hb_spi_get_dma_trans_count_remaining(
    const esy_spi_t *handle, size_t *out_tx_remaining,
    size_t *out_rx_remaining) {
  (void)handle;
  if (out_tx_remaining) *out_tx_remaining = 11u;
  if (out_rx_remaining) *out_rx_remaining = 22u;
  return ESY_OK;
}

esy_err_t esy_hb_spi_set_speed(esy_spi_t *handle, uint32_t speed_hz) {
  (void)handle;
  return speed_hz ? ESY_OK : ESY_ERR_INVALID_ARG;
}

esy_err_t esy_hb_spi_abort(esy_spi_t *handle) {
  (void)handle;
  ++s_state.spi_abort_calls;
  s_spi_dma_complete_cb = NULL;
  s_spi_dma_user_data = NULL;
  s_dma_busy_override = 0;
  return ESY_OK;
}

esy_err_t esy_hb_spi_get_caps(const esy_spi_t *handle, uint32_t *out_caps) {
  (void)handle;
  if (!out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_SPI_CAP_DMA_TRANSFER | ESY_SPI_CAP_TRANSFER16;
  return ESY_OK;
}

void esy_hb_spi_handle_irq(esy_spi_t *handle) { (void)handle; }

esy_err_t esy_hb_uart_claim(esy_uart_t *uart, uint8_t phys_index) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] != NULL || uart->ctx[1] != NULL) {
    return s_state.uart_owner == uart &&
                   uart->ctx[0] == uart->backend.bytes &&
                   uart->ctx[1] == NULL && !s_state.uart_enabled &&
                   uart->backend.bytes[0] == phys_index
               ? ESY_OK : ESY_ERR_STATE;
  }
  if (s_state.uart_owner != NULL) return ESY_ERR_BUSY;
  s_state.uart_owner = uart;
  s_state.uart_claim_order = next_event();
  uart->backend.bytes[0] = phys_index;
  uart->ctx[0] = uart->backend.bytes;
  return ESY_OK;
}

esy_err_t esy_hb_uart_release(esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_EQUAL_PTR(uart, s_state.uart_owner);
  TEST_ASSERT_FALSE(s_state.uart_enabled);
  s_state.uart_release_order = next_event();
  s_state.uart_owner = NULL;
  uart->ctx[0] = NULL;
  return ESY_OK;
}

esy_err_t esy_hb_uart_init(esy_uart_t *uart,
                               const esy_uart_config_t *config) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(config);
  TEST_ASSERT_EQUAL_PTR(uart, s_state.uart_owner);
  s_state.last_irq_priority = config->inner.irq_priority;
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  s_state.uart_enabled = true;
  return ESY_OK;
}

esy_err_t esy_hb_uart_deinit(esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  if (s_state.uart_owner != uart || uart->ctx[0] != uart->backend.bytes)
    return ESY_ERR_STATE;
  s_state.driver_deinit_order = next_event();
  if (s_state.uart_cleanup_result != ESY_OK)
    return s_state.uart_cleanup_result;
  s_state.uart_enabled = false;
  return ESY_OK;
}

int32_t esy_hb_uart_send_blocking(esy_uart_t *uart, const uint8_t *data,
                                      size_t len) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(data);
  s_state.driver_write_calls++;
  return (int32_t)len;
}

int32_t esy_hb_uart_receive_blocking(esy_uart_t *uart, uint8_t *data,
                                         size_t len) {
  (void)uart;
  (void)data;
  return (int32_t)len;
}

esy_err_t esy_hb_uart_send_dma(esy_uart_t *uart,
                                   esy_dma_config_t *dma_config) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_tx_inner = dma_config->inner;
  s_state.submitted_dma = *dma_config;
  return ESY_OK;
}

esy_err_t esy_hb_uart_receive_dma(esy_uart_t *uart,
                                      esy_dma_config_t *dma_config) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_rx_inner = dma_config->inner;
  s_state.submitted_dma = *dma_config;
  return ESY_OK;
}

esy_err_t esy_hb_uart_abort_dma(esy_uart_t *uart, bool is_tx) {
  TEST_ASSERT_NOT_NULL(uart);
  ++s_state.uart_abort_calls;
  s_state.uart_abort_is_tx = is_tx;
  return s_state.uart_abort_result;
}

esy_err_t esy_hb_uart_poll(esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  ++s_state.uart_poll_calls;
  return s_state.uart_poll_result;
}

void platform_test_set_uart_busy(bool tx_dma, bool rx_dma, bool wire) {
  s_state.uart_tx_dma_busy = tx_dma;
  s_state.uart_rx_dma_busy = rx_dma;
  s_state.uart_wire_busy = wire;
}

bool esy_hb_uart_is_dma_busy(const esy_uart_t *uart, bool is_tx) {
  return uart != NULL && s_state.uart_owner == uart &&
      (is_tx ? s_state.uart_tx_dma_busy : s_state.uart_rx_dma_busy);
}

esy_err_t esy_hb_uart_rx_stream_start(
    esy_uart_t *uart, uint8_t *storage, size_t storage_size,
    esy_uart_rx_stream_callback_t callback, void *user_data) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(storage);
  TEST_ASSERT_NOT_NULL(callback);
  (void)user_data;
  ++s_state.uart_stream_start_calls;
  if (s_state.uart_stream_start_result != ESY_OK &&
      !s_state.uart_stream_retain_on_failure)
    return s_state.uart_stream_start_result;
  s_uart_rx_stream_storage = storage;
  s_uart_rx_stream_size = storage_size;
  s_uart_rx_stream_peek_length = storage_size;
  return s_state.uart_stream_start_result;
}

esy_err_t esy_hb_uart_rx_stream_peek(esy_uart_t *uart, const uint8_t **data,
                                         size_t *length) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(data);
  TEST_ASSERT_NOT_NULL(length);
  *data = s_uart_rx_stream_storage;
  *length = s_uart_rx_stream_peek_length;
  return s_uart_rx_stream_storage ? ESY_OK : ESY_ERR_STATE;
}

esy_err_t esy_hb_uart_rx_stream_consume(esy_uart_t *uart, size_t length) {
  TEST_ASSERT_NOT_NULL(uart);
  if (!s_uart_rx_stream_storage) return ESY_ERR_STATE;
  if (length > s_uart_rx_stream_peek_length) return ESY_ERR_INVALID_ARG;
  s_uart_rx_stream_peek_length -= length;
  return ESY_OK;
}

esy_err_t esy_hb_uart_rx_stream_stop(esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  ++s_state.uart_stream_stop_calls;
  if (s_state.uart_stream_stop_result != ESY_OK)
    return s_state.uart_stream_stop_result;
  s_uart_rx_stream_storage = NULL;
  s_uart_rx_stream_size = 0u;
  s_uart_rx_stream_peek_length = 0u;
  return ESY_OK;
}

void esy_hb_uart_handle_irq(uint8_t phys_index) {
  s_state.last_channel = phys_index;
}

esy_err_t esy_hb_uart_get_dma_trans_count_remaining(
    const esy_uart_t *uart, size_t *out_tx_remaining,
    size_t *out_rx_remaining) {
  (void)uart;
  if (out_tx_remaining) *out_tx_remaining = 1u;
  if (out_rx_remaining) *out_rx_remaining = 2u;
  return ESY_OK;
}

esy_err_t esy_hb_uart_suspend(esy_uart_t *uart) {
  TEST_ASSERT_EQUAL_PTR(uart, s_state.uart_owner);
  s_state.uart_enabled = false;
  return s_state.uart_suspend_result;
}

esy_err_t esy_hb_uart_resume(esy_uart_t *uart) {
  TEST_ASSERT_EQUAL_PTR(uart, s_state.uart_owner);
  if (s_state.uart_resume_result == ESY_OK) s_state.uart_enabled = true;
  return s_state.uart_resume_result;
}

bool esy_hb_uart_is_readable(const esy_uart_t *uart) {
  (void)uart;
  return true;
}

esy_err_t esy_hb_uart_set_baudrate(esy_uart_t *uart, uint32_t baudrate) {
  (void)uart;
  return baudrate ? ESY_OK : ESY_ERR_INVALID_ARG;
}

esy_err_t esy_hb_uart_get_caps(const esy_uart_t *uart, uint32_t *out_caps) {
  (void)uart;
  if (!out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_UART_CAP_TX_DMA | ESY_UART_CAP_RX_DMA | ESY_UART_CAP_RX_STREAM;
  return ESY_OK;
}

esy_err_t esy_hb_uart_get_phys_index(const esy_uart_t *uart,
                                         uint8_t *out_index) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(out_index);
  s_state.driver_get_index_order = next_event();
  if (s_state.uart_owner != uart || uart->ctx[0] != uart->backend.bytes)
    return ESY_ERR_STATE;
  *out_index = uart->backend.bytes[0];
  return ESY_OK;
}

bool esy_hb_uart_is_transmitting(const esy_uart_t *uart) {
  return uart != NULL && s_state.uart_owner == uart && s_state.uart_wire_busy;
}

void platform_test_set_i2c_cleanup_result(esy_err_t result) {
  s_state.i2c_cleanup_result = result;
}

void platform_test_set_i2c_busy(bool busy) { s_state.i2c_busy = busy; }

esy_err_t esy_hb_i2c_claim(esy_i2c_t *i2c, uint8_t phys_index) {
  if (i2c == NULL || phys_index != TEST_I2C_INDEX)
    return ESY_ERR_INVALID_ARG;
  if (i2c->ctx != NULL)
    return s_state.i2c_owner == i2c && !s_state.i2c_enabled
               ? ESY_OK : ESY_ERR_STATE;
  if (s_state.i2c_owner != NULL) return ESY_ERR_BUSY;
  s_state.i2c_owner = i2c;
  i2c->backend.bytes[0] = phys_index;
  i2c->ctx = i2c->backend.bytes;
  return ESY_OK;
}

bool esy_hb_i2c_is_busy(const esy_i2c_t *i2c) {
  return i2c != NULL && s_state.i2c_owner == i2c && s_state.i2c_busy;
}

esy_err_t esy_hb_i2c_init_master(esy_i2c_t *i2c,
                                     const esy_i2c_config_t *config) {
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  TEST_ASSERT_NOT_NULL(i2c);
  TEST_ASSERT_NOT_NULL(config);
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  s_state.last_i2c_inner = config->inner;
  TEST_ASSERT_EQUAL_PTR(i2c, s_state.i2c_owner);
  s_state.i2c_enabled = true;
  s_state.resumed_speed_hz = config->speed_hz;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_init_slave(esy_i2c_t *i2c,
                                    const esy_i2c_slave_config_t *config) {
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  TEST_ASSERT_NOT_NULL(i2c);
  TEST_ASSERT_NOT_NULL(config);
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  s_state.last_i2c_inner = config->inner;
  TEST_ASSERT_EQUAL_PTR(i2c, s_state.i2c_owner);
  s_state.i2c_enabled = true;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_suspend(esy_i2c_t *i2c) {
  TEST_ASSERT_NOT_NULL(i2c);
  TEST_ASSERT_EQUAL_PTR(i2c, s_state.i2c_owner);
  if (s_state.i2c_busy) return ESY_ERR_BUSY;
  s_state.driver_deinit_order = next_event();
  if (s_state.i2c_cleanup_result != ESY_OK) return s_state.i2c_cleanup_result;
  s_state.i2c_enabled = false;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_deinit(esy_i2c_t *i2c) {
  TEST_ASSERT_NOT_NULL(i2c);
  TEST_ASSERT_EQUAL_PTR(i2c, s_state.i2c_owner);
  if (s_state.i2c_cleanup_result != ESY_OK) return s_state.i2c_cleanup_result;
  s_state.i2c_enabled = false;
  s_state.i2c_owner = NULL;
  i2c->ctx = NULL;
  return ESY_OK;
}

int32_t esy_hb_i2c_write_blocking(esy_i2c_t *i2c, uint16_t addr,
                                      const uint8_t *data, size_t len) {
  s_state.last_i2c_address = addr;
  TEST_ASSERT_NOT_NULL(i2c);
  TEST_ASSERT_NOT_NULL(data);
  s_state.driver_write_calls++;
  return (int32_t)len;
}

int32_t esy_hb_i2c_read_blocking(esy_i2c_t *i2c, uint16_t addr,
                                     uint8_t *data, size_t len) {
  (void)i2c;
  s_state.last_i2c_address = addr;
  (void)data;
  return (int32_t)len;
}

esy_err_t esy_hb_i2c_write_dma(esy_i2c_t *i2c, uint16_t addr,
                                   esy_dma_config_t *dma_config) {
  s_state.last_i2c_address = addr;
  TEST_ASSERT_NOT_NULL(i2c);
  TEST_ASSERT_NOT_NULL(dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_tx_inner = dma_config->inner;
  s_state.submitted_dma = *dma_config;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_read_dma(esy_i2c_t *i2c, uint16_t addr,
                                  esy_dma_config_t *dma_config) {
  s_state.last_i2c_address = addr;
  TEST_ASSERT_NOT_NULL(i2c);
  TEST_ASSERT_NOT_NULL(dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_rx_inner = dma_config->inner;
  s_state.submitted_dma = *dma_config;
  return ESY_OK;
}

int32_t esy_hb_i2c_transfer(esy_i2c_t *i2c, const esy_i2c_xfer_t *xfer) {
  (void)i2c;
  if (xfer != NULL) s_state.last_i2c_address = xfer->addr;
  return xfer ? (int32_t)(xfer->tx_len + xfer->rx_len) : ESY_ERR_INVALID_ARG;
}

esy_err_t esy_hb_i2c_set_speed(esy_i2c_t *i2c, uint32_t speed_hz) {
  (void)i2c;
  return speed_hz ? ESY_OK : ESY_ERR_INVALID_ARG;
}

esy_err_t esy_hb_i2c_abort(esy_i2c_t *i2c) {
  (void)i2c;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_recover_bus(esy_i2c_t *i2c) {
  (void)i2c;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_get_caps(const esy_i2c_t *i2c, uint32_t *out_caps) {
  (void)i2c;
  if (!out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_I2C_CAP_MASTER_TRANSFER | ESY_I2C_CAP_REPEATED_START;
  return ESY_OK;
}

esy_err_t esy_hb_i2c_get_phys_index(const esy_i2c_t *i2c,
                                        uint8_t *out_index) {
  TEST_ASSERT_NOT_NULL(i2c);
  TEST_ASSERT_NOT_NULL(out_index);
  s_state.driver_get_index_order = next_event();
  *out_index = i2c->backend.bytes[0];
  return ESY_OK;
}

esy_err_t esy_hb_pwm_check_available(uint8_t phys_index) {
  (void)phys_index;
  ++s_state.pwm_available_calls;
  s_state.pwm_available_order = next_event();
  return s_state.pwm_available_result;
}

esy_err_t esy_hb_pwm_init(esy_pwm_t *pwm, const esy_pwm_config_t *config) {
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  TEST_ASSERT_NOT_NULL(pwm);
  TEST_ASSERT_NOT_NULL(config);
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  pwm->ctx = (void *)(uintptr_t)config->phys_index;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_deinit(esy_pwm_t *pwm) {
  TEST_ASSERT_NOT_NULL(pwm);
  s_state.driver_deinit_order = next_event();
  return s_state.pwm_deinit_result;
}

esy_err_t esy_hb_pwm_abort_dma(esy_pwm_t *pwm) {
  TEST_ASSERT_NOT_NULL(pwm);
  s_state.pwm_abort_calls++;
  return s_state.pwm_abort_result;
}

esy_err_t esy_hb_pwm_poll(esy_pwm_t *pwm) {
  TEST_ASSERT_NOT_NULL(pwm);
  s_state.pwm_poll_calls++;
  return s_state.pwm_poll_result;
}

esy_err_t esy_hb_pwm_set_duty(esy_pwm_t *pwm, float duty_cycle) {
  TEST_ASSERT_NOT_NULL(pwm);
  s_state.driver_write_calls++;
  return (duty_cycle >= 0.0f && duty_cycle <= 1.0f) ? ESY_OK
                                                    : ESY_ERR_INVALID_ARG;
}

esy_err_t esy_hb_pwm_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz) {
  (void)pwm;
  return freq_hz ? ESY_OK : ESY_ERR_INVALID_ARG;
}

esy_err_t esy_hb_pwm_enable(esy_pwm_t *pwm, bool enable) {
  (void)pwm;
  (void)enable;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_set_polarity(esy_pwm_t *pwm, bool start_low) {
  (void)pwm;
  (void)start_low;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_enable_trigger_output(esy_pwm_t *pwm, bool enabled) {
  (void)pwm;
  (void)enabled;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_dma_output(esy_pwm_t *pwm,
                                    esy_dma_config_t *dma_config, bool loop) {
  (void)loop;
  TEST_ASSERT_NOT_NULL(pwm);
  TEST_ASSERT_NOT_NULL(dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_pwm_inner = dma_config->inner;
  s_state.submitted_dma = *dma_config;
  if (s_state.pwm_dma_complete_inline && dma_config->callback != NULL)
    dma_config->callback(dma_config->callback_arg);
  return s_state.pwm_dma_result;
}

esy_err_t esy_hb_pwm_set_wrap_handler(esy_pwm_t *pwm,
                                          void (*handler)(void *), void *arg,
                                          uint8_t irq_priority) {
  (void)handler;
  (void)arg;
  TEST_ASSERT_NOT_NULL(pwm);
  s_state.driver_set_wrap_handler_calls++;
  s_state.last_irq_priority = irq_priority;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_suspend(esy_pwm_t *pwm) {
  (void)pwm;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_resume(esy_pwm_t *pwm) {
  (void)pwm;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_set_low_power_mode(esy_pwm_t *pwm, bool enable_lprun) {
  (void)pwm;
  (void)enable_lprun;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_set_clock_source(esy_pwm_t *pwm,
                                          uint8_t clock_source) {
  (void)pwm;
  (void)clock_source;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_get_caps(const esy_pwm_t *pwm, uint32_t *out_caps) {
  (void)pwm;
  if (!out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_PWM_CAP_DMA_OUTPUT | ESY_PWM_CAP_WRAP_IRQ | ESY_PWM_CAP_POLARITY;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_get_phys_index(const esy_pwm_t *pwm,
                                        uint8_t *out_index) {
  TEST_ASSERT_NOT_NULL(pwm);
  TEST_ASSERT_NOT_NULL(out_index);
  s_state.driver_get_index_order = next_event();
  *out_index = (uint8_t)(uintptr_t)pwm->ctx;
  return ESY_OK;
}

void esy_hb_pwm_handle_irq(uint32_t index) { (void)index; }

esy_err_t esy_hb_adc_check_available(uint8_t phys_index) {
  if (phys_index >= TEST_DMA_RESOURCE_COUNT) return ESY_ERR_INVALID_ARG;
  ++s_state.adc_available_calls;
  s_state.adc_available_order = next_event();
  return s_state.adc_owners[phys_index] != NULL ? ESY_ERR_BUSY : ESY_OK;
}

esy_err_t esy_hb_adc_init(esy_adc_t *adc, const esy_adc_config_t *config) {
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  TEST_ASSERT_NOT_NULL(adc);
  TEST_ASSERT_NOT_NULL(config);
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  TEST_ASSERT_LESS_THAN_UINT32(TEST_DMA_RESOURCE_COUNT, config->phys_index);
  if (s_state.adc_owners[config->phys_index] != NULL) return ESY_ERR_BUSY;
  s_state.adc_owners[config->phys_index] = adc;
  adc->ctx = (void *)(uintptr_t)config->phys_index;
  return ESY_OK;
}

esy_err_t esy_hb_adc_deinit(esy_adc_t *adc) {
  TEST_ASSERT_NOT_NULL(adc);
  s_state.driver_deinit_order = next_event();
  if (s_state.adc_deinit_result != ESY_OK) return s_state.adc_deinit_result;
  for (uint32_t i = 0u; i < TEST_DMA_RESOURCE_COUNT; ++i)
    if (s_state.adc_owners[i] == adc) s_state.adc_owners[i] = NULL;
  adc->ctx = NULL;
  return ESY_OK;
}

esy_err_t esy_hb_adc_abort_dma(esy_adc_t *adc) {
  TEST_ASSERT_NOT_NULL(adc);
  ++s_state.adc_abort_calls;
  if (s_state.adc_abort_result == ESY_OK) s_adc_busy = false;
  return s_state.adc_abort_result;
}

int32_t esy_hb_adc_sample_once(esy_adc_t *adc) {
  TEST_ASSERT_NOT_NULL(adc);
  s_state.driver_sample_calls++;
  return 1234;
}

esy_err_t esy_hb_adc_transfer_dma(esy_adc_t *adc,
                                      esy_dma_config_t *dma_config) {
  TEST_ASSERT_NOT_NULL(adc);
  TEST_ASSERT_NOT_NULL(dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_adc_inner = dma_config->inner;
  s_state.submitted_dma = *dma_config;
  return s_state.adc_submit_result;
}

bool esy_hb_adc_is_dma_busy(const esy_adc_t *adc) {
  TEST_ASSERT_NOT_NULL(adc);
  return s_adc_busy;
}

esy_err_t esy_hb_adc_transfer_dma_multi_channel(
    esy_adc_t *adc, esy_dma_config_t *dma_config, const uint8_t *index_list,
    uint8_t num_indexes) {
  TEST_ASSERT_NOT_NULL(adc);
  TEST_ASSERT_NOT_NULL(dma_config);
  TEST_ASSERT_NOT_NULL(index_list);
  TEST_ASSERT_NOT_EQUAL_UINT8(0u, num_indexes);
  s_state.driver_transfer_dma_calls++;
  s_state.last_adc_inner = dma_config->inner;
  s_state.submitted_dma = *dma_config;
  return s_state.adc_submit_result;
}

esy_err_t esy_hb_adc_start_continuous_dma(esy_adc_t *adc,
                                              esy_dma_config_t *dma_config) {
  TEST_ASSERT_NOT_NULL(adc);
  TEST_ASSERT_NOT_NULL(dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_adc_inner = dma_config->inner;
  s_state.submitted_dma = *dma_config;
  return s_state.adc_submit_result;
}

void esy_hb_adc_handle_irq(void) {}

esy_err_t esy_hb_adc_set_trigger_source(esy_adc_t *adc,
                                         esy_adc_trigger_edge_t edge,
                                         esy_adc_trigger_source_t source) {
  TEST_ASSERT_NOT_NULL(adc);
  s_state.last_adc_trigger_edge = edge;
  s_state.last_adc_trigger_source = source;
  return ESY_OK;
}

esy_err_t esy_hb_adc_get_caps(const esy_adc_t *adc, uint32_t *out_caps) {
  TEST_ASSERT_NOT_NULL(adc);
  if (!out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_ADC_CAP_DMA | ESY_ADC_CAP_SCAN | ESY_ADC_CAP_EXTERNAL_TRIGGER |
              ESY_ADC_CAP_CIRCULAR_DMA;
  return ESY_OK;
}

esy_err_t esy_hb_adc_get_phys_index(const esy_adc_t *adc,
                                        uint8_t *out_index) {
  TEST_ASSERT_NOT_NULL(adc);
  TEST_ASSERT_NOT_NULL(out_index);
  s_state.driver_get_index_order = next_event();
  *out_index = (uint8_t)(uintptr_t)adc->ctx;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_claim(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *config) {
  if (cap == NULL || config == NULL || config->inner.gpio == ESY_PIN_UNUSED)
    return ESY_ERR_INVALID_ARG;
  if (cap->ctx != NULL) {
    return s_state.capture_owner == cap &&
                   cap->ctx == cap->backend.bytes && !s_state.capture_enabled &&
                   cap->backend.bytes[0] == config->phys_index
               ? ESY_OK : ESY_ERR_STATE;
  }
  if (s_state.capture_owner != NULL) return ESY_ERR_BUSY;
  s_state.capture_owner = cap;
  s_state.capture_claim_order = next_event();
  cap->backend.bytes[0] = config->phys_index;
  cap->ctx = cap->backend.bytes;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_init(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *config) {
  TEST_ASSERT_NOT_NULL(cap);
  TEST_ASSERT_NOT_NULL(config);
  TEST_ASSERT_EQUAL_PTR(cap, s_state.capture_owner);
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  s_state.capture_enabled = true;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_suspend(esy_timer_capture_t *cap) {
  TEST_ASSERT_NOT_NULL(cap);
  TEST_ASSERT_EQUAL_PTR(cap, s_state.capture_owner);
  s_state.driver_deinit_order = next_event();
  s_state.capture_enabled = false;
  if (s_state.capture_cleanup_result != ESY_OK)
    return s_state.capture_cleanup_result;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_deinit(esy_timer_capture_t *cap) {
  TEST_ASSERT_NOT_NULL(cap);
  TEST_ASSERT_EQUAL_PTR(cap, s_state.capture_owner);
  TEST_ASSERT_FALSE(s_state.capture_enabled);
  s_state.capture_release_order = next_event();
  s_state.capture_owner = NULL;
  cap->ctx = NULL;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_get_value(const esy_timer_capture_t *cap,
                                             uint32_t *value) {
  TEST_ASSERT_NOT_NULL(cap);
  if (!value) return ESY_ERR_INVALID_ARG;
  if (s_state.capture_owner != cap || !s_state.capture_enabled)
    return ESY_ERR_STATE;
  *value = 100u;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_get_interval(const esy_timer_capture_t *cap,
                                                uint32_t *interval) {
  TEST_ASSERT_NOT_NULL(cap);
  if (!interval) return ESY_ERR_INVALID_ARG;
  if (s_state.capture_owner != cap || !s_state.capture_enabled)
    return ESY_ERR_STATE;
  *interval = 25u;
  return ESY_OK;
}

void esy_hb_timer_capture_handle_irq(uint32_t index) {
  s_state.last_dreq = index;
}

esy_err_t esy_hb_timer_capture_get_caps(const esy_timer_capture_t *cap,
                                            uint32_t *out_caps) {
  TEST_ASSERT_NOT_NULL(cap);
  if (!out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_TIMER_CAPTURE_CAP_EDGE_CALLBACK | ESY_TIMER_CAPTURE_CAP_INTERVAL;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_get_phys_index(
    const esy_timer_capture_t *cap, uint8_t *out_index) {
  TEST_ASSERT_NOT_NULL(cap);
  TEST_ASSERT_NOT_NULL(out_index);
  s_state.driver_get_index_order = next_event();
  if (s_state.capture_owner != cap || cap->ctx != cap->backend.bytes)
    return ESY_ERR_STATE;
  *out_index = cap->backend.bytes[0];
  return ESY_OK;
}

void esy_hb_usb_dev_board_init(void) {
  s_state.board_configure_order = next_event();
}

esy_err_t esy_hb_usb_dev_init(esy_usb_dev_t *dev,
                                  const esy_usb_dev_config_t *cfg) {
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  TEST_ASSERT_NOT_NULL(dev);
  TEST_ASSERT_NOT_NULL(cfg);
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  dev->ctx = (void *)(uintptr_t)cfg->phys_index;
  return ESY_OK;
}

void esy_hb_usb_dev_deinit(esy_usb_dev_t *dev) {
  TEST_ASSERT_NOT_NULL(dev);
  s_state.driver_deinit_order = next_event();
}

esy_err_t esy_hb_usb_dev_get_caps(const esy_usb_dev_t *dev,
                                      uint32_t *out_caps) {
  TEST_ASSERT_NOT_NULL(dev);
  if (!out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_USB_CAP_DEVICE | ESY_USB_CAP_DMA;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_get_phys_index(const esy_usb_dev_t *dev,
                                            uint8_t *out_index) {
  TEST_ASSERT_NOT_NULL(dev);
  TEST_ASSERT_NOT_NULL(out_index);
  s_state.driver_get_index_order = next_event();
  *out_index = (uint8_t)(uintptr_t)dev->ctx;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_set_address(esy_usb_dev_t *dev, uint8_t addr) {
  TEST_ASSERT_NOT_NULL(dev);
  s_state.driver_write_calls++;
  s_state.last_usb_addr = addr;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_ep_init(esy_usb_dev_t *dev,
                                     const esy_usb_ep_cfg_t *cfg) {
  TEST_ASSERT_NOT_NULL(dev);
  TEST_ASSERT_NOT_NULL(cfg);
  s_state.driver_init_calls++;
  s_state.last_usb_ep_cfg = *cfg;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_ep_deinit(esy_usb_dev_t *dev, uint8_t ep_addr) {
  TEST_ASSERT_NOT_NULL(dev);
  s_state.driver_deinit_order = next_event();
  s_state.last_usb_ep_addr = ep_addr;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_ep_stall(esy_usb_dev_t *dev, uint8_t ep_addr,
                                      bool stall) {
  TEST_ASSERT_NOT_NULL(dev);
  s_state.last_usb_ep_addr = ep_addr;
  s_state.last_bool_arg = stall;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_ep_xfer(esy_usb_dev_t *dev, uint8_t ep_addr,
                                     esy_usb_xfer_t *xfer) {
  TEST_ASSERT_NOT_NULL(dev);
  TEST_ASSERT_NOT_NULL(xfer);
  s_state.driver_transfer_dma_calls++;
  s_state.last_usb_ep_addr = ep_addr;
  s_state.last_usb_len = (uint16_t)xfer->len;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_ep_cancel(esy_usb_dev_t *dev, uint8_t ep_addr) {
  TEST_ASSERT_NOT_NULL(dev);
  s_state.last_usb_ep_addr = ep_addr;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_ep0_read(esy_usb_dev_t *dev, uint8_t *buf,
                                      uint16_t len) {
  TEST_ASSERT_NOT_NULL(dev);
  if (!buf && len) return ESY_ERR_INVALID_ARG;
  s_state.driver_read_calls++;
  s_state.last_usb_len = len;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_ep0_write(esy_usb_dev_t *dev, const uint8_t *buf,
                                       uint16_t len) {
  TEST_ASSERT_NOT_NULL(dev);
  if (!buf && len) return ESY_ERR_INVALID_ARG;
  s_state.driver_write_calls++;
  s_state.last_usb_len = len;
  return ESY_OK;
}

esy_err_t esy_hb_usb_dev_ep0_status(esy_usb_dev_t *dev) {
  TEST_ASSERT_NOT_NULL(dev);
  s_state.driver_start_calls++;
  return ESY_OK;
}

esy_err_t esy_hb_xio_spi_init(esy_spi_t *spi,
                                  const esy_xio_spi_config_t *cfg) {
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  TEST_ASSERT_NOT_NULL(spi);
  TEST_ASSERT_NOT_NULL(cfg);
  s_state.driver_init_order = next_event();
  s_state.driver_init_calls++;
  s_state.last_xio_spi_config = *cfg;
  return ESY_OK;
}

void esy_hb_xio_spi_deinit(esy_spi_t *spi) {
  TEST_ASSERT_NOT_NULL(spi);
  if (s_dma_busy_override > 0) return;
  s_state.driver_deinit_order = next_event();
}

int32_t esy_hb_xio_spi_write_blocking(esy_spi_t *spi, const uint8_t *tx_buf,
                                          size_t len) {
  TEST_ASSERT_NOT_NULL(spi);
  TEST_ASSERT_NOT_NULL(tx_buf);
  s_state.driver_write_calls++;
  return (int32_t)len;
}

int32_t esy_hb_xio_spi_read_blocking(esy_spi_t *spi, uint8_t *rx_buf,
                                         size_t len) {
  TEST_ASSERT_NOT_NULL(spi);
  TEST_ASSERT_NOT_NULL(rx_buf);
  s_state.driver_read_calls++;
  return (int32_t)len;
}

int32_t esy_hb_xio_spi_transfer_blocking(esy_spi_t *spi,
                                             const uint8_t *tx_buf,
                                             uint8_t *rx_buf, size_t len) {
  (void)tx_buf;
  (void)rx_buf;
  TEST_ASSERT_NOT_NULL(spi);
  return (int32_t)len;
}

esy_err_t esy_hb_xio_spi_write_then_read_blocking(esy_spi_t *spi,
                                                      const uint8_t *tx_buf,
                                                      size_t tx_len,
                                                      uint8_t *rx_buf,
                                                      size_t rx_len) {
  (void)spi;
  (void)tx_buf;
  (void)tx_len;
  (void)rx_buf;
  (void)rx_len;
  return ESY_ERR_NOT_SUPPORTED;
}

int32_t esy_hb_xio_spi_write16_blocking(esy_spi_t *spi,
                                            const uint16_t *tx_buf,
                                            size_t len) {
  (void)tx_buf;
  TEST_ASSERT_NOT_NULL(spi);
  return (int32_t)len;
}

int32_t esy_hb_xio_spi_read16_blocking(esy_spi_t *spi, uint16_t *rx_buf,
                                           size_t len) {
  (void)rx_buf;
  TEST_ASSERT_NOT_NULL(spi);
  return (int32_t)len;
}

int32_t esy_hb_xio_spi_transfer16_blocking(esy_spi_t *spi,
                                               const uint16_t *tx_buf,
                                               uint16_t *rx_buf, size_t len) {
  (void)tx_buf;
  (void)rx_buf;
  TEST_ASSERT_NOT_NULL(spi);
  return (int32_t)len;
}

esy_err_t esy_hb_xio_spi_transfer_dma(esy_spi_t *spi,
                                          esy_dma_config_t *tx_dma_config,
                                          esy_dma_config_t *rx_dma_config,
                                          esy_spi_dma_complete_cb_t complete_cb,
                                          void *user_data) {
  TEST_ASSERT_NOT_NULL(spi);
  TEST_ASSERT_NOT_NULL(tx_dma_config);
  TEST_ASSERT_NOT_NULL(rx_dma_config);
  s_state.driver_transfer_dma_calls++;
  s_state.last_tx_inner = tx_dma_config->inner;
  s_state.last_rx_inner = rx_dma_config->inner;
  (void)complete_cb;
  (void)user_data;
  return ESY_OK;
}

uint8_t esy_hb_xio_spi_get_tx_pin(esy_spi_t *spi) {
  TEST_ASSERT_NOT_NULL(spi);
  return TEST_XIO_TX_PIN;
}

uint8_t esy_hb_xio_spi_get_rx_pin(esy_spi_t *spi) {
  TEST_ASSERT_NOT_NULL(spi);
  return TEST_XIO_RX_PIN;
}

uint8_t esy_hb_xio_spi_get_pin_sck(esy_spi_t *spi) {
  TEST_ASSERT_NOT_NULL(spi);
  return TEST_XIO_SCK_PIN;
}

esy_err_t esy_hb_xio_uart_tx_init(esy_uart_t *uart,
                                      const esy_xio_uart_config_t *cfg) {
  if (s_state.init_result != ESY_OK) return s_state.init_result;
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(cfg);
  s_state.driver_init_calls++;
  uart->ctx[0] = (void *)(uintptr_t)(cfg->pin + 1u);
  return ESY_OK;
}

int32_t esy_hb_xio_uart_tx_write_blocking(esy_uart_t *uart,
                                              const uint8_t *data, size_t len) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(data);
  s_state.driver_write_calls++;
  return (int32_t)len;
}

esy_err_t esy_hb_xio_uart_tx_write_dma(esy_uart_t *uart,
                                           esy_dma_config_t *dma_cfg) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(dma_cfg);
  s_state.driver_transfer_dma_calls++;
  s_state.last_tx_inner = dma_cfg->inner;
  s_state.submitted_dma = *dma_cfg;
  return ESY_OK;
}

uint8_t esy_hb_xio_uart_tx_get_pin(esy_uart_t *uart) {
  return uart && uart->ctx[0] ? (uint8_t)((uintptr_t)uart->ctx[0] - 1u) : ESY_PIN_UNUSED;
}

void platform_test_set_xio_uart_wire_busy(bool busy) {
  s_state.xio_uart_wire_busy = busy;
}

bool esy_hb_xio_uart_tx_is_busy(esy_uart_t *uart) {
  return uart != NULL && uart->ctx[0] != NULL && s_state.xio_uart_wire_busy;
}

void esy_hb_xio_uart_tx_deinit(esy_uart_t *uart) {
  if (uart->ctx[0] != NULL) s_state.driver_deinit_order = next_event();
  uart->ctx[0] = NULL;
}

esy_err_t esy_hb_xio_uart_rx_init(esy_uart_t *uart,
                                      const esy_xio_uart_config_t *cfg) {
  if (s_state.xio_rx_init_result != ESY_OK) return s_state.xio_rx_init_result;
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(cfg);
  s_state.driver_init_calls++;
  uart->ctx[1] = (void *)(uintptr_t)(cfg->pin + 1u);
  return ESY_OK;
}

int32_t esy_hb_xio_uart_rx_read_blocking(esy_uart_t *uart, uint8_t *buf,
                                             size_t len) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(buf);
  s_state.driver_read_calls++;
  return (int32_t)len;
}

esy_err_t esy_hb_xio_uart_rx_read_dma(esy_uart_t *uart,
                                          esy_dma_config_t *dma_cfg) {
  TEST_ASSERT_NOT_NULL(uart);
  TEST_ASSERT_NOT_NULL(dma_cfg);
  s_state.driver_transfer_dma_calls++;
  s_state.last_rx_inner = dma_cfg->inner;
  s_state.submitted_dma = *dma_cfg;
  return ESY_OK;
}

bool esy_hb_xio_uart_rx_is_readable(const esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  return s_state.xio_uart_readable;
}

void platform_test_set_xio_uart_readable(bool readable) {
  s_state.xio_uart_readable = readable;
}

uint8_t esy_hb_xio_uart_rx_get_pin(esy_uart_t *uart) {
  return uart && uart->ctx[1] ? (uint8_t)((uintptr_t)uart->ctx[1] - 1u) : ESY_PIN_UNUSED;
}

void esy_hb_xio_uart_rx_deinit(esy_uart_t *uart) {
  if (uart->ctx[1] != NULL) s_state.driver_deinit_order = next_event();
  uart->ctx[1] = NULL;
}

esy_err_t esy_hb_spi_poll(esy_spi_t *spi) {
  if (s_state.spi_submit_result != ESY_OK && s_spi_dma_complete_cb != NULL)
    platform_test_complete_spi_dma(spi, ESY_SPI_DMA_TRANSFER_DMA_ERROR);
  return ESY_OK;
}
bool esy_hb_spi_is_busy(esy_spi_t *spi) { (void)spi; return s_dma_busy_override > 0; }

void platform_test_reject_dma_mapping(bool reject) { s_state.reject_dma_mapping = reject; }
void platform_test_set_init_result(esy_err_t result) { s_state.init_result = result; }
void platform_test_set_xio_rx_init_result(esy_err_t result) { s_state.xio_rx_init_result = result; }
void platform_test_set_timer_immediate(bool immediate) { s_state.timer_immediate = immediate; }

void platform_test_set_timer_start_behavior(int32_t timer_id, bool inline_callback) {
  s_state.timer_scripted = true;
  s_state.timer_next_id = timer_id;
  s_state.timer_start_inline = inline_callback;
}

void platform_test_set_time_us(uint32_t time_us) { s_state.system_time_us = time_us; }

void platform_test_fire_timer(void) {
  if (s_state.timer_callback != NULL)
    s_state.timer_callback(s_state.timer_callback_arg);
}
esy_err_t esy_hb_xio_spi_poll(esy_spi_t *spi) { (void)spi; return ESY_OK; }
esy_err_t esy_hb_xio_spi_abort(esy_spi_t *spi) {
  (void)spi;
  s_dma_busy_override = 0;
  return ESY_OK;
}
bool esy_hb_xio_spi_is_busy(esy_spi_t *spi) { (void)spi; return s_dma_busy_override > 0; }

void platform_test_set_uart_resume_result(esy_err_t result) { s_state.uart_resume_result = result; }

esy_err_t esy_hb_xio_uart_tx_abort_dma(esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  s_state.dma_abort_calls++;
  return s_state.dma_abort_result;
}
esy_err_t esy_hb_xio_uart_rx_abort_dma(esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  s_state.dma_abort_calls++;
  return s_state.dma_abort_result;
}
esy_err_t esy_hb_xio_uart_tx_poll(esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  s_state.uart_poll_calls++;
  return s_state.uart_poll_result;
}
esy_err_t esy_hb_xio_uart_rx_poll(esy_uart_t *uart) {
  TEST_ASSERT_NOT_NULL(uart);
  s_state.uart_poll_calls++;
  return s_state.uart_poll_result;
}
