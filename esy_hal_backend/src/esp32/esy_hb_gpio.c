/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_gpio.h"
#include "esy_common/defs/coordination/esy_atomic_defs.h"

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_sleep.h"
#include <stdatomic.h>

#define MAX_GPIO_PINS 48

typedef struct {
  esy_gpio_irq_handler_t handlers[MAX_GPIO_PINS];
  void* args[MAX_GPIO_PINS];
  bool wake_owned[MAX_GPIO_PINS];
  atomic_int isr_core;
} esy_hb_gpio_ctx_t;

static esy_hb_gpio_ctx_t s_ctx = {.isr_core = ATOMIC_VAR_INIT(-1)};

static bool on_isr_core(void) {
  return atomic_load_explicit(&s_ctx.isr_core, memory_order_acquire) ==
         esp_cpu_get_core_id();
}

static void gpio_isr_handler(void* arg) {
  uint8_t pin = (uint8_t)(uintptr_t)arg;

  if (pin < MAX_GPIO_PINS && s_ctx.handlers[pin]) {
    s_ctx.handlers[pin](pin, s_ctx.args[pin]);
  }
}

esy_err_t esy_hb_gpio_init(uint8_t pin, esy_gpio_mode_t mode) {
  if (pin >= MAX_GPIO_PINS) return ESY_ERR_INVALID_ARG;
  /* gpio_config also replaces the interrupt type. Keep an attached IRQ/wake
   * owner's native configuration intact until its serialized detach. */
  if (s_ctx.handlers[pin] != NULL) return ESY_ERR_BUSY;

  gpio_config_t io_conf = {};
  io_conf.pin_bit_mask = (1ULL << pin);
  io_conf.intr_type = GPIO_INTR_DISABLE;

  switch (mode) {
    case ESY_GPIO_MODE_INPUT:
      io_conf.mode = GPIO_MODE_INPUT;
      io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
      io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
      break;
    case ESY_GPIO_MODE_INPUT_PULLUP:
      io_conf.mode = GPIO_MODE_INPUT;
      io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
      io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
      break;
    case ESY_GPIO_MODE_INPUT_PULLDOWN:
      io_conf.mode = GPIO_MODE_INPUT;
      io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
      io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
      break;
    case ESY_GPIO_MODE_OUTPUT:
      io_conf.mode = GPIO_MODE_OUTPUT;
      io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
      io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
      break;
    default:
      return ESY_ERR_INVALID_ARG;
  }

  esp_err_t ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    return ESY_ERR_INVALID_ARG;
  }

  return ESY_OK;
}

void esy_hb_gpio_deinit(uint8_t pin) {
  if (pin < MAX_GPIO_PINS) {
    gpio_reset_pin(pin);
  }
}

void esy_hb_gpio_write(uint8_t pin, bool value) {
  if (pin < MAX_GPIO_PINS) {
    gpio_set_level(pin, value ? 1 : 0);
  }
}

bool esy_hb_gpio_read(uint8_t pin) {
  if (pin < MAX_GPIO_PINS) {
    return gpio_get_level(pin) != 0;
  }
  return false;
}

esy_err_t esy_hb_gpio_attach_irq(const esy_gpio_irq_config_t* config) {
  if (!config || config->pin >= MAX_GPIO_PINS ||
      !GPIO_IS_VALID_GPIO(config->pin) || !config->handler)
    return ESY_ERR_INVALID_ARG;

  gpio_int_type_t intr_type;
  switch (config->irq_mode) {
    case ESY_GPIO_IRQ_RISING:
      intr_type = GPIO_INTR_POSEDGE;
      break;
    case ESY_GPIO_IRQ_FALLING:
      intr_type = GPIO_INTR_NEGEDGE;
      break;
    case ESY_GPIO_IRQ_BOTH:
      intr_type = GPIO_INTR_ANYEDGE;
      break;
    default:
      return ESY_ERR_INVALID_ARG;
  }

  gpio_pull_mode_t pull_mode;
  switch (config->pull) {
    case ESY_GPIO_PULL_NONE:
      pull_mode = GPIO_FLOATING;
      break;
    case ESY_GPIO_PULL_UP:
      pull_mode = GPIO_PULLUP_ONLY;
      break;
    case ESY_GPIO_PULL_DOWN:
      pull_mode = GPIO_PULLDOWN_ONLY;
      break;
    default:
      return ESY_ERR_INVALID_ARG;
  }

  int core = atomic_load_explicit(&s_ctx.isr_core, memory_order_acquire);
  if (core == -1) {
    int expected = -1;
    if (!atomic_compare_exchange_strong_explicit(
            &s_ctx.isr_core, &expected, -2, memory_order_acq_rel,
            memory_order_acquire)) return ESY_ERR_BUSY;
    const esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK) {
      atomic_store_explicit(&s_ctx.isr_core, -1, memory_order_release);
      return ret == ESP_ERR_INVALID_STATE ? ESY_ERR_STATE : ESY_ERR_IO;
    }
    core = esp_cpu_get_core_id();
    atomic_store_explicit(&s_ctx.isr_core, core, memory_order_release);
  }
  /* SDK removal does not wait for a callback executing on another core.
   * Keep lifecycle on the service core so detach retires borrowed context. */
  if (core != esp_cpu_get_core_id()) return ESY_ERR_STATE;
  if (s_ctx.handlers[config->pin] != NULL) return ESY_ERR_BUSY;
  if (gpio_set_pull_mode(config->pin, pull_mode) != ESP_OK) {
    return ESY_ERR_INVALID_ARG;
  }

  esp_err_t ret = gpio_set_intr_type(config->pin, intr_type);
  if (ret != ESP_OK) {
    return ESY_ERR_INVALID_ARG;
  }

  s_ctx.handlers[config->pin] = config->handler;
  s_ctx.args[config->pin] = config->arg;
  ret = gpio_isr_handler_add(config->pin, gpio_isr_handler,
                             (void*)(uintptr_t)config->pin);
  if (ret != ESP_OK) {
    s_ctx.handlers[config->pin] = NULL;
    s_ctx.args[config->pin] = NULL;
    return ESY_ERR_INVALID_ARG;
  }

  return ESY_OK;
}

