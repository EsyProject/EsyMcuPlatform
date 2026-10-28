/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_pwm.h"
#include "esy_common/defs/peripheral/esy_pwm_defs.h"

#include <stdatomic.h>
#include <string.h>

#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

#include "esy_hal_backend/esy_hb_dma.h"

#define RP2040_PWM_MAX (NUM_PWM_SLICES * 2)
#define PWM_DIVIDER_SCALE 16u
#define PWM_DIVIDER_MAX 4095u
#define PWM_PERIOD_MAX 65536u

enum {
  PWM_DMA_DONE = 1u << 0,
  PWM_DMA_ERROR = 1u << 1,
};

typedef struct {
  uint8_t slice;
  uint8_t channel;
  uint8_t phys_index;
  uint8_t gpio;
  float duty_cycle;

  void (*handler)(void *);
  void *handler_arg;

  bool enabled;
  bool initialized;
  bool suspended;
  bool inverted;

  bool dma_owned;
  uint32_t dma_dreq;
  uint8_t dma_channel;
  uint8_t dma_resource;
  atomic_uint dma_events;
  esy_dma_callback_t dma_callback;
  void *dma_callback_arg;
  esy_dma_callback_t dma_error_callback;
  void *dma_error_callback_arg;

  esy_pwm_t *bound_pwm;
} rp2040_pwm_ctx_t;

typedef struct {
  uint32_t freq_hz;
  uint8_t owners;
  uint8_t running_mask;
  uint8_t inversion_mask;
  uint8_t handler_mask;
  bool irq_enabled;
} rp2040_pwm_slice_t;

typedef struct {
  rp2040_pwm_ctx_t channel[RP2040_PWM_MAX];
  rp2040_pwm_slice_t slice[NUM_PWM_SLICES];
  uint8_t handler_count;
  uint8_t irq_priority;
  bool irq_installed;
} esy_hb_pwm_module_ctx_t;

/* All handles share one task/core owner. The same core's wrap IRQ only reads
 * published bindings; short IRQ-masked updates retire callback contexts. DMA
 * IRQs only stop their exclusively owned slice and publish atomic events. */
static esy_hb_pwm_module_ctx_t s_ctx;

static esy_err_t pwm_compute_period(uint32_t freq_hz, uint16_t *divider,
                                    uint16_t *top) {
  uint32_t clk_hz = clock_get_hz(clk_sys);
  if (freq_hz == 0u || freq_hz > clk_hz) return ESY_ERR_INVALID_ARG;
  const uint64_t scaled_clock = (uint64_t)clk_hz * PWM_DIVIDER_SCALE;
  const uint64_t denominator = (uint64_t)freq_hz * PWM_PERIOD_MAX;
  uint64_t div = (scaled_clock + denominator - 1u) / denominator;
  if (div < PWM_DIVIDER_SCALE) div = PWM_DIVIDER_SCALE;
  if (div > PWM_DIVIDER_MAX) return ESY_ERR_NOT_SUPPORTED;
  uint64_t period = scaled_clock / ((uint64_t)freq_hz * div);
  if (period == 0u || period > PWM_PERIOD_MAX)
    return ESY_ERR_NOT_SUPPORTED;
  *divider = (uint16_t)div;
  *top = (uint16_t)(period - 1u);
  return ESY_OK;
}

static esy_err_t pwm_compute_level(uint16_t top, float duty, uint16_t *level) {
  if (!(duty >= 0.0f && duty <= 1.0f)) return ESY_ERR_INVALID_ARG;
  uint32_t ticks = (uint32_t)(((uint32_t)top + 1u) * (double)duty);
  if (ticks > UINT16_MAX) return ESY_ERR_NOT_SUPPORTED;
  *level = (uint16_t)ticks;
  return ESY_OK;
}

static void pwm_write_timing(uint8_t slice, uint16_t divider, uint16_t top) {
  pwm_set_clkdiv_int_frac4(slice, (uint8_t)(divider / PWM_DIVIDER_SCALE),
                          (uint8_t)(divider % PWM_DIVIDER_SCALE));
  pwm_set_wrap(slice, top);
}

