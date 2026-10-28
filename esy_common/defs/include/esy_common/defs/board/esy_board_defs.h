/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_BOARD_DEFS_H
#define ESY_BOARD_DEFS_H

/**
 * @file esy_board_defs.h
 * @brief Board-level resource mapping and query interfaces.
 *
 * This file declares board-specific query and configuration APIs used to:
 * - obtain interrupt priorities or IRQ indices for peripherals
 *   (GPIO / PWM / Timer Capture / I2C / DMA, etc.);
 * - query DMA binding information for peripherals (dreq, channel, trigger);
 * - configure and restore board-level pin multiplexing for peripherals.
 *
 * @note
 * These APIs provide *board-level mapping and assembly only*.
 * They do not directly drive hardware peripherals.
 * Each platform must provide its own implementation in the board layer.
 */

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/image/esy_boot_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_spi_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_uart_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "esy_common/defs/peripheral/esy_i2c_defs.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#include "esy_common/defs/peripheral/esy_adc_defs.h"
#include "esy_common/defs/peripheral/esy_pwm_defs.h"
#include "esy_common/defs/peripheral/esy_timer_defs.h"
#include "esy_common/defs/peripheral/esy_timer_capture_defs.h"
#include "esy_common/defs/system/esy_system_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Board Initialization                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Board-owned controller pad states used by device PM adapters.
 *
 * A board implementation may apply alternate-function mux, pull, drive
 * strength, slew rate, and analog mode as required by the SoC.
 */
typedef enum {
  ESY_BOARD_CONTROLLER_PIN_STATE_DEFAULT = 0,
  ESY_BOARD_CONTROLLER_PIN_STATE_IDLE,
  ESY_BOARD_CONTROLLER_PIN_STATE_SLEEP,
  ESY_BOARD_CONTROLLER_PIN_STATE_COUNT
} esy_board_controller_pin_state_t;



/**
 * @brief Initialize board-level resources.
 *
 * This function should be called once during system startup to
 * prepare board-specific mappings and default configurations.
 */
extern void esy_board_init(void);

/**
 * @brief Query the board system initialization configuration.
 *
 * @param[out] config System clock and timing configuration.
 * @retval true  Query succeeded and @p config is filled.
 * @retval false Query failed.
 */
extern bool esy_board_get_system_config(esy_system_config_t* config);

/**
 * @brief Query the board timer task internal configuration.
 *
 * @param[in]  mode   Timer resolution mode.
 * @param[out] config Timer clock source and interrupt priority.
 * @retval true  Query succeeded and @p config is filled.
 * @retval false Query failed.
 */
extern bool esy_board_get_timer_task_inner_config(
    esy_timer_mode_t mode, esy_timer_task_inner_config_t* config);

/**
 * @brief Query the board bootloader flash layout.
 *
 * @param[out] layout Bootloader slot and metadata layout.
 * @retval true  Query succeeded and @p layout is filled.
 * @retval false Query failed.
 */
extern bool esy_board_get_bootloader_layout(esy_bootloader_layout_t* layout);

/* -------------------------------------------------------------------------- */
/* IRQ Information                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get interrupt priority for a GPIO pin.
 *
 * @param[in] pin Logical/physical GPIO pin number (board-defined).
 * @return Interrupt priority value (lower value means higher priority;
 *         priority scheme is platform-defined).
 */
extern uint8_t esy_board_gpio_get_irq_priority(uint32_t pin);

/**
 * @brief Get interrupt priority for a PWM controller instance.
 *
 * @param[in] pwm_index PWM controller index.
 * @return Interrupt priority.
 */
extern uint8_t esy_board_pwm_get_irq_priority(uint32_t pwm_index);

/**
 * @brief Get interrupt priority for a Timer Capture instance.
 *
 * @param[in] timer_capture_index Timer Capture controller index.
 * @return Interrupt priority.
 */
extern uint8_t esy_board_timer_capture_get_irq_priority(
    uint32_t timer_capture_index);

/**
 * @brief Get interrupt priority for an I2C controller instance.
 *
 * @param[in] i2c_index I2C controller index.
 * @return Interrupt priority.
 */
extern uint8_t esy_board_i2c_get_irq_priority(uint8_t i2c_index);

/* -------------------------------------------------------------------------- */
/* DMA Binding Information                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Derive DMA IRQ index from request source and channel.
 *
 * Used for DMA interrupt registration and routing.
 *
 * @param[in] dreq    DMA request source identifier (platform-specific).
 * @param[in] channel DMA channel number (platform-specific).
 * @return DMA IRQ index (platform-specific).
 */
extern uint8_t esy_board_dma_get_irq_index(uint32_t dreq, uint8_t channel);

