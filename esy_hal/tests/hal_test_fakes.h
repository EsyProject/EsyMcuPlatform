/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_TESTS_PLATFORM_TEST_FAKES_H
#define ESY_HAL_TESTS_PLATFORM_TEST_FAKES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/peripheral/esy_adc_defs.h"
#include "esy_common/defs/board/esy_board_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include "esy_common/defs/peripheral/esy_i2c_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "esy_common/defs/system/esy_system_defs.h"
#include "esy_common/defs/peripheral/esy_timer_defs.h"
#include "esy_common/defs/peripheral/esy_timer_capture_defs.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#include "esy_common/defs/peripheral/esy_usb_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_spi_defs.h"
#include "esy_hal/esy_hal_boot.h"
#include "esy_hal_backend/esy_hb_flash.h"

enum {
  TEST_SPI_INDEX = 3u,
  TEST_UART_INDEX = 4u,
  TEST_I2C_INDEX = 5u,
  TEST_I2C_SDA_PIN = 18u,
  TEST_I2C_SCL_PIN = 19u,
  TEST_PWM_INDEX = 6u,
  TEST_ADC_INDEX = 7u,
  TEST_TIMER_CAPTURE_INDEX = 8u,
  TEST_USB_INDEX = 9u,
  TEST_GPIO_PIN = 21u,
  TEST_XIO_SCK_PIN = 10u,
  TEST_XIO_TX_PIN = 11u,
  TEST_XIO_RX_PIN = 12u,
  TEST_TX_DREQ = 0x31u,
  TEST_RX_DREQ = 0x42u,
  TEST_PWM_DREQ = 0x53u,
  TEST_ADC_DREQ = 0x64u,
  TEST_TX_CHANNEL = 5u,
  TEST_RX_CHANNEL = 6u,
  TEST_PWM_CHANNEL = 7u,
  TEST_ADC_CHANNEL = 8u,
  TEST_DMA_IRQ_INDEX = 1u,
  TEST_DMA_IRQ_PRIORITY = 4u,
  TEST_GPIO_IRQ_PRIORITY = 6u,
  TEST_PWM_IRQ_PRIORITY = 9u,
  TEST_UART_IRQ_PRIORITY = 0x80u,
  TEST_DMA_RESOURCE_COUNT = 32u,
};

