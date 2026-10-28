/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_pwm.h"
#include <string.h>

#ifndef FAKE_ESY_HAL_PWM_MAX_INSTANCES
#define FAKE_ESY_HAL_PWM_MAX_INSTANCES 8
#endif

typedef struct
{
    esy_pwm_t *pwm;
    bool       in_use;

    bool       initialized;
    const struct esy_device_s *device;
    bool dma_active;
    bool dma_circular;
    bool callback_running;
    esy_dma_callback_t dma_complete_cb;
    void *dma_complete_arg;
    esy_dma_callback_t dma_half_cb;
    void *dma_half_arg;
    esy_dma_callback_t dma_error_cb;
    void *dma_error_arg;
    float      duty_cycle;
    uint32_t   freq_hz;
    bool       enabled;
    bool       start_low;

    /* Wrap callback state. */
    void     (*wrap_handler)(void *);
    void      *wrap_arg;

    /* Trigger output / DMA flags kept for future assertions. */
    bool       trigger_enabled;
    uint8_t    phys_index;
    bool       dma_event_pending;
    bool       dma_error_pending;
} fake_pwm_slot_t;

typedef struct {
    fake_pwm_slot_t slots[FAKE_ESY_HAL_PWM_MAX_INSTANCES];
    esy_err_t dma_result;
    esy_err_t abort_result;
    esy_err_t deinit_result;
} fake_esy_hal_pwm_ctx_t;

static fake_esy_hal_pwm_ctx_t s_ctx = {.dma_result = ESY_ERR_NOT_SUPPORTED};

static fake_pwm_slot_t *fake_pwm_get_slot(esy_pwm_t *pwm, bool create_if_missing)
{
    if (pwm == NULL) return NULL;
    /* Find an existing slot. */
    for (int i = 0; i < FAKE_ESY_HAL_PWM_MAX_INSTANCES; ++i) {
        if (s_ctx.slots[i].in_use && s_ctx.slots[i].pwm == pwm) {
            return &s_ctx.slots[i];
        }
    }

    if (!create_if_missing) {
        return NULL;
    }

    /* Create a new slot. */
    for (int i = 0; i < FAKE_ESY_HAL_PWM_MAX_INSTANCES; ++i) {
        if (!s_ctx.slots[i].in_use) {
            memset(&s_ctx.slots[i], 0, sizeof(s_ctx.slots[i]));
            s_ctx.slots[i].in_use = true;
            s_ctx.slots[i].pwm = pwm;
            return &s_ctx.slots[i];
        }
    }

    return NULL;
}

void fake_esy_hal_pwm_reset(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.dma_result = ESY_ERR_NOT_SUPPORTED;
}

static const esy_pwm_vtable_t k_fake_pwm_ops = {
        .set_duty = esy_hal_pwm_set_duty,
        .set_wrap = esy_hal_pwm_set_wrap,
        .enable = esy_hal_pwm_enable,
        .set_polarity = esy_hal_pwm_set_polarity,
        .enable_trigger_output = esy_hal_pwm_enable_trigger_output,
        .dma_output = esy_hal_pwm_dma_output,
        .abort_dma = esy_hal_pwm_abort_dma,
        .poll = esy_hal_pwm_poll,
        .set_wrap_handler = esy_hal_pwm_set_wrap_handler,
        .suspend = esy_hal_pwm_suspend,
        .resume = esy_hal_pwm_resume,
        .set_low_power_mode = esy_hal_pwm_set_low_power_mode,
        .set_clock_source = esy_hal_pwm_set_clock_source,
        .get_caps = esy_hal_pwm_get_caps,
        .get_phys_index = esy_hal_pwm_get_phys_index,
    };


/* Fake implementation of the HAL PWM API. */

void esy_hal_pwm_default_cfg(esy_pwm_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    /* Provide conservative defaults. */
    cfg->freq_hz = 1000u;
    cfg->duty_cycle = 50;
    cfg->start_low = false;
    /* Other fields can be added when the public config grows. */
}

esy_err_t esy_hal_pwm_init(esy_pwm_t *pwm, const esy_pwm_config_t *cfg)
{
    if (pwm == NULL || cfg == NULL) return ESY_ERR_INVALID_ARG;
    if (pwm->ops != NULL) return ESY_ERR_STATE;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, true);
    if (slot == NULL) return ESY_ERR_NO_MEM;
    if (slot->initialized || slot->device != NULL) return ESY_ERR_STATE;
    slot->initialized = true;
    slot->phys_index = cfg->phys_index;
    slot->freq_hz = cfg->freq_hz;
    slot->duty_cycle = (float)cfg->duty_cycle / 100.0f;
    slot->start_low = cfg->start_low;
    pwm->ops = &k_fake_pwm_ops;
    return ESY_OK;
}