/**
 * @brief Get DMA interrupt priority for a given request and channel.
 *
 * @param[in] dreq    DMA request source identifier.
 * @param[in] channel DMA channel number.
 * @return Interrupt priority.
 */
extern uint8_t esy_board_dma_get_irq_priority(uint32_t dreq, uint8_t channel);

/**
 * @brief Query SPI TX/RX DMA binding information.
 *
 * @param[in]  spi_index SPI controller index.
 * @param[out] tx_cfg    DMA internal configuration for TX direction.
 * @param[out] rx_cfg    DMA internal configuration for RX direction.
 * @retval true  Query succeeded and @p tx_cfg / @p rx_cfg are filled.
 * @retval false Query failed (invalid index or not configured).
 */
extern bool esy_board_spi_get_dma_info(uint8_t spi_index,
                                       esy_dma_inner_config_t* tx_cfg,
                                       esy_dma_inner_config_t* rx_cfg);

/**
 * @brief Query I2C DMA binding information.
 *
 * @param[in]  i2c_index I2C controller index.
 * @param[out] cfg       DMA internal configuration.
 * @param[in]  is_tx     true for TX direction, false for RX direction.
 * @retval true  Query succeeded and @p cfg is filled.
 * @retval false Query failed.
 */
extern bool esy_board_i2c_get_dma_info(uint8_t i2c_index,
                                       esy_dma_inner_config_t* cfg,
                                       bool is_tx);

/**
 * @brief Query UART TX/RX DMA binding information.
 *
 * @param[in]  uart_index UART controller index.
 * @param[out] cfg        DMA internal configuration.
 * @param[in]  is_tx      true for TX direction, false for RX direction.
 * @retval true  Query succeeded and @p cfg is filled.
 * @retval false Query failed.
 */
extern bool esy_board_uart_get_dma_info(uint8_t uart_index,
                                        esy_dma_inner_config_t* cfg,
                                        bool is_tx);

/**
 * @brief Query PWM DMA binding information.
 *
 * Used for DMA-based PWM updates (e.g. compare value streaming).
 *
 * @param[in]  pwm_index PWM controller index.
 * @param[out] cfg       DMA internal configuration.
 * @retval true  Query succeeded.
 * @retval false Query failed.
 */
extern bool esy_board_pwm_get_dma_info(uint8_t pwm_index,
                                       esy_dma_inner_config_t* cfg);

/**
 * @brief Query ADC DMA binding information.
 *
 * @param[in]  adc_index ADC controller index.
 * @param[out] cfg       DMA internal configuration.
 * @retval true  Query succeeded.
 * @retval false Query failed.
 */
extern bool esy_board_adc_get_dma_info(uint8_t adc_index,
                                       esy_dma_inner_config_t* cfg);

/**
 * @brief Query DMA binding information for XIO (programmable IO subsystem).
 *
 * @param[in]  pin XIO-bound pin number (board-defined).
 * @param[out] cfg DMA internal configuration.
 * @retval true  Query succeeded.
 * @retval false Query failed.
 */
extern bool esy_board_xio_get_dma_info(uint8_t pin,
                                       esy_dma_inner_config_t* cfg);

/* -------------------------------------------------------------------------- */
/* Pin Multiplexing Configuration (Before HAL Backend Initialization)          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Configure an already-reserved Timer Capture input.
 *
 * The HAL claims the configured GPIO/timer/channel before this call. Preserve
 * those resource identities; configure only the declared input. Return false
 * without changing hardware when the board mapping does not match. The HAL
 * resets routing only after successful configuration and native IRQ retirement.
 *
 * @param[in]  timer_capture_index Timer Capture controller index.
 * @param[in]  config              Capture internal configuration
 *                                 (channel, polarity, etc.).
 * @retval true  Configuration succeeded.
 * @retval false Configuration failed (unsupported index or pin).
 */
extern bool esy_board_timer_capture_configure_pin(
    uint8_t timer_capture_index, esy_timer_capture_inner_config_t* config);

/**
 * @brief Restore Timer Capture pin multiplexing to default state.
 *
 * @param[in] timer_capture_index Timer Capture controller index.
 */
extern void esy_board_timer_capture_reset_pin(uint8_t timer_capture_index);

/**
 * @brief Configure output pin and multiplexing for PWM.
 *
 * @param[in] pwm_index PWM controller index.
 * @param[in] config    PWM internal configuration
 *                      (channel mapping, polarity, frequency-related mapping).
 */
extern void esy_board_pwm_configure_pin(uint8_t pwm_index,
                                        esy_pwm_inner_config_t* config);

/**
 * @brief Restore PWM pin multiplexing to default state.
 *
 * @param[in] pwm_index PWM controller index.
 */
extern void esy_board_pwm_reset_pin(uint8_t pwm_index);