typedef struct {
  esy_adc_t *adc_owners[TEST_DMA_RESOURCE_COUNT];
  uint32_t adc_available_order;
  uint32_t adc_available_calls;
  esy_gpio_irq_config_t gpio_irq_config;
  bool gpio_irq_attached;
  bool timer_scripted;
  bool timer_start_inline;
  int32_t timer_next_id;
  uint32_t timer_active_mask;
  esy_i2c_t *i2c_owner;
  bool i2c_enabled;
  bool i2c_busy;
  uint16_t last_i2c_address;
  bool xio_uart_wire_busy;
  bool xio_uart_readable;
  bool uart_tx_dma_busy;
  bool uart_rx_dma_busy;
  bool uart_wire_busy;
  esy_err_t i2c_cleanup_result;
  uint32_t resumed_speed_hz;
  esy_dma_config_t submitted_dma;
  esy_dma_config_t last_spi_tx_dma;
  esy_dma_config_t last_spi_rx_dma;
  bool reject_dma_mapping;
  bool dma_resource_uses_dreq;
  esy_err_t dma_resource_result;
  esy_err_t dma_start_result;
  esy_err_t dma_abort_result;
  esy_err_t dma_control_result;
  const void *dma_owners[TEST_DMA_RESOURCE_COUNT];
  const void *last_dma_owner;
  uint32_t dma_abort_calls;
  uint32_t dma_control_calls;
  esy_err_t init_result;
  esy_err_t uart_validate_result;
  esy_err_t i2c_slave_validate_result;
  esy_err_t gpio_wakeup_enable_result;
  esy_err_t gpio_wakeup_disable_result;
  bool gpio_wakeup_retain_on_failure;
  esy_spi_t *spi_owner;
  bool spi_enabled;
  esy_err_t spi_claim_result;
  esy_err_t spi_cleanup_result;
  uint32_t spi_claim_order;
  uint32_t spi_release_order;
  uint32_t spi_abort_calls;
  esy_err_t spi_submit_result;
  esy_err_t adc_submit_result;
  esy_err_t adc_abort_result;
  esy_err_t adc_deinit_result;
  uint32_t adc_abort_calls;
  esy_adc_trigger_edge_t last_adc_trigger_edge;
  esy_adc_trigger_source_t last_adc_trigger_source;
  esy_err_t pwm_deinit_result;
  esy_err_t pwm_abort_result;
  esy_err_t pwm_dma_result;
  esy_err_t pwm_poll_result;
  uint32_t pwm_abort_calls;
  uint32_t pwm_poll_calls;
  esy_err_t pwm_available_result;
  uint32_t pwm_available_calls;
  uint32_t pwm_available_order;
  bool pwm_dma_complete_inline;
  esy_uart_t *uart_owner;
  bool uart_enabled;
  esy_err_t uart_cleanup_result;
  uint32_t uart_claim_order;
  uint32_t uart_release_order;
  esy_timer_capture_t *capture_owner;
  bool capture_enabled;
  bool capture_reject_pin;
  esy_err_t capture_cleanup_result;
  uint32_t capture_claim_order;
  uint32_t capture_release_order;
  esy_err_t uart_suspend_result;
  esy_err_t uart_resume_result;
  esy_err_t uart_abort_result;
  esy_err_t uart_poll_result;
  uint32_t uart_abort_calls;
  uint32_t uart_poll_calls;
  bool uart_abort_is_tx;
  esy_err_t uart_stream_start_result;
  esy_err_t uart_stream_stop_result;
  bool uart_stream_retain_on_failure;
  uint32_t uart_stream_start_calls;
  uint32_t uart_stream_stop_calls;
  esy_err_t xio_rx_init_result;
  bool timer_immediate;
  uint32_t system_time_us;
  esy_timer_callback_t timer_callback;
  void *timer_callback_arg;
  uint32_t board_configure_order;
  uint32_t driver_init_order;
  uint32_t driver_get_index_order;
  uint32_t board_reset_order;
  uint32_t board_pin_state_order;
  uint32_t driver_deinit_order;
  uint8_t configure_index;
  esy_spi_role_t configure_role;
  uint8_t reset_index;
  uint32_t driver_init_calls;
  uint32_t driver_transfer_dma_calls;
  uint32_t driver_write_calls;
  uint32_t driver_prepare_calls;
  uint32_t driver_start_calls;
  uint32_t driver_sample_calls;
  uint32_t driver_read_calls;
  uint32_t driver_flash_geometry_calls;
  uint32_t driver_flash_sector_calls;
  esy_err_t driver_flash_geometry_result;
  esy_err_t driver_flash_sector_result;
  uint32_t driver_flash_safe_execute_calls;
  esy_hb_flash_safe_execute_t driver_flash_safe_execute;
  esy_err_t driver_flash_safe_execute_result;
  uint32_t driver_irq_save_calls;
  uint32_t driver_irq_restore_calls;
  uint32_t driver_tick_from_isr_calls;
  uint32_t driver_random_calls;
  esy_err_t driver_random_result;
  uint8_t *driver_random_buffer;
  size_t driver_random_length;
  uint32_t driver_memory_barrier_calls;
  uint32_t driver_signal_event_calls;
  uint32_t driver_wait_for_event_calls;
  uint32_t driver_attach_irq_calls;
  uint32_t driver_disable_wakeup_calls;
  uint32_t driver_set_wrap_handler_calls;
  uint32_t last_channel_mask;
  uint32_t last_dreq;
  uint32_t last_count;
  uint32_t last_address;
  uint32_t last_irq_status;
  size_t last_length;
  uint8_t last_channel;
  uint8_t last_pin;
  uint8_t last_core_id;
  uint8_t last_usb_addr;
  uint8_t last_usb_ep_addr;
  uint8_t last_xio_pin;
  uint16_t last_usb_len;
  bool last_gpio_value;
  bool last_pause_on_debug;
  bool last_bool_arg;
  bool boot_layout_unavailable;
  esy_boot_runtime_t boot_runtime;
  esy_err_t boot_runtime_result;
  esy_err_t boot_layout_result;
  esy_err_t boot_pending_result;
  esy_err_t boot_confirm_result;
  uint32_t boot_layout_validate_calls;
  uint32_t boot_pending_calls;
  uint32_t boot_confirm_calls;
  esy_err_t low_power_result;
  esy_err_t retention_restore_result;
  uint32_t low_power_supported_mode_mask;
  uint64_t always_on_time_us;
  uint64_t low_power_elapsed_us;
  bool always_on_supported;
  bool retention_supported;
  uint32_t retention_prepare_calls;
  uint32_t retention_restore_calls;
  uint32_t retention_cancel_calls;
  uint32_t shutdown_calls;
  uint64_t wakeup_timer_armed_time_us;
  uint32_t wakeup_timer_prepare_calls;
  uint32_t wakeup_timer_capture_calls;
  uint32_t wakeup_timer_finish_calls;
  uint32_t wakeup_timer_abort_calls;
  uint8_t wakeup_timer_alarm_index;
  bool wakeup_timer_expired;
  esy_err_t pin_state_result;
  esy_board_controller_pin_state_t last_controller_pin_state;
  uint8_t last_irq_priority;
  esy_timer_source_t last_timer_source;
  esy_gpio_mode_t last_gpio_mode;
  esy_gpio_irq_mode_t last_gpio_irq_mode;
  esy_gpio_pull_t last_gpio_pull;
  esy_dma_inner_config_t last_tx_inner;
  esy_dma_inner_config_t last_rx_inner;
  esy_dma_inner_config_t last_pwm_inner;
  esy_dma_inner_config_t last_adc_inner;
  esy_dma_inner_config_t last_dma_inner;
  esy_adc_inner_config_t last_adc_config;
  esy_i2c_inner_config_t last_i2c_inner;
  esy_timer_capture_inner_config_t last_timer_capture_config;
  esy_xio_inner_config_t last_xio_config;
  esy_xio_spi_config_t last_xio_spi_config;
  esy_usb_ep_cfg_t last_usb_ep_cfg;
  esy_system_config_t last_system_config;
} platform_test_state_t;

