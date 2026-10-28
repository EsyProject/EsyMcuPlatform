/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_pwm.h"
#include "esy_common/defs/base/esy_common_defs.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include <string.h>

#define MAX_PWM_CHANNELS 8
#define PWM_CHANNELS_PER_TIMER 2u
#define DEFAULT_PWM_RESOLUTION LEDC_TIMER_13_BIT

typedef struct {
    uint8_t phys_index;
    uint8_t gpio_num;
    uint8_t channel;
    float duty_cycle;
    bool enabled;
    bool channel_configured;
    bool start_low;
    ledc_timer_t timer_num;
    ledc_channel_t ledc_channel;
    void (*wrap_handler)(void *);
    void *wrap_arg;
    uint8_t irq_priority;
    intr_handle_t wrap_interrupt;
    esy_pwm_t *bound_pwm;
} esy_pwm_esp32_ctx_t;

typedef struct {
    uint32_t freq_hz;
    uint8_t owners;
    bool release_pending;
} esy_pwm_esp32_timer_t;

typedef struct {
    esy_pwm_esp32_ctx_t pwm_contexts[MAX_PWM_CHANNELS];
    esy_pwm_esp32_timer_t timers[LEDC_TIMER_MAX];
} esy_hb_pwm_ctx_t;

/* One externally serialized owner coordinates every channel and shared timer. */
static esy_hb_pwm_ctx_t s_ctx;

static ledc_timer_t get_timer_for_channel(uint8_t channel) {
    return (ledc_timer_t)(channel / PWM_CHANNELS_PER_TIMER);
}

static uint32_t duty_to_ledc_duty(float duty_cycle, ledc_timer_bit_t resolution) {
    if (duty_cycle < 0.0f) duty_cycle = 0.0f;
    if (duty_cycle > 1.0f) duty_cycle = 1.0f;
    
    uint32_t max_duty = (1 << resolution) - 1;
    return (uint32_t)(duty_cycle * max_duty);
}

static esy_err_t acquire_timer(ledc_timer_t timer_num, uint32_t freq_hz) {
    esy_pwm_esp32_timer_t *timer = &s_ctx.timers[timer_num];
    if (timer->release_pending) return ESY_ERR_BUSY;
    if (timer->owners != 0u) {
        if (timer->freq_hz != freq_hz) return ESY_ERR_BUSY;
        ++timer->owners;
        return ESY_OK;
    }
    
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = DEFAULT_PWM_RESOLUTION,
        .freq_hz = freq_hz,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = timer_num,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        return ESY_ERR_FAIL;
    }
    
    timer->freq_hz = freq_hz;
    timer->owners = 1u;
    return ESY_OK;
}

static esy_err_t release_timer(ledc_timer_t timer_num) {
    esy_pwm_esp32_timer_t *timer = &s_ctx.timers[timer_num];
    if (timer->owners > 1u) {
        --timer->owners;
        return ESY_OK;
    }
    /* Retain the last claim until the SDK has released its clock ownership. */
    timer->release_pending = true;
    if (ledc_timer_pause(LEDC_LOW_SPEED_MODE, timer_num) != ESP_OK)
        return ESY_ERR_FAIL;
    const ledc_timer_config_t config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = timer_num,
        .deconfigure = true,
    };
    if (ledc_timer_config(&config) != ESP_OK) return ESY_ERR_FAIL;
    *timer = (esy_pwm_esp32_timer_t){0};
    return ESY_OK;
}