/**
 * @brief Configure SPI pins and multiplexing.
 *
 * @param[in] spi_index SPI controller index.
 * @param[in] role      SPI role (master/slave).
 * @param[in] config    SPI internal configuration
 *                      (timing, polarity, data width, pin mapping).
 */
extern void esy_board_spi_configure_pins(uint8_t spi_index,
                                         esy_spi_role_t role,
                                         esy_spi_inner_config_t* config);

/**
 * @brief Restore SPI pin multiplexing to default state.
 *
 * @param[in] spi_index SPI controller index.
 */
extern void esy_board_spi_reset_pins(uint8_t spi_index);

/**
 * @brief Select a board-defined SPI controller pad state.
 *
 * @param[in] spi_index SPI controller index.
 * @param[in] state Target board-owned pad state.
 * @retval ESY_OK State selected.
 * @retval ESY_ERR_NOT_SUPPORTED Controller or state is unsupported.
 * @note An error must leave the previous pad state unchanged.
 */
extern esy_err_t esy_board_spi_select_pin_state(
    uint8_t spi_index, esy_board_controller_pin_state_t state);

/**
 * @brief Configure UART pins and complete board-owned controller metadata.
 *
 * @param[in] uart_index UART controller index.
 * @param[in,out] config Non-NULL UART internal configuration. Board ports may
 *                       replace the IRQ priority with their controller policy;
 *                       the HAL forwards the completed configuration to
 *                       the HAL backend before RX streaming starts. The priority
 *                       must permit any RTOS FromISR calls made by RX callbacks.
 * @note Called in initialization/task context, not from an ISR. The board does
 *       not retain config. Priority encoding is platform-specific; zero is an
 *       actual priority, not a request for automatic default selection.
 */
extern void esy_board_uart_configure_pins(uint8_t uart_index,
                                          esy_uart_inner_config_t* config);

/**
 * @brief Restore UART pin multiplexing to default state.
 *
 * @param[in] uart_index UART controller index.
 */
extern void esy_board_uart_reset_pins(uint8_t uart_index);

/**
 * @brief Select a board-defined UART controller pad state.
 *
 * @param[in] uart_index UART controller index.
 * @param[in] state Target board-owned pad state.
 * An error must leave the previous pad state unchanged. Wake-on-RX is a
 * separate controller capability and is not inferred from a pad state.
 */
extern esy_err_t esy_board_uart_select_pin_state(
    uint8_t uart_index, esy_board_controller_pin_state_t state);

/**
 * @brief Configure I2C pins and multiplexing.
 *
 * @param[in] i2c_index I2C controller index.
 * @param[in] role      I2C role (master/slave).
 * @param[in,out] config I2C internal configuration. HAL initializes sda_pin and
 * scl_pin to ESY_PIN_UNUSED before this call. An ESP32 board must fill distinct,
 * output-capable physical GPIO numbers for both roles; leaving either unused
 * makes backend init fail with ESY_ERR_INVALID_ARG. Other ports may retain
 * their existing fixed pin setup and ignore these fields.
 */
extern void esy_board_i2c_configure_pins(uint8_t i2c_index,
                                         esy_i2c_role_t role,
                                         esy_i2c_inner_config_t* config);

/**
 * @brief Restore I2C pin multiplexing to default state.
 *
 * @param[in] i2c_index I2C controller index.
 */
extern void esy_board_i2c_reset_pins(uint8_t i2c_index);

/**
 * @brief Select a board-defined I2C controller pad state.
 *
 * @param[in] i2c_index I2C controller index.
 * @param[in] state Target board-owned pad state.
 * @retval ESY_OK State selected.
 * @retval ESY_ERR_NOT_SUPPORTED Controller or state is unsupported.
 * @note An error must leave the previous pad state unchanged.
 */
extern esy_err_t esy_board_i2c_select_pin_state(
    uint8_t i2c_index, esy_board_controller_pin_state_t state);

/**
 * @brief Configure analog input pin for ADC.
 *
 * @param[in] adc_index ADC controller index.
 * @param[in] config    ADC internal configuration (channel mapping).
 */
extern void esy_board_adc_configure_pin(uint8_t adc_index,
                                        esy_adc_inner_config_t* config);

/**
 * @brief Restore ADC analog pin multiplexing to default state.
 *
 * @param[in] adc_index ADC controller index.
 */
extern void esy_board_adc_reset_pin(uint8_t adc_index);

/**
 * @brief Configure pins and multiplexing for XIO (programmable IO).
 *
 * @param[in] pin    XIO-bound pin number (board-defined).
 * @param[in] config XIO internal configuration.
 */
extern void esy_board_xio_configure_pins(uint8_t pin,
                                         esy_xio_inner_config_t* config);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_BOARD_DEFS_H */