static void fake_pwm_clear_dma(fake_pwm_slot_t *slot)
{
    slot->dma_active = false;
    slot->dma_circular = false;
    slot->dma_complete_cb = NULL;
    slot->dma_complete_arg = NULL;
    slot->dma_half_cb = NULL;
    slot->dma_half_arg = NULL;
    slot->dma_error_cb = NULL;
    slot->dma_error_arg = NULL;
    slot->dma_event_pending = false;
    slot->dma_error_pending = false;
}

esy_err_t esy_hal_pwm_abort_dma(esy_pwm_t *pwm)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_SUPPORTED;
    if (slot->callback_running) return ESY_ERR_BUSY;
    if (!slot->dma_active) return ESY_OK;
    if (s_ctx.abort_result != ESY_OK) return s_ctx.abort_result;
    slot->enabled = false;
    fake_pwm_clear_dma(slot);
    return ESY_OK;
}

esy_err_t esy_hal_pwm_poll(esy_pwm_t *pwm)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_SUPPORTED;
    if (slot->callback_running) return ESY_ERR_BUSY;
    if (!slot->dma_active || !slot->dma_event_pending) return ESY_OK;
    bool error = slot->dma_error_pending;
    bool terminal = error || !slot->dma_circular;
    if (terminal && s_ctx.abort_result != ESY_OK) return s_ctx.abort_result;
    esy_dma_callback_t callback = error ? slot->dma_error_cb : slot->dma_complete_cb;
    void *arg = error ? slot->dma_error_arg : slot->dma_complete_arg;
    slot->dma_event_pending = false;
    slot->dma_error_pending = false;
    if (error) {
        slot->enabled = false;
    }
    slot->callback_running = true;
    if (callback != NULL) callback(arg);
    slot->callback_running = false;
    if (terminal) fake_pwm_clear_dma(slot);
    return ESY_OK;
}

esy_err_t esy_hal_pwm_deinit(esy_pwm_t *pwm)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized)
        return pwm->ops == NULL ? ESY_OK : ESY_ERR_STATE;
    if (slot->device != NULL) return ESY_ERR_STATE;
    if (slot->callback_running) return ESY_ERR_BUSY;
    esy_err_t result = esy_hal_pwm_abort_dma(pwm);
    if (result != ESY_OK) return result;
    if (s_ctx.deinit_result != ESY_OK) return s_ctx.deinit_result;
    memset(slot, 0, sizeof(*slot));
    memset(pwm, 0, sizeof(*pwm));
    return ESY_OK;
}

esy_err_t esy_hal_pwm_set_duty(esy_pwm_t *pwm, float duty_cycle)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    if (slot->dma_active || slot->callback_running) return ESY_ERR_BUSY;
    if (!(duty_cycle >= 0.0f && duty_cycle <= 1.0f)) return ESY_ERR_INVALID_ARG;
    slot->duty_cycle = duty_cycle;
    return ESY_OK;
}

esy_err_t esy_hal_pwm_set_wrap(esy_pwm_t *pwm, uint32_t freq_hz)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    if (slot->dma_active || slot->callback_running) return ESY_ERR_BUSY;
    if (freq_hz == 0u) return ESY_ERR_INVALID_ARG;
    slot->freq_hz = freq_hz;
    return ESY_OK;
}

esy_err_t esy_hal_pwm_enable(esy_pwm_t *pwm, bool enable)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    if (slot->dma_active || slot->callback_running) return ESY_ERR_BUSY;
    slot->enabled = enable;
    return ESY_OK;
}

esy_err_t esy_hal_pwm_set_polarity(esy_pwm_t *pwm, bool start_low)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    if (slot->dma_active || slot->callback_running) return ESY_ERR_BUSY;
    slot->start_low = start_low;
    return ESY_OK;
}

esy_err_t esy_hal_pwm_enable_trigger_output(esy_pwm_t *pwm,
                                                 bool enabled)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    if (slot->dma_active || slot->callback_running) return ESY_ERR_BUSY;
    slot->trigger_enabled = enabled;
    /* The fake reports support for this capability. */
    return ESY_OK;
}

