/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_INPUT_GPIO_H
#define ESY_INPUT_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include "esy_middleware/input/esy_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_input_gpio.h
 * @brief Optional polling providers borrowing initialized public HAL GPIOs.
 *
 * These synchronous adapters neither initialize nor reconfigure, register IRQs
 * for, or deinitialize GPIOs. Their owner keeps each borrowed handle alive and
 * serializes polling with both input and GPIO lifecycle/PM changes. Calls are
 * main-loop/task-context only, never ISR-safe. Each poll performs one or two HAL
 * reads and one core sample, so its latency also includes the selected GPIO
 * backend and synchronous event callback. Timing and callback contracts from
 * esy_input.h apply unchanged. Link esy_input_gpio in addition to esy_input.
 */

/** @brief Borrowed button provider; the provider itself is read only during poll. */
typedef struct {
  const esy_gpio_t *gpio; /**< Required initialized input GPIO, owned by the consumer. */
  bool active_low; /**< True maps a low electrical level to logical pressed. */
} esy_input_gpio_button_t;

/**
 * @brief Borrowed quadrature provider; phase handles must be distinct.
 *
 * Both GPIOs must be initialized inputs. The provider and handles remain valid
 * through poll; no reference is retained by the adapter or core input instance.
 */
typedef struct {
  const esy_gpio_t *phase_a; /**< Required consumer-owned GPIO for AB bit 1. */
  const esy_gpio_t *phase_b; /**< Required distinct consumer-owned GPIO for bit 0. */
  bool inverted_a; /**< Invert the sampled electrical level of phase A. */
  bool inverted_b; /**< Invert the sampled electrical level of phase B. */
} esy_input_gpio_encoder_t;

/**
 * @brief Read one button GPIO, convert polarity, and submit one core sample.
 * @param button Initialized input handle exclusively owned by the caller.
 * @param provider Required provider with a non-NULL initialized GPIO.
 * @param now_ms Caller-supplied timestamp satisfying esy_input.h's time contract.
 * @return ESY_ERR_INVALID_ARG for NULL pointers; otherwise the first HAL read
 * error, or the result of esy_input_button_sample().
 * GPIO read failure leaves the input state and its pending timers unchanged;
 * callers must keep the successful-sample gap within the core time contract.
 * Handle state and callback reentry are validated by the core after a successful
 * read. No GPIO configuration, IRQ ownership, or provider reference is changed.
 */
esy_err_t esy_input_gpio_button_poll(esy_input_button_t *button,
                                    const esy_input_gpio_button_t *provider,
                                    uint32_t now_ms);

/**
 * @brief Read both GPIO phases and submit one AB sample to the core decoder.
 * @param encoder Initialized input handle exclusively owned by the caller.
 * @param provider Required provider with two distinct non-NULL GPIO handles.
 * @param now_ms Caller-supplied timestamp satisfying esy_input.h's time contract.
 * @return ESY_ERR_INVALID_ARG for NULL pointers or identical phase handles;
 * otherwise the first HAL read error, or esy_input_encoder_sample()'s result.
 * No core sample is submitted unless both reads succeed. The reads are
 * sequential, not an atomic port snapshot: sample while phases are stable and
 * fast enough to observe every edge. A changing input between reads can produce
 * an invalid AB transition; the core returns ESY_ERR_IO and resynchronizes.
 * Hardware requiring a coherent snapshot should call esy_input_encoder_sample()
 * with its own coherent provider. Core validation occurs after successful reads.
 */
esy_err_t esy_input_gpio_encoder_poll(esy_input_encoder_t *encoder,
                                     const esy_input_gpio_encoder_t *provider,
                                     uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* ESY_INPUT_GPIO_H */