extern const platform_test_state_t *platform_test_state(void);
void platform_test_set_random_result(esy_err_t result);
void platform_test_set_uart_stream_results(esy_err_t start_result,
                                           esy_err_t stop_result,
                                           bool retain_on_failure);
void platform_test_reject_dma_mapping(bool reject);
void platform_test_set_init_result(esy_err_t result);
void platform_test_set_uart_validate_result(esy_err_t result);
void platform_test_set_i2c_slave_validate_result(esy_err_t result);
void platform_test_set_gpio_wakeup_results(esy_err_t enable_result,
                                           esy_err_t disable_result,
                                           bool retain_on_failure);
void platform_test_set_uart_resume_result(esy_err_t result);
void platform_test_set_uart_suspend_result(esy_err_t result);
void platform_test_set_uart_dma_results(esy_err_t abort, esy_err_t poll);
void platform_test_set_uart_cleanup_result(esy_err_t result);
void platform_test_set_capture_cleanup_result(esy_err_t result);
void platform_test_set_capture_reject_pin(bool reject);
void platform_test_set_xio_rx_init_result(esy_err_t result);
void platform_test_set_timer_immediate(bool immediate);
void platform_test_set_timer_start_behavior(int32_t timer_id, bool inline_callback);
void platform_test_fire_gpio_irq(void);
void platform_test_set_time_us(uint32_t time_us);
void platform_test_fire_timer(void);
extern void platform_test_reset(void);
extern void platform_test_set_flash_safe_execute_result(esy_err_t result);
extern void platform_test_set_flash_query_results(esy_err_t geometry_result,
                                                  esy_err_t sector_result);
extern void platform_test_reset_order_counters(void);
extern void platform_test_set_dma_busy(bool busy);
void platform_test_set_uart_busy(bool tx_dma, bool rx_dma, bool wire);
void platform_test_set_dma_owner(uint8_t resource, const void *owner);
void platform_test_set_dma_resource_mapping(bool uses_dreq, esy_err_t result);
void platform_test_set_dma_results(esy_err_t start, esy_err_t abort,
                                   esy_err_t control);
void platform_test_half_complete_dma(void);
void platform_test_set_spi_submit_result(esy_err_t result);
void platform_test_set_spi_claim_result(esy_err_t result);
void platform_test_set_spi_cleanup_result(esy_err_t result);
void platform_test_set_pwm_results(esy_err_t deinit, esy_err_t abort,
                                  esy_err_t dma, esy_err_t poll);
void platform_test_set_pwm_dma_complete_inline(bool complete_inline);
void platform_test_set_pwm_available_result(esy_err_t result);
extern void platform_test_set_adc_busy(bool busy);
void platform_test_set_adc_results(esy_err_t submit, esy_err_t abort,
                                   esy_err_t deinit);
void platform_test_set_i2c_cleanup_result(esy_err_t result);
void platform_test_set_i2c_busy(bool busy);
void platform_test_set_xio_uart_wire_busy(bool busy);
void platform_test_set_xio_uart_readable(bool readable);
extern void platform_test_set_low_power_result(esy_err_t result);
extern void platform_test_set_low_power_supported_mode_mask(uint32_t mode_mask);
extern void platform_test_set_retention_support(bool supported,
                                                esy_err_t restore_result);
extern void platform_test_set_always_on_time(uint64_t now_us,
                                             uint64_t elapsed_us,
                                             bool supported);
extern void platform_test_set_wakeup_timer_expired(bool expired);
extern void platform_test_set_pin_state_result(esy_err_t result);
extern void platform_test_set_pc(uintptr_t pc);
extern void platform_test_set_boot_layout_available(bool available);
void platform_test_set_boot_runtime(esy_boot_runtime_t runtime, esy_err_t result);
void platform_test_set_boot_layout_result(esy_err_t result);
void platform_test_set_boot_action_results(esy_err_t pending, esy_err_t confirm);
extern void platform_test_complete_spi_dma(
    esy_spi_t *spi, esy_spi_dma_transfer_status_t status);
extern void platform_test_complete_dma(bool error);

#endif /* ESY_HAL_TESTS_PLATFORM_TEST_FAKES_H */
