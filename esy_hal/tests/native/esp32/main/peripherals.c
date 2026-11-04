/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_gpio.h"
#include "esy_hal/esy_hal_uart.h"
#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_i2c.h"
#include "esy_hal/esy_hal_pwm.h"
#include "esy_hal/esy_hal_timer.h"
#include "esy_hal/esy_hal_timer_capture.h"
#include "esy_hal/esy_hal_watchdog.h"
#include "esy_hal/esy_hal_flash.h"
#include "esy_hal/esy_hal_adc.h"
#include "esy_hal/esy_hal_can.h"
#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/os/esy_os_queue.h"
#include "esy_middleware/os/esy_os_mutex.h"
#include "esy_middleware/os/esy_os_semaphore.h"
#include "esy_middleware/os/esy_os_timer.h"
#include "esy_middleware/os/esy_os_work.h"
#include "esy_middleware/os/esy_os_stream_buffer.h"
#include "esy_middleware/os/esy_os_msg_buffer.h"

#include <stdatomic.h>
#include <assert.h>

static void native_rx_event(esy_uart_t *uart,
    esy_uart_rx_stream_event_t event, void *arg) {
  (void)uart;
  (void)event;
  atomic_store_explicit((atomic_bool *)arg, true, memory_order_release);
}

static void native_gpio_event(uint8_t pin, void *arg) {
  (void)pin;
  atomic_store_explicit((atomic_bool *)arg, true, memory_order_release);
}

