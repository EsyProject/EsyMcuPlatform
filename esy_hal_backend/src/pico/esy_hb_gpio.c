/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_gpio.h"
#include "esy_common/defs/coordination/esy_atomic_defs.h"

#include <stdatomic.h>
#include "pico/platform.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"

#define MAX_GPIO_PINS 30

typedef struct {
  esy_gpio_irq_handler_t handlers[MAX_GPIO_PINS];
  void *args[MAX_GPIO_PINS];
  uint32_t irq_events[MAX_GPIO_PINS];
  bool irq_priority_set;
  atomic_uint irq_core_token;
} esy_hb_gpio_ctx_t;

static esy_hb_gpio_ctx_t s_ctx;

static void gpio_irq_dispatch(uint gpio, uint32_t events) {
  (void)events;

  if (gpio < MAX_GPIO_PINS && s_ctx.handlers[gpio]) {
    s_ctx.handlers[gpio](gpio, s_ctx.args[gpio]);
  }
}

static uint32_t irq_events_from_mode(esy_gpio_irq_mode_t mode) {
  switch (mode) {
    case ESY_GPIO_IRQ_RISING:
      return GPIO_IRQ_EDGE_RISE;
    case ESY_GPIO_IRQ_FALLING:
      return GPIO_IRQ_EDGE_FALL;
    case ESY_GPIO_IRQ_BOTH:
      return GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL;
    default:
      return 0u;
  }
}

esy_err_t esy_hb_gpio_init(uint8_t pin, esy_gpio_mode_t mode) {
  if (pin >= MAX_GPIO_PINS) return ESY_ERR_INVALID_ARG;

  gpio_init(pin);

  switch (mode) {
    case ESY_GPIO_MODE_INPUT:
      gpio_set_dir(pin, GPIO_IN);
      gpio_disable_pulls(pin);
      break;
    case ESY_GPIO_MODE_INPUT_PULLUP:
      gpio_set_dir(pin, GPIO_IN);
      gpio_pull_up(pin);
      break;
    case ESY_GPIO_MODE_INPUT_PULLDOWN:
      gpio_set_dir(pin, GPIO_IN);
      gpio_pull_down(pin);
      break;
    case ESY_GPIO_MODE_OUTPUT:
      gpio_set_dir(pin, GPIO_OUT);
      break;
    default:
      return ESY_ERR_INVALID_ARG;
  }

  return ESY_OK;
}

void esy_hb_gpio_deinit(uint8_t pin) {
  if (pin >= MAX_GPIO_PINS) return;
  if (s_ctx.handlers[pin] != NULL &&
      atomic_load_explicit(&s_ctx.irq_core_token, memory_order_acquire) !=
          get_core_num() + 1u) return;

  gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
  s_ctx.handlers[pin] = NULL;
  s_ctx.args[pin] = NULL;
  s_ctx.irq_events[pin] = 0u;
  gpio_deinit(pin);
}

void esy_hb_gpio_write(uint8_t pin, bool value) {
  if (pin < MAX_GPIO_PINS) {
    gpio_put(pin, value);
  }
}

bool esy_hb_gpio_read(uint8_t pin) {
  if (pin < MAX_GPIO_PINS) {
    return gpio_get(pin);
  }
  return false;
}

esy_err_t esy_hb_gpio_attach_irq(const esy_gpio_irq_config_t *config) {
  if (!config || config->pin >= MAX_GPIO_PINS || !config->handler)
    return ESY_ERR_INVALID_ARG;

  uint32_t events = irq_events_from_mode(config->irq_mode);
  if (events == 0u) return ESY_ERR_INVALID_ARG;

  if (config->pull != ESY_GPIO_PULL_NONE && config->pull != ESY_GPIO_PULL_UP &&
      config->pull != ESY_GPIO_PULL_DOWN) return ESY_ERR_INVALID_ARG;
  /* One dispatch core owns this shared IRQ service. The atomic token selects
   * the first owner; later lifecycle operations are serialized on that core. */
  const unsigned int token = get_core_num() + 1u;
  unsigned int expected = 0u;
  if (!atomic_compare_exchange_strong_explicit(
          &s_ctx.irq_core_token, &expected, token, memory_order_acq_rel,
          memory_order_acquire) && expected != token) return ESY_ERR_STATE;
  if (s_ctx.handlers[config->pin] != NULL) return ESY_ERR_BUSY;

  gpio_disable_pulls(config->pin);
  if (config->pull == ESY_GPIO_PULL_UP) gpio_pull_up(config->pin);
  if (config->pull == ESY_GPIO_PULL_DOWN) gpio_pull_down(config->pin);

  s_ctx.args[config->pin] = config->arg;
  s_ctx.handlers[config->pin] = config->handler;
  s_ctx.irq_events[config->pin] = events;

  if (!s_ctx.irq_priority_set) {
    irq_set_priority(IO_IRQ_BANK0, config->priority);
    s_ctx.irq_priority_set = true;
  }

  gpio_set_irq_enabled_with_callback(config->pin, events, true,
                                     gpio_irq_dispatch);

  return ESY_OK;
}

void esy_hb_gpio_enable_irq(uint8_t pin) {
  if (pin >= MAX_GPIO_PINS || !s_ctx.handlers[pin] ||
      atomic_load_explicit(&s_ctx.irq_core_token, memory_order_acquire) !=
          get_core_num() + 1u) return;

  gpio_set_irq_enabled(pin, s_ctx.irq_events[pin], true);
}

void esy_hb_gpio_disable_irq(uint8_t pin) {
  if (pin < MAX_GPIO_PINS &&
      atomic_load_explicit(&s_ctx.irq_core_token, memory_order_acquire) ==
          get_core_num() + 1u) {
    gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    s_ctx.handlers[pin] = NULL;
    s_ctx.args[pin] = NULL;
    s_ctx.irq_events[pin] = 0u;
  }
}

esy_err_t esy_hb_gpio_enable_wakeup(const esy_gpio_irq_config_t *config,
                                    bool *out_cleanup_required) {
  if (out_cleanup_required == NULL) return ESY_ERR_INVALID_ARG;
  *out_cleanup_required = false;
  esy_err_t ret = esy_hb_gpio_attach_irq(config);
  *out_cleanup_required = ret == ESY_OK;
  return ret;
}

esy_err_t esy_hb_gpio_disable_wakeup(uint8_t pin) {
  if (pin >= MAX_GPIO_PINS) return ESY_ERR_INVALID_ARG;
  if (s_ctx.handlers[pin] != NULL &&
      atomic_load_explicit(&s_ctx.irq_core_token, memory_order_acquire) !=
          get_core_num() + 1u) return ESY_ERR_STATE;
  esy_hb_gpio_disable_irq(pin);
  return ESY_OK;
}

extern void esy_hb_gpio_handle_irq(uint8_t irq_group) { (void)irq_group; }
