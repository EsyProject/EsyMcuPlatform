/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_board/esy_board.h"
#include "driver/gpio.h"
#include "driver/uart.h"

uint8_t esy_board_gpio_get_irq_priority(uint32_t pin) { (void)pin; return 1u; }
uint8_t esy_board_pwm_get_irq_priority(uint32_t index) { (void)index; return 1u; }
uint8_t esy_board_timer_capture_get_irq_priority(uint32_t index) { (void)index; return 1u; }

bool esy_board_uart_get_dma_info(uint8_t index, esy_dma_inner_config_t *config, bool tx) {
  (void)index; (void)config; (void)tx; return false;
}
bool esy_board_spi_get_dma_info(uint8_t index, esy_dma_inner_config_t *tx,
                                esy_dma_inner_config_t *rx) {
  if (index != 0u) return false;
  /* The native SPI provider owns DMA selection; no generic channel is borrowed. */
  if (tx != NULL) *tx = (esy_dma_inner_config_t){0};
  if (rx != NULL) *rx = (esy_dma_inner_config_t){0};
  return true;
}
bool esy_board_i2c_get_dma_info(uint8_t index, esy_dma_inner_config_t *config, bool tx) {
  (void)index; (void)config; (void)tx; return false;
}
bool esy_board_pwm_get_dma_info(uint8_t index, esy_dma_inner_config_t *config) {
  (void)index; (void)config; return false;
}
bool esy_board_adc_get_dma_info(uint8_t index, esy_dma_inner_config_t *config) {
  (void)index; (void)config; return false;
}
void esy_board_adc_configure_pin(uint8_t index, esy_adc_inner_config_t *config) {
  (void)index; (void)config;
}
void esy_board_adc_reset_pin(uint8_t index) { (void)index; }

void esy_board_uart_configure_pins(uint8_t index, esy_uart_inner_config_t *config) {
  if (config == NULL || index != 1u) return;
  config->irq_priority = 1u;
  (void)uart_set_pin(UART_NUM_1, 17, 16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}
void esy_board_uart_reset_pins(uint8_t index) {
  if (index != 1u) return;
  (void)gpio_reset_pin(GPIO_NUM_17);
  (void)gpio_reset_pin(GPIO_NUM_16);
}
void esy_board_spi_configure_pins(uint8_t index, esy_spi_role_t role,
                                  esy_spi_inner_config_t *config) {
  (void)index; (void)role;
  if (config != NULL) config->cs_pin = 5u;
}
void esy_board_spi_reset_pins(uint8_t index) { (void)index; }
void esy_board_i2c_configure_pins(uint8_t index, esy_i2c_role_t role,
                                  esy_i2c_inner_config_t *config) {
  (void)index; (void)role;
  if (config == NULL) return;
  config->sda_pin = 21u;
  config->scl_pin = 22u;
}
void esy_board_i2c_reset_pins(uint8_t index) { (void)index; }
void esy_board_pwm_configure_pin(uint8_t index, esy_pwm_inner_config_t *config) {
  if (config == NULL) return;
  config->gpio = 25u;
  config->channel = index;
  config->tim_id = 0u;
}
void esy_board_pwm_reset_pin(uint8_t index) { (void)index; }
bool esy_board_timer_capture_configure_pin(uint8_t index,
    esy_timer_capture_inner_config_t *config) {
  (void)index;
  return config != NULL && config->gpio == 26u;
}
void esy_board_timer_capture_reset_pin(uint8_t index) { (void)index; }

esy_err_t esy_board_uart_select_pin_state(uint8_t index, esy_board_controller_pin_state_t state) {
  (void)index; (void)state; return ESY_ERR_NOT_SUPPORTED;
}
esy_err_t esy_board_spi_select_pin_state(uint8_t index, esy_board_controller_pin_state_t state) {
  (void)index; (void)state; return ESY_ERR_NOT_SUPPORTED;
}
esy_err_t esy_board_i2c_select_pin_state(uint8_t index, esy_board_controller_pin_state_t state) {
  (void)index; (void)state; return ESY_ERR_NOT_SUPPORTED;
}