static esy_err_t native_uart_dma_rejection_probe(esy_uart_t *uart) {
  uint8_t bytes[4] = {0};
  esy_dma_config_t transfer = {
      .src = bytes, .dst = bytes, .length = sizeof(bytes),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  if (esy_hal_uart_send_dma(uart, &transfer) != ESY_ERR_NOT_SUPPORTED ||
      esy_hal_uart_is_busy(uart, true) ||
      esy_hal_uart_receive_dma(uart, &transfer) != ESY_ERR_NOT_SUPPORTED ||
      esy_hal_uart_is_busy(uart, false) ||
      esy_hal_uart_abort_dma(uart, true) != ESY_ERR_STATE ||
      esy_hal_uart_abort_dma(uart, false) != ESY_ERR_STATE)
    return ESY_ERR_STATE;
  return esy_hal_uart_send_timeout(uart, bytes, sizeof(bytes), 100u) ==
      (int32_t)sizeof(bytes) ? ESY_OK : ESY_ERR_IO;
}

void esy_native_probe_peripherals(void) {
  atomic_bool event_pending;
  atomic_init(&event_pending, false);
  esy_gpio_t gpio = {0};
  esy_gpio_config_t gpio_config;
  esy_uart_t uart = {0};
  esy_uart_config_t uart_config;
  esy_spi_t spi = {0};
  esy_spi_config_t spi_config;
  esy_i2c_t i2c = {0};
  esy_i2c_config_t i2c_config;
  esy_pwm_t pwm = {0};
  esy_pwm_config_t pwm_config;
  esy_timer_capture_t capture = {0};
  esy_timer_capture_config_t capture_config;
  esy_flash_geometry_t geometry;
  esy_adc_t adc = {0};
  esy_adc_config_t adc_config;
  uint32_t adc_caps;
  uint8_t rx_storage[32];
  _Alignas(4) uint8_t spi_tx[4] = {0};
  esy_dma_config_t spi_dma = {
      .src = spi_tx, .length = sizeof(spi_tx),
      .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
  };
  esy_can_t can = {0};
  esy_can_config_t can_config;
  esy_can_filter_t can_filter = {.id = 0x123u, .mask = ESY_CAN_STD_ID_MAX};
  esy_can_frame_t can_frame = {.id = 0x123u, .length = 1u, .data = {0x55u}};
  esy_can_status_t can_status;
  uint32_t can_caps;

  esy_hal_gpio_default_cfg(&gpio_config);
  (void)esy_hal_gpio_init(&gpio, &gpio_config);
  (void)esy_hal_gpio_enable_wakeup(&gpio, ESY_GPIO_IRQ_HIGH_LEVEL,
                                    native_gpio_event, &event_pending);
  (void)esy_hal_gpio_disable_wakeup(&gpio);
  (void)esy_hal_gpio_deinit(&gpio);
  esy_hal_uart_default_cfg(&uart_config);
  uart_config.phys_index = 1u;
  if (esy_hal_uart_init(&uart, &uart_config) == ESY_OK) {
    (void)esy_hal_uart_rx_stream_start(&uart, rx_storage, sizeof(rx_storage), native_rx_event, &event_pending);
    (void)esy_hal_uart_rx_stream_stop(&uart);
    esy_err_t uart_result = native_uart_dma_rejection_probe(&uart);
    assert(uart_result == ESY_OK);
    uart_result = esy_hal_uart_deinit(&uart);
    assert(uart_result == ESY_OK);
    (void)uart_result;
  }
  esy_hal_spi_default_cfg(&spi_config);
  (void)esy_hal_spi_init(&spi, &spi_config);
  if (esy_hal_spi_write_dma(&spi, &spi_dma) == ESY_OK) {
    /* ESP32 public DMA is unsupported; this branch is not entered. */
  }
  (void)esy_hal_spi_abort(&spi);
  (void)esy_hal_spi_deinit(&spi);
  esy_hal_i2c_default_cfg(&i2c_config);
  (void)esy_hal_i2c_init(&i2c, &i2c_config);
  (void)esy_hal_i2c_deinit(&i2c);
  esy_hal_pwm_default_cfg(&pwm_config);
  (void)esy_hal_pwm_init(&pwm, &pwm_config);
  (void)esy_hal_pwm_enable(&pwm, true);
  (void)esy_hal_pwm_set_polarity(&pwm, true);
  (void)esy_hal_pwm_suspend(&pwm);
  (void)esy_hal_pwm_resume(&pwm);
  (void)esy_hal_pwm_deinit(&pwm);
  esy_hal_timer_capture_default_cfg(&capture_config);
  capture_config.phys_index = 4u;
  capture_config.inner.tim_id = 1u;
  capture_config.inner.channel = 1u;
  capture_config.inner.gpio = 26u;
  (void)esy_hal_timer_capture_init(&capture, &capture_config);
  (void)esy_hal_timer_capture_get_caps(&capture, &adc_caps);
  (void)esy_hal_timer_capture_get_value(&capture, &adc_caps);
  (void)esy_hal_timer_capture_get_interval(&capture, &adc_caps);
  (void)esy_hal_timer_capture_deinit(&capture);
  (void)esy_hal_timer_init();
  esy_hal_timer_deinit();
  (void)esy_hal_watchdog_init(1000u, false);
  (void)esy_hal_watchdog_deinit();
  (void)esy_hal_flash_init();
  (void)esy_hal_flash_get_geometry(&geometry);
  esy_hal_flash_deinit();
  esy_hal_adc_default_cfg(&adc_config);
  adc_config.phys_index = 1u;
  adc_config.inner.channel = 0u;
  (void)esy_hal_adc_init(&adc, &adc_config);
  (void)esy_hal_adc_sample_once(&adc);
  (void)esy_hal_adc_get_caps(&adc, &adc_caps);
  (void)esy_hal_adc_set_trigger(&adc, ESY_ADC_TRIGGER_EDGE_NONE, ESY_ADC_TRIGGER_SRC_SOFTWARE);
  (void)esy_hal_adc_deinit(&adc);

  esy_hal_can_default_cfg(&can_config);
  can_config.phys_index = 1u;
  can_config.port.tx_pin = 26u;
  can_config.port.rx_pin = 27u;
  can_config.nominal.prescaler = 10u;
  if (esy_hal_can_init(&can, &can_config) == ESY_OK) {
    (void)esy_hal_can_set_filters(&can, &can_filter, 1u);
    (void)esy_hal_can_get_caps(&can, &can_caps);
    (void)esy_hal_can_start(&can);
    (void)esy_hal_can_send(&can, &can_frame);
    (void)esy_hal_can_receive(&can, &can_frame);
    (void)esy_hal_can_get_status(&can, &can_status);
    (void)esy_hal_can_stop(&can);
  }
  (void)esy_hal_can_deinit(&can);

  esy_os_queue_delete(NULL);
  esy_os_mutex_delete(NULL);
  esy_os_semaphore_delete(NULL);
  (void)esy_os_timer_delete(NULL);
  (void)esy_os_work_cancel_sync(NULL);
  esy_os_stream_buffer_delete(NULL);
  esy_os_msg_buffer_delete(NULL);
}