void esy_hb_gpio_enable_irq(uint8_t pin) {
  if (pin < MAX_GPIO_PINS && on_isr_core() && s_ctx.handlers[pin] != NULL) {
    gpio_intr_enable(pin);
  }
}

static esy_err_t gpio_detach_irq(uint8_t pin) {
  if (pin >= MAX_GPIO_PINS) return ESY_ERR_INVALID_ARG;
  if (s_ctx.handlers[pin] == NULL) return ESY_OK;
  if (!on_isr_core()) return ESY_ERR_STATE;
  if (gpio_intr_disable(pin) != ESP_OK ||
      gpio_isr_handler_remove(pin) != ESP_OK) return ESY_ERR_IO;
  s_ctx.handlers[pin] = NULL;
  s_ctx.args[pin] = NULL;
  return ESY_OK;
}

void esy_hb_gpio_disable_irq(uint8_t pin) {
  if (pin < MAX_GPIO_PINS && !s_ctx.wake_owned[pin])
    (void)gpio_detach_irq(pin);
}

esy_err_t esy_hb_gpio_enable_wakeup(const esy_gpio_irq_config_t* config,
                                    bool *out_cleanup_required) {
  if (out_cleanup_required == NULL) return ESY_ERR_INVALID_ARG;
  *out_cleanup_required = false;
  if (!config || config->pin >= MAX_GPIO_PINS ||
      !GPIO_IS_VALID_GPIO(config->pin)) return ESY_ERR_INVALID_ARG;
  if (config->irq_mode != ESY_GPIO_IRQ_RISING &&
      config->irq_mode != ESY_GPIO_IRQ_FALLING)
    return ESY_ERR_NOT_SUPPORTED;

  /* The IRQ lifecycle owns this pin. Claim it before changing wake state so
   * BUSY, wrong-core and invalid-config rejection have no wake side effects.
   * Leave the shared sleep-domain policy to its system owner. */
  esy_err_t ret = esy_hb_gpio_attach_irq(config);
  if (ret != ESY_OK) return ret;
  s_ctx.wake_owned[config->pin] = true;
  *out_cleanup_required = true;
  /* Arm the shared GPIO wake class before the per-pin source. No global
   * operation can then fail after the pin becomes a live wake source. */
  if (esp_sleep_enable_gpio_wakeup() == ESP_OK &&
      gpio_wakeup_enable(config->pin,
          config->irq_mode == ESY_GPIO_IRQ_RISING
              ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL) == ESP_OK)
    return ESY_OK;
  ret = esy_hb_gpio_disable_wakeup(config->pin);
  *out_cleanup_required = s_ctx.wake_owned[config->pin];
  return ret == ESY_OK ? ESY_ERR_IO : ret;
}

esy_err_t esy_hb_gpio_disable_wakeup(uint8_t pin) {
  if (pin >= MAX_GPIO_PINS || !GPIO_IS_VALID_GPIO(pin))
    return ESY_ERR_INVALID_ARG;
  if (!s_ctx.wake_owned[pin]) return ESY_OK;
  if (!on_isr_core()) return ESY_ERR_STATE;
  if (gpio_wakeup_disable(pin) != ESP_OK) return ESY_ERR_IO;
  esy_err_t ret = gpio_detach_irq(pin);
  if (ret == ESY_OK) s_ctx.wake_owned[pin] = false;
  return ret;
}

extern void esy_hb_gpio_handle_irq(uint8_t irq_group) { (void)irq_group; }