static void pwm_update_irq(uint8_t slice) {
  rp2040_pwm_slice_t *shared = &s_ctx.slice[slice];
  bool enabled = (shared->handler_mask & shared->running_mask) != 0u;
  if (enabled == shared->irq_enabled) return;
  if (enabled) pwm_clear_irq(slice);
  pwm_set_irq_enabled(slice, enabled);
  shared->irq_enabled = enabled;
}

/* Per-pin override hides a stopped channel even while its peer keeps counting.
 * CC writes alone are shadowed and cannot guarantee an immediate idle level. */
static void pwm_update_output(rp2040_pwm_ctx_t *c) {
  rp2040_pwm_slice_t *shared = &s_ctx.slice[c->slice];
  uint8_t bit = (uint8_t)(1u << c->channel);
  if (c->enabled && !c->suspended) {
    shared->running_mask |= bit;
    pwm_set_enabled(c->slice, true);
    gpio_set_outover(c->gpio, GPIO_OVERRIDE_NORMAL);
  } else {
    gpio_set_outover(c->gpio,
                     c->inverted ? GPIO_OVERRIDE_HIGH : GPIO_OVERRIDE_LOW);
    shared->running_mask &= (uint8_t)~bit;
    pwm_set_enabled(c->slice, shared->running_mask != 0u);
  }
  pwm_update_irq(c->slice);
}

/* IRQ callbacks publish only; owner polling retires the shared dispatcher
 * before notifying users or allowing handle/buffer reuse. */
static void pwm_dma_callback_wrapper(void *arg) {
  rp2040_pwm_ctx_t *c = arg;
  atomic_fetch_or_explicit(&c->dma_events, PWM_DMA_DONE, memory_order_release);
}

static void pwm_dma_error_wrapper(void *arg) {
  rp2040_pwm_ctx_t *c = arg;
  gpio_set_outover(c->gpio,
                   c->inverted ? GPIO_OVERRIDE_HIGH : GPIO_OVERRIDE_LOW);
  pwm_set_enabled(c->slice, false);
  atomic_fetch_or_explicit(&c->dma_events, PWM_DMA_ERROR, memory_order_release);
}

static void pwm_dma_clear(rp2040_pwm_ctx_t *c) {
  c->dma_callback = NULL;
  c->dma_callback_arg = NULL;
  c->dma_error_callback = NULL;
  c->dma_error_callback_arg = NULL;
  atomic_store_explicit(&c->dma_events, 0u, memory_order_relaxed);
}