esy_err_t esy_hb_pwm_check_available(uint8_t phys_index) {
    if (phys_index >= MAX_PWM_CHANNELS) return ESY_ERR_INVALID_ARG;
    return s_ctx.pwm_contexts[phys_index].bound_pwm == NULL
        ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_hb_pwm_init(esy_pwm_t *pwm, const esy_pwm_config_t *config) {
    if (!pwm || !config) {
        return ESY_ERR_INVALID_ARG;
    }
    
    if (config->phys_index >= MAX_PWM_CHANNELS || config->freq_hz == 0u ||
        config->duty_cycle > 100u || !GPIO_IS_VALID_OUTPUT_GPIO(config->inner.gpio)) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esy_pwm_esp32_ctx_t *ctx = &s_ctx.pwm_contexts[config->phys_index];
    if (ctx->bound_pwm != NULL) return ESY_ERR_BUSY;
    ledc_timer_t timer_num = get_timer_for_channel(config->phys_index);
    esy_err_t err = acquire_timer(timer_num, config->freq_hz);
    if (err != ESY_OK) return err;

    memset(ctx, 0, sizeof(*ctx));
    ctx->phys_index = config->phys_index;
    ctx->gpio_num = config->inner.gpio;
    ctx->channel = config->inner.channel;
    ctx->duty_cycle = (float)config->duty_cycle / 100.0f;
    ctx->enabled = false;
    ctx->start_low = config->start_low;
    ctx->ledc_channel = (ledc_channel_t)config->phys_index;
    ctx->timer_num = timer_num;
    ctx->wrap_handler = NULL;
    ctx->wrap_arg = NULL;
    ctx->irq_priority = 0;
    
    /* Publish acquired resources before later SDK calls can require cleanup. */
    ctx->bound_pwm = pwm;
    pwm->ctx = ctx;
    
    ledc_channel_config_t ledc_channel = {
        .channel = ctx->ledc_channel,
        .duty = 0u,
        .gpio_num = ctx->gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = ctx->timer_num,
        .flags.output_invert = ctx->start_low ? 1 : 0
    };
    
    esp_err_t ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        /* The supported SDK rejects before programming the channel. */
        err = esy_hb_pwm_deinit(pwm);
        return err == ESY_OK ? ESY_ERR_FAIL : err;
    }
    ctx->channel_configured = true;
    if (ledc_stop(LEDC_LOW_SPEED_MODE, ctx->ledc_channel,
                  ctx->start_low ? 0u : 1u) != ESP_OK) {
        err = esy_hb_pwm_deinit(pwm);
        return err == ESY_OK ? ESY_ERR_FAIL : err;
    }
    return ESY_OK;
}

esy_err_t esy_hb_pwm_deinit(esy_pwm_t *pwm) {
    if (!pwm) return ESY_ERR_INVALID_ARG;
    if (!pwm->ctx) return ESY_OK;
    
    esy_pwm_esp32_ctx_t *ctx = (esy_pwm_esp32_ctx_t *)pwm->ctx;
    esy_pwm_esp32_timer_t *timer = &s_ctx.timers[ctx->timer_num];
    if (timer->owners == 1u) timer->release_pending = true;
    
    if (ctx->channel_configured &&
        ledc_stop(LEDC_LOW_SPEED_MODE, ctx->ledc_channel, 0) != ESP_OK)
        return ESY_ERR_FAIL;
    if (ctx->wrap_interrupt != NULL) {
        if (esp_intr_free(ctx->wrap_interrupt) != ESP_OK) return ESY_ERR_FAIL;
        ctx->wrap_interrupt = NULL;
        ctx->wrap_handler = NULL;
        ctx->wrap_arg = NULL;
    }
    esy_err_t err = release_timer(ctx->timer_num);
    if (err != ESY_OK) return err;

    memset(ctx, 0, sizeof(*ctx));
    pwm->ctx = NULL;
    return ESY_OK;
}

esy_err_t esy_hb_pwm_set_duty(esy_pwm_t *pwm, float duty_cycle) {
    if (!pwm || !pwm->ctx) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esy_pwm_esp32_ctx_t *ctx = (esy_pwm_esp32_ctx_t *)pwm->ctx;
    
    if (duty_cycle < 0.0f || duty_cycle > 1.0f) {
        return ESY_ERR_INVALID_ARG;
    }
    /* Updating LEDC duty also enables its physical output. Keep disabled
     * channels untouched, as required by the public PWM state contract. */
    if (!ctx->enabled) return ESY_ERR_STATE;
    
    uint32_t duty = duty_to_ledc_duty(duty_cycle, DEFAULT_PWM_RESOLUTION);
    
    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, ctx->ledc_channel, duty);
    if (ret != ESP_OK) {
        return ESY_ERR_FAIL;
    }
    ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, ctx->ledc_channel);
    if (ret != ESP_OK) {
        return ESY_ERR_FAIL;
    }
    ctx->duty_cycle = duty_cycle;
    return ESY_OK;
}

esy_err_t esy_hb_pwm_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz) {
    if (!pwm || !pwm->ctx) {
        return ESY_ERR_INVALID_ARG;
    }
    
    if (freq_hz == 0) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esy_pwm_esp32_ctx_t *ctx = (esy_pwm_esp32_ctx_t *)pwm->ctx;
    esy_pwm_esp32_timer_t *timer = &s_ctx.timers[ctx->timer_num];
    if (timer->release_pending) return ESY_ERR_BUSY;
    if (timer->freq_hz == freq_hz) return ESY_OK;
    if (timer->owners > 1u) return ESY_ERR_BUSY;
    
    esp_err_t ret = ledc_set_freq(LEDC_LOW_SPEED_MODE, ctx->timer_num, freq_hz);
    if (ret != ESP_OK) {
        return ESY_ERR_FAIL;
    }
    timer->freq_hz = freq_hz;
    return ESY_OK;
}