esy_err_t esy_hal_pwm_dma_output(esy_pwm_t *pwm,
                                  esy_dma_config_t *dma_cfg, bool loop)
{
    if (pwm == NULL || dma_cfg == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_SUPPORTED;
    if (slot->dma_active || slot->callback_running || slot->enabled)
        return ESY_ERR_BUSY;
    if (s_ctx.dma_result == ESY_ERR_NOT_SUPPORTED) return s_ctx.dma_result;
    if (dma_cfg->src == NULL || dma_cfg->length == 0u) return ESY_ERR_INVALID_ARG;
    slot->dma_active = true;
    slot->dma_circular = loop || dma_cfg->circular;
    slot->dma_complete_cb = dma_cfg->callback;
    slot->dma_complete_arg = dma_cfg->callback_arg;
    slot->dma_half_cb = dma_cfg->half_callback;
    slot->dma_half_arg = dma_cfg->half_callback_arg;
    slot->dma_error_cb = dma_cfg->error_callback;
    slot->dma_error_arg = dma_cfg->error_callback_arg;
    slot->enabled = true;
    if (s_ctx.dma_result != ESY_OK) {
        esy_err_t result = esy_hal_pwm_abort_dma(pwm);
        return result == ESY_OK ? s_ctx.dma_result : result;
    }
    return ESY_OK;
}

esy_err_t esy_hal_pwm_set_wrap_handler(esy_pwm_t *pwm,
                                            void (*handler)(void *),
                                            void *arg)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    if (slot->dma_active || slot->callback_running) return ESY_ERR_BUSY;
    slot->wrap_handler = handler;
    slot->wrap_arg = arg;
    return ESY_OK;
}

esy_err_t esy_hal_pwm_suspend(esy_pwm_t *pwm)
{
    (void)pwm;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_pwm_resume(esy_pwm_t *pwm)
{
    (void)pwm;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_pwm_set_low_power_mode(esy_pwm_t *pwm,
                                              bool enable_lprun)
{
    (void)pwm;
    (void)enable_lprun;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_pwm_set_clock_source(esy_pwm_t *pwm,
                                            uint8_t clock_source)
{
    (void)pwm;
    (void)clock_source;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_pwm_get_caps(const esy_pwm_t *pwm,
                                    uint32_t *out_caps)
{
    if (pwm == NULL || out_caps == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot((esy_pwm_t *)pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    uint32_t caps = ESY_PWM_CAP_WRAP_IRQ | ESY_PWM_CAP_POLARITY;
    if (s_ctx.dma_result != ESY_ERR_NOT_SUPPORTED)
        caps |= ESY_PWM_CAP_DMA_OUTPUT;
    *out_caps = caps;
    return ESY_OK;
}

esy_err_t esy_hal_pwm_get_phys_index(const esy_pwm_t *pwm,
                                      uint8_t *out_index)
{
    if (pwm == NULL || out_index == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot((esy_pwm_t *)pwm, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    *out_index = slot->phys_index;
    return ESY_OK;
}

void esy_hal_pwm_handle_irq(uint32_t index)
{
    (void)index;
    /* Real platforms may find the slot by index and trigger wrap. Tests
     * usually call fake_esy_hal_pwm_invoke_wrap_handler() directly.
     */
}

/* Fake extension API implementation. */

bool fake_esy_hal_pwm_is_initialized(esy_pwm_t *pwm)
{
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    return slot ? slot->initialized : false;
}

float fake_esy_hal_pwm_get_duty(esy_pwm_t *pwm)
{
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    return slot ? slot->duty_cycle : 0.0f;
}

uint32_t fake_esy_hal_pwm_get_freq(esy_pwm_t *pwm)
{
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    return slot ? slot->freq_hz : 0u;
}

bool fake_esy_hal_pwm_is_enabled(esy_pwm_t *pwm)
{
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    return slot ? slot->enabled : false;
}

void fake_esy_hal_pwm_invoke_wrap_handler(esy_pwm_t *pwm)
{
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (!slot) {
        return;
    }
    if (slot->wrap_handler) {
        slot->wrap_handler(slot->wrap_arg);
    }
}

void fake_esy_hal_pwm_set_dma_result(esy_err_t result)
{
    s_ctx.dma_result = result;
}

void fake_esy_hal_pwm_set_cleanup_results(esy_err_t abort_result,
                                          esy_err_t deinit_result)
{
    s_ctx.abort_result = abort_result;
    s_ctx.deinit_result = deinit_result;
}

esy_err_t fake_esy_hal_pwm_queue_dma_event(esy_pwm_t *pwm, bool error)
{
    if (pwm == NULL) return ESY_ERR_INVALID_ARG;
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot == NULL || !slot->initialized || !slot->dma_active)
        return ESY_ERR_STATE;
    if (slot->callback_running) return ESY_ERR_BUSY;
    slot->dma_event_pending = true;
    slot->dma_error_pending |= error;
    return ESY_OK;
}

bool fake_esy_hal_pwm_is_dma_active(const esy_pwm_t *pwm) {
    const fake_pwm_slot_t *slot = fake_pwm_get_slot((esy_pwm_t *)pwm, false);
    return slot != NULL && slot->dma_active;
}

void fake_esy_hal_pwm_set_device(esy_pwm_t *pwm, const struct esy_device_s *device) {
    fake_pwm_slot_t *slot = fake_pwm_get_slot(pwm, false);
    if (slot != NULL) slot->device = device;
}
