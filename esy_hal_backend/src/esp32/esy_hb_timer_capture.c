/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_timer_capture.h"
#include "esy_common/defs/coordination/esy_atomic_defs.h"
#include "esy_hb_esp32_context.h"

#include "driver/gpio.h"
#include "driver/mcpwm_cap.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <string.h>

#define ESP32_TC_GROUPS 2u
#define ESP32_TC_SLOTS_PER_GROUP 3u
#define ESP32_TC_MAX (ESP32_TC_GROUPS * ESP32_TC_SLOTS_PER_GROUP)
#define ESP32_TC_US_PER_SECOND 1000000u
#define ESP32_TC_MAX_IRQ_PRIORITY 3u

typedef struct {
  mcpwm_cap_channel_handle_t channel;
  esy_timer_capture_t *bound_handle;
  esy_timer_capture_callback_t callback;
  void *callback_arg;
  uint32_t previous_ticks;
  uint32_t ticks_per_us;
  atomic_uint last_captured_us;
  atomic_uint last_interval_us;
  atomic_bool enabled;
  atomic_bool callback_active;
  uint8_t phys_index;
  uint8_t gpio;
  uint8_t irq_core;
  uint8_t irq_priority;
  bool has_capture;
  bool channel_enabled;
  bool callbacks_registered;
  bool needs_cleanup;
} esp32_tc_ctx_t;

typedef struct {
  mcpwm_cap_timer_handle_t timer;
  esy_timer_capture_t *retiring_owner;
  uint32_t ticks_per_us;
  uint8_t channels;
  bool enabled;
  bool started;
} esp32_tc_group_t;

typedef struct {
  esy_timer_capture_t *owner;
  uint8_t gpio;
  uint8_t irq_core;
  uint8_t irq_priority;
} esp32_tc_claim_t;

