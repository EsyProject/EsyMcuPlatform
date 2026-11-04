/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_adc.h"

#include "esy_hb_esp32_context.h"
#include "esp_adc/adc_oneshot.h"

#include <string.h>

#define ESP32_ADC_PHYSICAL_INDEX 1u
#define ESP32_ADC_UNIT_COUNT 2u
#define ESP32_ADC_CHANNEL_COUNT 8u
#define ESP32_ADC_RAW_MAX 4095

typedef struct {
    adc_oneshot_unit_handle_t unit;
    esy_adc_t *owner;
    uint8_t channel;
    bool configured;
    bool cleanup_pending;
} esy_hb_adc_ctx_t;

/* One task/startup owner serializes every ADC1 handle and native lifecycle. */
static esy_hb_adc_ctx_t s_ctx;

static const adc_oneshot_chan_cfg_t k_channel_profile = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
};

static esy_hb_adc_ctx_t *adc_context(const esy_adc_t *adc) {
    return adc != NULL && adc->ctx == &s_ctx && s_ctx.owner == adc
        ? &s_ctx : NULL;
}

static bool adc_ready(const esy_hb_adc_ctx_t *ctx) {
    return ctx != NULL && ctx->configured && !ctx->cleanup_pending;
}

static esy_err_t native_error(esp_err_t result) {
    switch (result) {
        case ESP_OK: return ESY_OK;
        case ESP_ERR_INVALID_ARG: return ESY_ERR_INVALID_ARG;
        case ESP_ERR_INVALID_STATE: return ESY_ERR_STATE;
        case ESP_ERR_NO_MEM: return ESY_ERR_NO_MEM;
        case ESP_ERR_TIMEOUT: return ESY_ERR_TIMEOUT;
        default: return ESY_ERR_IO;
    }
}

esy_err_t esy_hb_adc_validate_config(const esy_adc_config_t *config) {
    if (config == NULL || config->phys_index == 0u ||
        config->phys_index > ESP32_ADC_UNIT_COUNT ||
        (uint32_t)config->sample_time > ESY_ADC_SAMPLETIME_DEFAULT)
        return ESY_ERR_INVALID_ARG;
    if (config->phys_index != ESP32_ADC_PHYSICAL_INDEX)
        return ESY_ERR_NOT_SUPPORTED;
    if (config->inner.channel >= ESP32_ADC_CHANNEL_COUNT)
        return ESY_ERR_INVALID_ARG;
    if (config->sample_time != ESY_ADC_SAMPLETIME_DEFAULT)
        return ESY_ERR_NOT_SUPPORTED;
    return ESY_OK;
}