static esy_err_t pwm_dma_retire(rp2040_pwm_ctx_t *c, bool stop_output) {
  if (!c->dma_owned) return ESY_OK;
  if (stop_output) {
    uint32_t key = save_and_disable_interrupts();
    c->enabled = false;
    pwm_update_output(c);
    restore_interrupts(key);
  }
  esy_err_t result = esy_hb_dma_abort(c, c->dma_dreq, c->dma_channel);
  if (result != ESY_OK) return result;
  uint32_t compare = pwm_hw->slice[c->slice].cc;
  compare = (compare >> (c->channel * 16u)) & UINT16_MAX;
  c->duty_cycle = (float)compare /
      ((uint32_t)pwm_hw->slice[c->slice].top + 1u);
  if (c->duty_cycle > 1.0f) c->duty_cycle = 1.0f;
  c->dma_owned = false;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_check_available(uint8_t phys_index) {
  if (phys_index >= RP2040_PWM_MAX) return ESY_ERR_INVALID_ARG;
  return s_ctx.channel[phys_index].bound_pwm == NULL ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_hb_pwm_init(esy_pwm_t *pwm,
                              const esy_pwm_config_t *config) {
  if (!pwm || !config) return ESY_ERR_INVALID_ARG;
  esy_err_t result = esy_hb_pwm_check_available(config->phys_index);
  if (result != ESY_OK) return result;
  if (config->inner.gpio >= NUM_BANK0_GPIOS || config->duty_cycle > 100u)
    return ESY_ERR_INVALID_ARG;

  uint8_t slice = (uint8_t)pwm_gpio_to_slice_num(config->inner.gpio);
  uint8_t ch = (uint8_t)pwm_gpio_to_channel(config->inner.gpio);
  uint16_t divider, top, level;
  result = pwm_compute_period(config->freq_hz, &divider, &top);
  if (result != ESY_OK) return result;
  result = pwm_compute_level(top, (float)config->duty_cycle / 100.0f, &level);
  if (result != ESY_OK) return result;

  /* Distinct GPIOs can alias the same hardware channel. Logical slots alone
   * cannot establish ownership of the physical compare register. */
  for (size_t i = 0u; i < RP2040_PWM_MAX; ++i) {
    const rp2040_pwm_ctx_t *peer = &s_ctx.channel[i];
    if (peer->bound_pwm == NULL || peer->slice != slice) continue;
    if (peer->channel == ch || peer->dma_owned) return ESY_ERR_BUSY;
  }
  rp2040_pwm_slice_t *shared = &s_ctx.slice[slice];
  if (shared->owners != 0u && shared->freq_hz != config->freq_hz)
    return ESY_ERR_BUSY;

  rp2040_pwm_ctx_t *c = &s_ctx.channel[config->phys_index];
  memset(c, 0, sizeof(*c));
  c->slice = slice;
  c->channel = ch;
  c->phys_index = config->phys_index;
  c->gpio = config->inner.gpio;
  c->duty_cycle = (float)config->duty_cycle / 100.0f;
  c->inverted = config->start_low;
  atomic_init(&c->dma_events, 0u);

  uint32_t key = save_and_disable_interrupts();
  if (shared->owners == 0u) {
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac4(&cfg, divider / PWM_DIVIDER_SCALE,
                                    divider % PWM_DIVIDER_SCALE);
    pwm_config_set_wrap(&cfg, top);
    pwm_init(slice, &cfg, false);
    pwm_set_irq_enabled(slice, false);
    pwm_clear_irq(slice);
    shared->freq_hz = config->freq_hz;
  }
  ++shared->owners;
  if (c->inverted) shared->inversion_mask |= (uint8_t)(1u << ch);
  pwm_set_output_polarity(slice, (shared->inversion_mask & 1u) != 0u,
                          (shared->inversion_mask & 2u) != 0u);
  pwm_set_chan_level(slice, ch, level);
  pwm_update_output(c);

  c->initialized = true;
  c->bound_pwm = pwm;
  pwm->ctx = c;
  restore_interrupts(key);
  return ESY_OK;
}

esy_err_t esy_hb_pwm_deinit(esy_pwm_t *pwm) {
  if (!pwm) return ESY_ERR_INVALID_ARG;
  if (!pwm->ctx) return ESY_OK;
  esy_err_t result = esy_hb_pwm_abort_dma(pwm);
  if (result != ESY_OK) return result;
  rp2040_pwm_ctx_t *c = (rp2040_pwm_ctx_t *)pwm->ctx;
  result = esy_hb_pwm_set_wrap_handler(pwm, NULL, NULL, 0u);
  if (result != ESY_OK) return result;
  uint32_t key = save_and_disable_interrupts();
  c->enabled = false;
  pwm_update_output(c);
  pwm_set_chan_level(c->slice, c->channel, 0);
  rp2040_pwm_slice_t *shared = &s_ctx.slice[c->slice];
  shared->inversion_mask &= (uint8_t)~(1u << c->channel);
  pwm_set_output_polarity(c->slice, (shared->inversion_mask & 1u) != 0u,
                          (shared->inversion_mask & 2u) != 0u);
  --shared->owners;
  if (shared->owners == 0u) memset(shared, 0, sizeof(*shared));

  c->initialized = false;
  c->bound_pwm = NULL;
  pwm->ctx = NULL;
  restore_interrupts(key);
  return ESY_OK;
}

esy_err_t esy_hb_pwm_set_duty(esy_pwm_t *pwm, float duty_cycle) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = (rp2040_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  uint16_t level;
  esy_err_t result = pwm_compute_level((uint16_t)pwm_hw->slice[c->slice].top,
                                      duty_cycle, &level);
  if (result != ESY_OK) return result;
  pwm_set_chan_level(c->slice, c->channel, level);
  c->duty_cycle = duty_cycle;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz) {
  if (!pwm || !pwm->ctx || !freq_hz) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = (rp2040_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  rp2040_pwm_slice_t *shared = &s_ctx.slice[c->slice];
  if (shared->freq_hz == freq_hz) return ESY_OK;
  if (shared->owners > 1u) return ESY_ERR_BUSY;
  uint16_t divider, top, level;
  esy_err_t result = pwm_compute_period(freq_hz, &divider, &top);
  if (result != ESY_OK) return result;
  result = pwm_compute_level(top, c->duty_cycle, &level);
  if (result != ESY_OK) return result;
  pwm_write_timing(c->slice, divider, top);
  pwm_set_chan_level(c->slice, c->channel, level);
  shared->freq_hz = freq_hz;

  return ESY_OK;
}

esy_err_t esy_hb_pwm_enable(esy_pwm_t *pwm, bool enable) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = (rp2040_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;
  if (c->suspended) return ESY_ERR_STATE;
  uint32_t key = save_and_disable_interrupts();
  c->enabled = enable;
  pwm_update_output(c);
  restore_interrupts(key);
  return ESY_OK;
}

esy_err_t esy_hb_pwm_set_polarity(esy_pwm_t *pwm, bool start_low) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = (rp2040_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;

  rp2040_pwm_slice_t *shared = &s_ctx.slice[c->slice];
  uint8_t bit = (uint8_t)(1u << c->channel);
  uint32_t key = save_and_disable_interrupts();
  c->inverted = start_low;
  if (start_low) shared->inversion_mask |= bit;
  else shared->inversion_mask &= (uint8_t)~bit;
  pwm_set_output_polarity(c->slice, (shared->inversion_mask & 1u) != 0u,
                          (shared->inversion_mask & 2u) != 0u);
  pwm_update_output(c);
  restore_interrupts(key);
  return ESY_OK;
}

esy_err_t esy_hb_pwm_enable_trigger_output(esy_pwm_t *pwm, bool enabled) {
  (void)pwm;
  (void)enabled;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_dma_output(esy_pwm_t *pwm,
                                 esy_dma_config_t *dma_config, bool loop) {
  if (!pwm || !pwm->ctx || !dma_config || !dma_config->src ||
      dma_config->length == 0u) return ESY_ERR_INVALID_ARG;
  if (loop || dma_config->circular) return ESY_ERR_NOT_SUPPORTED;
  if (dma_config->transfer_size != ESY_DMA_TRANSFER_SIZE_16)
    return ESY_ERR_NOT_SUPPORTED;
  rp2040_pwm_ctx_t *c = pwm->ctx;
  if (c->dma_owned || c->enabled || s_ctx.slice[c->slice].owners != 1u)
    return ESY_ERR_BUSY;
  if (c->suspended) return ESY_ERR_STATE;
  dma_config->inner.dreq = (uint32_t)(DREQ_PWM_WRAP0 + c->slice);
  dma_config->inner.direction = ESY_DMA_MEM_TO_PERIPH;
  uint8_t resource;
  esy_err_t result = esy_hb_dma_get_resource_id(dma_config->inner.dreq,
                                               dma_config->inner.channel,
                                               &resource);
  if (result != ESY_OK) return result;
  for (size_t i = 0; i < RP2040_PWM_MAX; ++i) {
    if (s_ctx.channel[i].dma_owned && s_ctx.channel[i].dma_resource == resource)
      return ESY_ERR_BUSY;
  }
  if (esy_hb_dma_is_busy(dma_config->inner.dreq, dma_config->inner.channel))
    return ESY_ERR_BUSY;
  dma_config->dst = (void *)((uintptr_t)&pwm_hw->slice[c->slice].cc +
                            (c->channel == 0 ? 0u : sizeof(uint16_t)));
  c->dma_callback = dma_config->callback;
  c->dma_callback_arg = dma_config->callback_arg;
  c->dma_error_callback = dma_config->error_callback;
  c->dma_error_callback_arg = dma_config->error_callback_arg;
  dma_config->callback = pwm_dma_callback_wrapper;
  dma_config->callback_arg = c;
  dma_config->error_callback = pwm_dma_error_wrapper;
  dma_config->error_callback_arg = c;
  atomic_store_explicit(&c->dma_events, 0u, memory_order_relaxed);
  result = esy_hb_dma_prepare(c, dma_config);
  if (result != ESY_OK) {
    pwm_dma_clear(c);
    return result;
  }
  c->dma_dreq = dma_config->inner.dreq;
  c->dma_channel = dma_config->inner.channel;
  c->dma_resource = resource;
  c->dma_owned = true;
  uint32_t key = save_and_disable_interrupts();
  c->enabled = true;
  pwm_update_output(c);
  restore_interrupts(key);
  result = esy_hb_dma_start(c, 1u << resource);
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_hb_pwm_abort_dma(pwm);
    return cleanup == ESY_OK ? result : cleanup;
  }
  return ESY_OK;
}

esy_err_t esy_hb_pwm_abort_dma(esy_pwm_t *pwm) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = pwm->ctx;
  esy_err_t result = pwm_dma_retire(c, true);
  if (result != ESY_OK) return result;
  pwm_dma_clear(c);
  return ESY_OK;
}

esy_err_t esy_hb_pwm_poll(esy_pwm_t *pwm) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = pwm->ctx;
  if (!c->dma_owned) return ESY_OK;
  unsigned events = atomic_load_explicit(&c->dma_events, memory_order_acquire);
  if (events == 0u) return ESY_OK;
  esy_err_t result = pwm_dma_retire(c, (events & PWM_DMA_ERROR) != 0u);
  if (result != ESY_OK) return result;
  events = atomic_load_explicit(&c->dma_events, memory_order_acquire);
  if (events & PWM_DMA_ERROR) {
    uint32_t key = save_and_disable_interrupts();
    c->enabled = false;
    pwm_update_output(c);
    restore_interrupts(key);
  }
  esy_dma_callback_t callback = events & PWM_DMA_ERROR
      ? c->dma_error_callback : c->dma_callback;
  void *arg = events & PWM_DMA_ERROR
      ? c->dma_error_callback_arg : c->dma_callback_arg;
  pwm_dma_clear(c);
  if (callback != NULL) callback(arg);
  return ESY_OK;
}

static void pwm_wrap_isr(void) {
  uint32_t status = pwm_get_irq_status_mask();
  for (uint8_t slice = 0u; slice < NUM_PWM_SLICES; ++slice) {
    if (status & (1u << slice)) pwm_clear_irq(slice);
  }
  for (uint8_t i = 0; i < RP2040_PWM_MAX; ++i) {
    rp2040_pwm_ctx_t *c = &s_ctx.channel[i];
    if (!c->initialized || !c->enabled || c->suspended) continue;
    if ((status & (1u << c->slice)) && c->handler)
      c->handler(c->handler_arg);
  }
}

esy_err_t esy_hb_pwm_set_wrap_handler(esy_pwm_t *pwm,
                                          void (*handler)(void *),
                                          void *arg,
                                          uint8_t irq_priority) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = (rp2040_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;
  unsigned peers = s_ctx.handler_count - (c->handler != NULL ? 1u : 0u);
  if (handler != NULL && peers != 0u && s_ctx.irq_priority != irq_priority)
    return ESY_ERR_BUSY;

  /* Same-core task/ISR binding publication; lifecycle and control calls never
   * run inside a wrap callback. No user callback executes under this mask. */
  uint32_t key = save_and_disable_interrupts();
  if (c->handler != NULL) --s_ctx.handler_count;
  c->handler = handler;
  c->handler_arg = handler != NULL ? arg : NULL;
  rp2040_pwm_slice_t *shared = &s_ctx.slice[c->slice];
  uint8_t bit = (uint8_t)(1u << c->channel);
  if (handler != NULL) {
    ++s_ctx.handler_count;
    shared->handler_mask |= bit;
  } else {
    shared->handler_mask &= (uint8_t)~bit;
  }
  if (handler != NULL && !s_ctx.irq_installed) {
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_wrap_isr);
    s_ctx.irq_installed = true;
  }
  if (handler != NULL) {
    irq_set_priority(PWM_IRQ_WRAP, irq_priority);
    s_ctx.irq_priority = irq_priority;
    irq_set_enabled(PWM_IRQ_WRAP, true);
  }
  pwm_update_irq(c->slice);
  if (s_ctx.handler_count == 0u && s_ctx.irq_installed) {
    irq_set_enabled(PWM_IRQ_WRAP, false);
    irq_remove_handler(PWM_IRQ_WRAP, pwm_wrap_isr);
    s_ctx.irq_installed = false;
  }
  restore_interrupts(key);
  return ESY_OK;
}