esy_err_t esy_hb_pwm_enable(esy_pwm_t *pwm, bool enable) {
    if (!pwm || !pwm->ctx) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esy_pwm_esp32_ctx_t *ctx = (esy_pwm_esp32_ctx_t *)pwm->ctx;
    
    if (enable && !ctx->enabled) {
        uint32_t duty = duty_to_ledc_duty(ctx->duty_cycle, DEFAULT_PWM_RESOLUTION);
        esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, ctx->ledc_channel, duty);
        if (ret != ESP_OK) {
            return ESY_ERR_FAIL;
        }
        
        ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, ctx->ledc_channel);
        if (ret != ESP_OK) {
            return ESY_ERR_FAIL;
        }
        
        ctx->enabled = true;
    } else if (!enable && ctx->enabled) {
        esp_err_t ret = ledc_stop(LEDC_LOW_SPEED_MODE, ctx->ledc_channel, ctx->start_low ? 0 : 1);
        if (ret != ESP_OK) {
            return ESY_ERR_FAIL;
        }
        
        ctx->enabled = false;
    }
    
    return ESY_OK;
}

esy_err_t esy_hb_pwm_set_polarity(esy_pwm_t *pwm, bool start_low) {
    if (!pwm || !pwm->ctx) {
        return ESY_ERR_INVALID_ARG;
    }
    
    (void)start_low;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_enable_trigger_output(esy_pwm_t *pwm, bool enabled) {
    (void)pwm;
    (void)enabled;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_dma_output(esy_pwm_t *pwm, esy_dma_config_t *dma_config, bool loop) {
    (void)pwm;
    (void)dma_config;
    (void)loop;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_abort_dma(esy_pwm_t *pwm) {
    return pwm != NULL && pwm->ctx != NULL ? ESY_OK : ESY_ERR_INVALID_ARG;
}

esy_err_t esy_hb_pwm_poll(esy_pwm_t *pwm) {
    return pwm != NULL && pwm->ctx != NULL ? ESY_OK : ESY_ERR_INVALID_ARG;
}

esy_err_t esy_hb_pwm_set_wrap_handler(esy_pwm_t *pwm, void (*handler)(void *), void *arg, uint8_t irq_priority) {
    if (!pwm || !pwm->ctx) {
        return ESY_ERR_INVALID_ARG;
    }
    
    /* LEDC exposes fade completion, not a counter-wrap event. Never borrow a
     * callback or allocate an unrelated native ISR for an unsupported request. */
    if (handler != NULL) return ESY_ERR_NOT_SUPPORTED;
    (void)arg;
    (void)irq_priority;

    esy_pwm_esp32_ctx_t *ctx = (esy_pwm_esp32_ctx_t *)pwm->ctx;
    
    if (ctx->wrap_interrupt != NULL) {
        if (esp_intr_free(ctx->wrap_interrupt) != ESP_OK) return ESY_ERR_FAIL;
        ctx->wrap_interrupt = NULL;
    }
    ctx->wrap_handler = NULL;
    ctx->wrap_arg = NULL;
    ctx->irq_priority = 0u;

    return ESY_OK;
}

esy_err_t esy_hb_pwm_suspend(esy_pwm_t *pwm) {
    (void)pwm;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_resume(esy_pwm_t *pwm) {
    (void)pwm;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_set_low_power_mode(esy_pwm_t *pwm, bool enable_lprun) {
    (void)pwm;
    (void)enable_lprun;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_pwm_set_clock_source(esy_pwm_t *pwm, uint8_t clock_source) {
    (void)pwm;
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

void esy_hb_pwm_handle_irq(uint32_t index) {
    if (index >= MAX_PWM_CHANNELS) {
        return;
    }
    
    esy_pwm_esp32_ctx_t *ctx = &s_ctx.pwm_contexts[index];
    
    if (ctx->wrap_handler) {
        ctx->wrap_handler(ctx->wrap_arg);
    }
}

esy_err_t esy_hb_pwm_get_caps(const esy_pwm_t *pwm, uint32_t *out_caps) {
    if (!pwm || !pwm->ctx || !out_caps) {
        return ESY_ERR_INVALID_ARG;
    }
    
    /* No optional ESY_PWM_CAP_* operation is implemented by this port. */
    *out_caps = 0;
    
    return ESY_OK;
}

esy_err_t esy_hb_pwm_get_phys_index(const esy_pwm_t *pwm, uint8_t *out_index) {
    if (!pwm || !pwm->ctx || !out_index) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esy_pwm_esp32_ctx_t *ctx = (esy_pwm_esp32_ctx_t *)pwm->ctx;
    *out_index = ctx->phys_index;
    
    return ESY_OK;
}