esy_err_t esy_hb_adc_check_available(uint8_t phys_index) {
    if (phys_index == 0u || phys_index > ESP32_ADC_UNIT_COUNT)
        return ESY_ERR_INVALID_ARG;
    if (phys_index != ESP32_ADC_PHYSICAL_INDEX) return ESY_ERR_NOT_SUPPORTED;
    if (esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;
    return s_ctx.owner == NULL ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_hb_adc_init(esy_adc_t *adc, const esy_adc_config_t *config) {
    if (adc == NULL) return ESY_ERR_INVALID_ARG;
    esy_err_t result = esy_hb_adc_validate_config(config);
    if (result != ESY_OK) return result;
    if (esy_hb_esp32_is_in_isr() || adc->ctx != NULL) return ESY_ERR_STATE;
    result = esy_hb_adc_check_available(config->phys_index);
    if (result != ESY_OK) return result;

    const adc_oneshot_unit_init_cfg_t native_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_unit_handle_t unit = NULL;
    esp_err_t native = adc_oneshot_new_unit(&native_config, &unit);
    if (native != ESP_OK)
        return native == ESP_ERR_NOT_FOUND ? ESY_ERR_BUSY : native_error(native);

    /* Publish the acquired unit before channel setup so failed init can be
     * deinitialized without losing an SDK resource or its exclusive claim. */
    s_ctx.unit = unit;
    s_ctx.owner = adc;
    s_ctx.channel = config->inner.channel;
    adc->ctx = &s_ctx;
    native = adc_oneshot_config_channel(unit, (adc_channel_t)s_ctx.channel,
                                        &k_channel_profile);
    if (native != ESP_OK) return native_error(native);
    s_ctx.configured = true;
    return ESY_OK;
}

esy_err_t esy_hb_adc_deinit(esy_adc_t *adc) {
    if (adc == NULL) return ESY_ERR_INVALID_ARG;
    if (esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;
    esy_hb_adc_ctx_t *ctx = adc_context(adc);
    if (ctx == NULL) return adc->ctx == NULL ? ESY_OK : ESY_ERR_STATE;

    ctx->cleanup_pending = true;
    esp_err_t result = adc_oneshot_del_unit(ctx->unit);
    if (result != ESP_OK) return native_error(result);
    memset(ctx, 0, sizeof(*ctx));
    adc->ctx = NULL;
    return ESY_OK;
}

esy_err_t esy_hb_adc_abort_dma(esy_adc_t *adc) {
    if (adc == NULL) return ESY_ERR_INVALID_ARG;
    if (esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;
    return adc->ctx == NULL || adc_context(adc) != NULL ? ESY_OK : ESY_ERR_STATE;
}

int32_t esy_hb_adc_sample_once(esy_adc_t *adc) {
    if (adc == NULL) return ESY_ERR_INVALID_ARG;
    esy_hb_adc_ctx_t *ctx = adc_context(adc);
    if (!adc_ready(ctx) || esy_hb_esp32_is_in_isr()) return ESY_ERR_STATE;

    int raw = 0;
    esp_err_t result = adc_oneshot_read(ctx->unit, (adc_channel_t)ctx->channel,
                                        &raw);
    if (result != ESP_OK) return native_error(result);
    return raw >= 0 && raw <= ESP32_ADC_RAW_MAX ? raw : ESY_ERR_IO;
}

esy_err_t esy_hb_adc_transfer_dma(esy_adc_t *adc, esy_dma_config_t *config) {
    if (adc == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
    if (!adc_ready(adc_context(adc)) || esy_hb_esp32_is_in_isr())
        return ESY_ERR_STATE;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_adc_transfer_dma_multi_channel(esy_adc_t *adc,
    esy_dma_config_t *config, const uint8_t *channels, uint8_t count) {
    if (adc == NULL || config == NULL || channels == NULL || count == 0u)
        return ESY_ERR_INVALID_ARG;
    if (!adc_ready(adc_context(adc)) || esy_hb_esp32_is_in_isr())
        return ESY_ERR_STATE;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_adc_start_continuous_dma(esy_adc_t *adc,
                                         esy_dma_config_t *config) {
    if (adc == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
    if (!adc_ready(adc_context(adc)) || esy_hb_esp32_is_in_isr())
        return ESY_ERR_STATE;
    return ESY_ERR_NOT_SUPPORTED;
}

bool esy_hb_adc_is_dma_busy(const esy_adc_t *adc) {
    (void)adc;
    return false;
}

esy_err_t esy_hb_adc_set_trigger_source(esy_adc_t *adc,
                                         esy_adc_trigger_edge_t edge,
                                         esy_adc_trigger_source_t source) {
    if (adc == NULL || (uint32_t)edge > ESY_ADC_TRIGGER_EDGE_BOTH ||
        (uint32_t)source > ESY_ADC_TRIGGER_SRC_EXTI11 ||
        ((source == ESY_ADC_TRIGGER_SRC_SOFTWARE) !=
         (edge == ESY_ADC_TRIGGER_EDGE_NONE))) return ESY_ERR_INVALID_ARG;
    if (!adc_ready(adc_context(adc)) || esy_hb_esp32_is_in_isr())
        return ESY_ERR_STATE;
    return source == ESY_ADC_TRIGGER_SRC_SOFTWARE ? ESY_OK
                                                  : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_adc_get_caps(const esy_adc_t *adc, uint32_t *caps) {
    if (adc_context(adc) == NULL || caps == NULL) return ESY_ERR_INVALID_ARG;
    *caps = 0u;
    return ESY_OK;
}

esy_err_t esy_hb_adc_get_phys_index(const esy_adc_t *adc, uint8_t *index) {
    if (adc_context(adc) == NULL || index == NULL) return ESY_ERR_INVALID_ARG;
    *index = ESP32_ADC_PHYSICAL_INDEX;
    return ESY_OK;
}

void esy_hb_adc_handle_irq(void) {
    /* Oneshot conversion has no ADC-owned IRQ or callback registration. */
}