typedef struct {
  atomic_flag claim_lock;
  esp32_tc_claim_t claims[ESP32_TC_MAX];
  esp32_tc_group_t groups[ESP32_TC_GROUPS];
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

static esy_err_t capture_error(esp_err_t error) {
  switch (error) {
    case ESP_OK: return ESY_OK;
    case ESP_ERR_NO_MEM: return ESY_ERR_NO_MEM;
    case ESP_ERR_NOT_FOUND: return ESY_ERR_BUSY;
    case ESP_ERR_INVALID_ARG: return ESY_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return ESY_ERR_STATE;
    case ESP_ERR_NOT_SUPPORTED: return ESY_ERR_NOT_SUPPORTED;
    default: return ESY_ERR_IO;
  }
}

static bool capture_isr(mcpwm_cap_channel_handle_t channel,
                        const mcpwm_capture_event_data_t *event, void *arg) {
  esp32_tc_ctx_t *c = arg;
  (void)channel;
  atomic_store_explicit(&c->callback_active, true, memory_order_release);
  if (atomic_load_explicit(&c->enabled, memory_order_acquire)) {
    /* Hardware latches ticks. Subtract in the native 32-bit domain before
     * truncating to microseconds; converted timestamps have a different wrap. */
    const uint32_t ticks = event->cap_value;
    const uint32_t timestamp_us = ticks / c->ticks_per_us;
    const uint32_t interval_us = c->has_capture
        ? (uint32_t)(ticks - c->previous_ticks) / c->ticks_per_us : 0u;
    c->previous_ticks = ticks;
    c->has_capture = true;
    atomic_store_explicit(&c->last_interval_us, interval_us, memory_order_release);
    atomic_store_explicit(&c->last_captured_us, timestamp_us, memory_order_release);
    if (c->callback != NULL) c->callback(timestamp_us, c->callback_arg);
  }
  atomic_store_explicit(&c->callback_active, false, memory_order_release);
  return false;
}

esy_err_t esy_hb_timer_capture_claim(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  if (cap == NULL || cfg == NULL || cfg->phys_index >= ESP32_TC_MAX ||
      cfg->inner.tim_id != cfg->phys_index / ESP32_TC_SLOTS_PER_GROUP ||
      cfg->inner.channel != cfg->phys_index % ESP32_TC_SLOTS_PER_GROUP ||
      cfg->inner.irq_priority > ESP32_TC_MAX_IRQ_PRIORITY ||
      cfg->inner.gpio == ESY_PIN_UNUSED || !GPIO_IS_VALID_GPIO(cfg->inner.gpio))
    return ESY_ERR_INVALID_ARG;
  if (!esy_hb_esp32_can_block()) return ESY_ERR_STATE;
#if CONFIG_MCPWM_ISR_IRAM_SAFE
  /* The generic callback and caller-owned context have no IRAM/internal-RAM
   * placement contract. Do not advertise cache-disabled callback safety. */
  return ESY_ERR_NOT_SUPPORTED;
#endif
  const uint8_t core = (uint8_t)esp_cpu_get_core_id();
  const uint8_t group_id = cfg->phys_index / ESP32_TC_SLOTS_PER_GROUP;
  if (cap->ctx != NULL) {
    const esp32_tc_ctx_t *c = capture_context(cap);
    const esp32_tc_group_t *group = &s_ctx.groups[group_id];
    return c != NULL && c->phys_index == cfg->phys_index &&
                   c->gpio == cfg->inner.gpio && c->irq_core == core &&
                   c->irq_priority == cfg->inner.irq_priority &&
                   !atomic_load_explicit(&c->enabled, memory_order_acquire) &&
                   c->channel == NULL && !c->needs_cleanup &&
                   group->retiring_owner == NULL &&
                   !atomic_load_explicit(&c->callback_active, memory_order_acquire)
               ? ESY_OK : ESY_ERR_STATE;
  }
  /* Only reservation metadata is locked. Native lifecycle is externally
   * serialized per group on its IRQ-allocation core, outside this try-lock. */
  if (atomic_flag_test_and_set_explicit(&s_ctx.claim_lock, memory_order_acquire))
    return ESY_ERR_BUSY;
  esy_err_t result = ESY_OK;
  for (uint32_t i = 0u; i < ESP32_TC_MAX; ++i) {
    const esp32_tc_claim_t *claim = &s_ctx.claims[i];
    if (claim->owner == NULL) continue;
    if (i == cfg->phys_index || claim->gpio == cfg->inner.gpio ||
        (i / ESP32_TC_SLOTS_PER_GROUP == group_id &&
         (claim->irq_core != core || claim->irq_priority != cfg->inner.irq_priority))) {
      result = ESY_ERR_BUSY;
      break;
    }
  }
  if (result == ESY_OK && s_ctx.groups[group_id].retiring_owner != NULL)
    result = ESY_ERR_BUSY;
  if (result == ESY_OK) {
    esp32_tc_ctx_t *c = (esp32_tc_ctx_t *)cap->backend.bytes;
    memset(c, 0, sizeof(*c));
    atomic_init(&c->enabled, false);
    atomic_init(&c->callback_active, false);
    atomic_init(&c->last_captured_us, 0u);
    atomic_init(&c->last_interval_us, 0u);
    c->phys_index = cfg->phys_index;
    c->gpio = cfg->inner.gpio;
    c->irq_core = core;
    c->irq_priority = cfg->inner.irq_priority;
    c->bound_handle = cap;
    s_ctx.claims[cfg->phys_index] = (esp32_tc_claim_t){
        .owner = cap, .gpio = c->gpio, .irq_core = core,
        .irq_priority = c->irq_priority};
    cap->ctx = c;
  }
  atomic_flag_clear_explicit(&s_ctx.claim_lock, memory_order_release);
  return result;
}

static esy_err_t capture_prepare_group(esp32_tc_group_t *group, uint8_t group_id) {
  if (group->retiring_owner != NULL) return ESY_ERR_BUSY;
  if (group->timer == NULL) {
    const mcpwm_capture_timer_config_t config = {
        .group_id = group_id, .clk_src = MCPWM_CAPTURE_CLK_SRC_APB,
        .resolution_hz = 0u};
    esp_err_t error = mcpwm_new_capture_timer(&config, &group->timer);
    if (error != ESP_OK) return capture_error(error);
    uint32_t resolution_hz = 0u;
    error = mcpwm_capture_timer_get_resolution(group->timer, &resolution_hz);
    if (error != ESP_OK) return capture_error(error);
    if (resolution_hz < ESP32_TC_US_PER_SECOND ||
        resolution_hz % ESP32_TC_US_PER_SECOND != 0u)
      return ESY_ERR_NOT_SUPPORTED;
    group->ticks_per_us = resolution_hz / ESP32_TC_US_PER_SECOND;
  }
  if (!group->enabled) {
    esp_err_t error = mcpwm_capture_timer_enable(group->timer);
    if (error != ESP_OK) return capture_error(error);
    group->enabled = true;
  }
  if (!group->started) {
    esp_err_t error = mcpwm_capture_timer_start(group->timer);
    if (error != ESP_OK) return capture_error(error);
    group->started = true;
  }
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_init(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  esy_err_t result = esy_hb_timer_capture_claim(cap, cfg);
  if (result != ESY_OK) return result;
  esp32_tc_ctx_t *c = capture_context(cap);
  esp32_tc_group_t *group = &s_ctx.groups[c->phys_index / ESP32_TC_SLOTS_PER_GROUP];
  c->needs_cleanup = true;
  result = capture_prepare_group(group, cfg->inner.tim_id);
  if (result != ESY_OK) {
    if (group->timer != NULL && group->channels == 0u) group->retiring_owner = cap;
    return result;
  }
  c->callback = cfg->callback;
  c->callback_arg = cfg->callback_arg;
  c->ticks_per_us = group->ticks_per_us;
  c->has_capture = false;
  c->previous_ticks = 0u;
  atomic_store_explicit(&c->last_captured_us, 0u, memory_order_relaxed);
  atomic_store_explicit(&c->last_interval_us, 0u, memory_order_relaxed);
  const mcpwm_capture_channel_config_t config = {
      .gpio_num = c->gpio, .intr_priority = c->irq_priority, .prescale = 1u,
      .flags = {.pos_edge = cfg->rising_edge, .neg_edge = !cfg->rising_edge,
                .keep_io_conf_at_exit = true}};
  esp_err_t error = mcpwm_new_capture_channel(group->timer, &config, &c->channel);
  if (error != ESP_OK) {
    if (group->channels == 0u) group->retiring_owner = cap;
    return capture_error(error);
  }
  ++group->channels;
  const mcpwm_capture_event_callbacks_t callbacks = {.on_cap = capture_isr};
  error = mcpwm_capture_channel_register_event_callbacks(c->channel, &callbacks, c);
  if (error != ESP_OK) return capture_error(error);
  c->callbacks_registered = true;
  atomic_store_explicit(&c->enabled, true, memory_order_release);
  error = mcpwm_capture_channel_enable(c->channel);
  if (error != ESP_OK) {
    atomic_store_explicit(&c->enabled, false, memory_order_release);
    return capture_error(error);
  }
  c->channel_enabled = true;
  return ESY_OK;
}

esy_err_t esy_hb_timer_capture_suspend(esy_timer_capture_t *cap) {
  if (cap == NULL) return ESY_ERR_INVALID_ARG;
  esp32_tc_ctx_t *c = capture_context(cap);
  if (c == NULL || !esy_hb_esp32_can_block() ||
      c->irq_core != esp_cpu_get_core_id()) return ESY_ERR_STATE;
  esp32_tc_group_t *group = &s_ctx.groups[c->phys_index / ESP32_TC_SLOTS_PER_GROUP];
  if (group->retiring_owner != NULL && group->retiring_owner != cap)
    return ESY_ERR_BUSY;
  /* Same-core SDK interrupt disable retires the borrowed callback before any
   * channel/context deletion. Failed stages retain their native handles. */
  atomic_store_explicit(&c->enabled, false, memory_order_release);
  if (c->channel_enabled) {
    esp_err_t error = mcpwm_capture_channel_disable(c->channel);
    if (error != ESP_OK) return capture_error(error);
    c->channel_enabled = false;
  }
  if (atomic_load_explicit(&c->callback_active, memory_order_acquire))
    return ESY_ERR_BUSY;
  if (c->callbacks_registered) {
    const mcpwm_capture_event_callbacks_t callbacks = {0};
    esp_err_t error = mcpwm_capture_channel_register_event_callbacks(
        c->channel, &callbacks, NULL);
    if (error != ESP_OK) return capture_error(error);
    c->callbacks_registered = false;
  }
  if (c->channel != NULL) {
    esp_err_t error = mcpwm_del_capture_channel(c->channel);
    if (error != ESP_OK) return capture_error(error);
    c->channel = NULL;
    --group->channels;
  }
  if (group->channels == 0u && group->timer != NULL) {
    group->retiring_owner = cap;
    if (group->started) {
      esp_err_t error = mcpwm_capture_timer_stop(group->timer);
      if (error != ESP_OK) return capture_error(error);
      group->started = false;
    }
    if (group->enabled) {
      esp_err_t error = mcpwm_capture_timer_disable(group->timer);
      if (error != ESP_OK) return capture_error(error);
      group->enabled = false;
    }
    esp_err_t error = mcpwm_del_capture_timer(group->timer);
    if (error != ESP_OK) return capture_error(error);
    group->timer = NULL;
    group->ticks_per_us = 0u;
    group->retiring_owner = NULL;
  }
  c->needs_cleanup = false;
  return ESY_OK;
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
  memset(&s_ctx.claims[c->phys_index], 0, sizeof(s_ctx.claims[c->phys_index]));
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
  *caps = ESY_TIMER_CAPTURE_CAP_EDGE_CALLBACK | ESY_TIMER_CAPTURE_CAP_INTERVAL |
      ESY_TIMER_CAPTURE_CAP_HARDWARE_LATCH;
  return ESY_OK;
}

void esy_hb_timer_capture_handle_irq(uint32_t index) { (void)index; }