esy_err_t esy_hb_pwm_suspend(esy_pwm_t *pwm) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = (rp2040_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;
  if (c->suspended) return ESY_ERR_STATE;
  uint32_t key = save_and_disable_interrupts();
  c->suspended = true;
  pwm_update_output(c);
  restore_interrupts(key);
  return ESY_OK;
}

esy_err_t esy_hb_pwm_resume(esy_pwm_t *pwm) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  rp2040_pwm_ctx_t *c = (rp2040_pwm_ctx_t *)pwm->ctx;
  if (c->dma_owned) return ESY_ERR_BUSY;
  if (!c->suspended) return ESY_ERR_STATE;
  uint32_t key = save_and_disable_interrupts();
  c->suspended = false;
  pwm_update_output(c);
  restore_interrupts(key);
  return ESY_OK;
}

esy_err_t esy_hb_pwm_set_low_power_mode(esy_pwm_t *pwm,
                                            bool enable_lprun) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  (void)enable_lprun;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_set_clock_source(esy_pwm_t *pwm,
                                          uint8_t clock_source) {
  if (!pwm || !pwm->ctx) return ESY_ERR_INVALID_ARG;
  (void)clock_source;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_set_dead_time(esy_pwm_t *pwm, uint32_t dead_time_ns) {
  (void)pwm;
  (void)dead_time_ns;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_enable_complementary(esy_pwm_t *pwm, bool enable) {
  (void)pwm;
  (void)enable;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_get_caps(const esy_pwm_t *pwm, uint32_t *out_caps) {
  if (!pwm || !pwm->ctx || !out_caps) return ESY_ERR_INVALID_ARG;
  *out_caps = ESY_PWM_CAP_DMA_OUTPUT | ESY_PWM_CAP_WRAP_IRQ |
              ESY_PWM_CAP_POLARITY;
  return ESY_OK;
}

esy_err_t esy_hb_pwm_get_phys_index(const esy_pwm_t *pwm,
                                        uint8_t *out_index) {
  if (!pwm || !pwm->ctx || !out_index) return ESY_ERR_INVALID_ARG;
  const rp2040_pwm_ctx_t *c = (const rp2040_pwm_ctx_t *)pwm->ctx;
  *out_index = c->phys_index;
  return ESY_OK;
}

void esy_hb_pwm_handle_irq(uint32_t index) {
  if (index >= RP2040_PWM_MAX) {
    return;
  }

  const rp2040_pwm_ctx_t *c = (const rp2040_pwm_ctx_t *)&s_ctx.channel[index];

  if (!c->initialized || !c->enabled || c->suspended) {
    return;
  }

  if (c->handler) {
    c->handler(c->handler_arg);
  }
}
