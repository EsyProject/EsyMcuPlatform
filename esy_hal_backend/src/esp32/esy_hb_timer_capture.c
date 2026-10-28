/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_timer_capture.h"
#include "esy_common/defs/coordination/esy_atomic_defs.h"
#include "esy_hal_backend/esy_hb_gpio.h"

#include "esp_cpu.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_timer.h"
#include "driver/gpio.h"

#define ESP32_TC_MAX 8u

typedef struct {
  uint8_t phys_index;
  uint8_t gpio;
  uint8_t irq_core;
  bool has_capture;
  bool irq_attached;
  atomic_bool enabled;
  atomic_bool callback_active;
  uint32_t previous_us;
  atomic_uint last_captured_us;
  atomic_uint last_interval_us;
  esy_timer_capture_callback_t callback;
  void *callback_arg;
  esy_timer_capture_t *bound_handle;
} esp32_tc_ctx_t;

typedef struct {
  esy_timer_capture_t *owner;
  uint8_t gpio;
} esp32_tc_claim_t;

typedef struct {
  atomic_flag claim_lock;
  esp32_tc_claim_t claims[ESP32_TC_MAX];
} esy_hb_timer_capture_ctx_t;

static esy_hb_timer_capture_ctx_t s_ctx = {
    .claim_lock = ATOMIC_FLAG_INIT,
};

_Static_assert(ESY_TIMER_CAPTURE_PRIV_BYTES >= sizeof(esp32_tc_ctx_t),
               "ESY_TIMER_CAPTURE_PRIV_BYTES too small");

static esp32_tc_ctx_t *capture_context(const esy_timer_capture_t *cap) {
  if (cap == NULL || cap->ctx != (const void *)cap->backend.bytes) return NULL;
  esp32_tc_ctx_t *c = cap->ctx;
  return c->bound_handle == cap && c->phys_index < ESP32_TC_MAX ? c : NULL;
}

static void tc_gpio_isr(uint8_t gpio, void *arg) {
  esp32_tc_ctx_t *c = arg;
  if (c == NULL) return;
  atomic_store_explicit(&c->callback_active, true, memory_order_release);
  if (atomic_load_explicit(&c->enabled, memory_order_acquire) && c->gpio == gpio) {
    const uint32_t now_us = (uint32_t)esp_timer_get_time();
    /* The ISR alone owns history. Queries load one complete published word. */
    const uint32_t interval = c->has_capture ? now_us - c->previous_us : 0u;
    c->previous_us = now_us;
    c->has_capture = true;
    atomic_store_explicit(&c->last_interval_us, interval, memory_order_release);
    atomic_store_explicit(&c->last_captured_us, now_us, memory_order_release);
    if (c->callback != NULL) c->callback(now_us, c->callback_arg);
  }
  atomic_store_explicit(&c->callback_active, false, memory_order_release);
}

esy_err_t esy_hb_timer_capture_claim(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  if (cap == NULL || cfg == NULL || cfg->phys_index >= ESP32_TC_MAX ||
      cfg->inner.gpio == ESY_PIN_UNUSED || !GPIO_IS_VALID_GPIO(cfg->inner.gpio))
    return ESY_ERR_INVALID_ARG;
  if (cap->ctx != NULL) {
    const esp32_tc_ctx_t *c = capture_context(cap);
    return c != NULL && c->phys_index == cfg->phys_index &&
                   c->gpio == cfg->inner.gpio && c->irq_core == esp_cpu_get_core_id() &&
                   !c->enabled &&
                   !c->irq_attached && !c->callback_active
               ? ESY_OK : ESY_ERR_STATE;
  }
  /* The try-lock serializes a multi-resource reservation without waiting in
   * a critical section. Contending lifecycle calls retry from their owner. */
  if (atomic_flag_test_and_set_explicit(&s_ctx.claim_lock, memory_order_acquire))
    return ESY_ERR_BUSY;
  esy_err_t result = ESY_OK;
  for (uint32_t i = 0u; i < ESP32_TC_MAX; ++i) {
    const esp32_tc_claim_t *claim = &s_ctx.claims[i];
    if (claim->owner != NULL &&
        (i == cfg->phys_index || claim->gpio == cfg->inner.gpio)) {
      result = ESY_ERR_BUSY;
      break;
    }
  }
  if (result == ESY_OK) {
    esp32_tc_ctx_t *c = (esp32_tc_ctx_t *)cap->backend.bytes;
    memset(c, 0, sizeof(*c));
    atomic_init(&c->enabled, false);
    atomic_init(&c->callback_active, false);
    atomic_init(&c->last_captured_us, 0u);
    atomic_init(&c->last_interval_us, 0u);
    c->phys_index = cfg->phys_index;
    c->gpio = cfg->inner.gpio;
    c->irq_core = (uint8_t)esp_cpu_get_core_id();
    c->bound_handle = cap;
    s_ctx.claims[cfg->phys_index].gpio = cfg->inner.gpio;
    s_ctx.claims[cfg->phys_index].owner = cap;
    cap->ctx = c;
  }
  atomic_flag_clear_explicit(&s_ctx.claim_lock, memory_order_release);
  return result;
}

