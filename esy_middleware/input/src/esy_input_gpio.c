/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/input/esy_input_gpio.h"

#include <stddef.h>

#include "esy_hal/esy_hal_gpio.h"

#define ESY_INPUT_GPIO_PHASE_A_BIT UINT8_C(2)
#define ESY_INPUT_GPIO_PHASE_B_BIT UINT8_C(1)

esy_err_t esy_input_gpio_button_poll(esy_input_button_t *button,
                                    const esy_input_gpio_button_t *provider,
                                    uint32_t now_ms) {
  bool level;
  esy_err_t err;

  if (button == NULL || provider == NULL || provider->gpio == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  err = esy_hal_gpio_read(provider->gpio, &level);
  if (err != ESY_OK) {
    return err;
  }

  return esy_input_button_sample(button, level != provider->active_low, now_ms);
}

esy_err_t esy_input_gpio_encoder_poll(esy_input_encoder_t *encoder,
                                     const esy_input_gpio_encoder_t *provider,
                                     uint32_t now_ms) {
  bool phase_a;
  bool phase_b;
  uint8_t ab;
  esy_err_t err;

  if (encoder == NULL || provider == NULL || provider->phase_a == NULL ||
      provider->phase_b == NULL || provider->phase_a == provider->phase_b) {
    return ESY_ERR_INVALID_ARG;
  }

  err = esy_hal_gpio_read(provider->phase_a, &phase_a);
  if (err != ESY_OK) {
    return err;
  }
  err = esy_hal_gpio_read(provider->phase_b, &phase_b);
  if (err != ESY_OK) {
    return err;
  }

  ab = (uint8_t)((phase_a != provider->inverted_a ? ESY_INPUT_GPIO_PHASE_A_BIT : 0u) |
                 (phase_b != provider->inverted_b ? ESY_INPUT_GPIO_PHASE_B_BIT : 0u));
  return esy_input_encoder_sample(encoder, ab, now_ms);
}