esy_err_t esy_hb_timer_capture_init(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  esy_err_t result = esy_hb_timer_capture_claim(cap, cfg);
  if (result != ESY_OK) return result;
  esp32_tc_ctx_t *c = capture_context(cap);
  c->callback = cfg->callback;
  c->callback_arg = cfg->callback_arg;
  c->has_capture = false;
  c->previous_us = 0u;
  atomic_store_explicit(&c->last_captured_us, 0u, memory_order_relaxed);
  atomic_store_explicit(&c->last_interval_us, 0u, memory_order_relaxed);
  const esy_gpio_irq_config_t irq_config = {
      .pin = c->gpio,
      .irq_mode = cfg->rising_edge ? ESY_GPIO_IRQ_RISING : ESY_GPIO_IRQ_FALLING,
      .handler = tc_gpio_isr,
      .arg = c,
      .priority = cfg->inner.irq_priority,
  };
  atomic_store_explicit(&c->enabled, true, memory_order_release);
  result = esy_hb_gpio_attach_irq(&irq_config);
  if (result != ESY_OK) {
    atomic_store_explicit(&c->enabled, false, memory_order_release);
    return result;
  }
  c->irq_attached = true;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_suspend(esy_timer_capture_t *cap) {
  if (cap == NULL) return ESY_ERR_INVALID_ARG;
  esp32_tc_ctx_t *c = capture_context(cap);
  if (c == NULL || c->irq_core != esp_cpu_get_core_id()) return ESY_ERR_STATE;
  /* IRQ lifecycle runs on its dispatch core, outside callbacks. Detach first
   * retires the provider's borrowed context before a reservation is released. */
  atomic_store_explicit(&c->enabled, false, memory_order_release);
  if (c->irq_attached) {
    esy_hb_gpio_disable_irq(c->gpio);
    c->irq_attached = false;
  }
  return atomic_load_explicit(&c->callback_active, memory_order_acquire)
             ? ESY_ERR_BUSY : ESY_OK;
}

esy_err_t esy_hb_timer_capture_deinit(esy_timer_capture_t *cap) {
  if (cap == NULL) return ESY_ERR_INVALID_ARG;
  if (cap->ctx == NULL) return ESY_OK;
  esp32_tc_ctx_t *c = capture_context(cap);
  if (c == NULL) return ESY_ERR_STATE;
  esy_err_t result = esy_hb_timer_capture_suspend(cap);
  if (result != ESY_OK) return result;
  if (atomic_flag_test_and_set_explicit(&s_ctx.claim_lock, memory_order_acquire))
    return ESY_ERR_BUSY;
  if (s_ctx.claims[c->phys_index].owner != cap) {
    atomic_flag_clear_explicit(&s_ctx.claim_lock, memory_order_release);
    return ESY_ERR_STATE;
  }
  s_ctx.claims[c->phys_index].owner = NULL;
  memset(c, 0, sizeof(*c));
  cap->ctx = NULL;
  atomic_flag_clear_explicit(&s_ctx.claim_lock, memory_order_release);
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_get_value(
    const esy_timer_capture_t *cap, uint32_t *value) {
  if (cap == NULL || value == NULL) return ESY_ERR_INVALID_ARG;
  const esp32_tc_ctx_t *c = capture_context(cap);
  if (c == NULL || !atomic_load_explicit(&c->enabled, memory_order_acquire))
    return ESY_ERR_STATE;
  *value = atomic_load_explicit(&c->last_captured_us, memory_order_acquire);
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_get_interval(
    const esy_timer_capture_t *cap, uint32_t *interval) {
  if (cap == NULL || interval == NULL) return ESY_ERR_INVALID_ARG;
  const esp32_tc_ctx_t *c = capture_context(cap);
  if (c == NULL || !atomic_load_explicit(&c->enabled, memory_order_acquire))
    return ESY_ERR_STATE;
  *interval = atomic_load_explicit(&c->last_interval_us, memory_order_acquire);
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_get_phys_index(
    const esy_timer_capture_t *cap, uint8_t *index) {
  if (cap == NULL || index == NULL) return ESY_ERR_INVALID_ARG;
  const esp32_tc_ctx_t *c = capture_context(cap);
  if (c == NULL) return ESY_ERR_STATE;
  *index = c->phys_index;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_get_caps(
    const esy_timer_capture_t *cap, uint32_t *caps) {
  if (cap == NULL || caps == NULL) return ESY_ERR_INVALID_ARG;
  if (capture_context(cap) == NULL) return ESY_ERR_STATE;
  *caps = ESY_TIMER_CAPTURE_CAP_EDGE_CALLBACK | ESY_TIMER_CAPTURE_CAP_INTERVAL;
  return ESY_OK;
}

void esy_hb_timer_capture_handle_irq(uint32_t index) { (void)index; }
