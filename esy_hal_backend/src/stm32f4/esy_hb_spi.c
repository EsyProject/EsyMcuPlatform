/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_spi.h"

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "stm32f4xx_hal.h"

#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_hal_backend/esy_hb_gpio.h"

#define STM32F4_SPI_MAX 4u
#define STM32F4_SPI_BLOCKING_TIMEOUT_LOOPS 1000000u

typedef struct {
    SPI_TypeDef           *inst;
    uint8_t                phys_index;
    esy_spi_role_t         role;

    uint32_t               speed_hz;
    uint8_t                data_bits;
    bool                   is_msb_first;
    uint8_t                mode;
    uint32_t               cs_pin;

    SPI_HandleTypeDef      hspi;

    esy_spi_dma_complete_cb_t pending_cb;
    void                     *pending_arg;
    size_t                    tx_length;
    size_t                    rx_length;
    size_t                    tx_unit_bytes;
    size_t                    rx_unit_bytes;
    size_t                    tx_remaining;
    size_t                    rx_remaining;
    uint32_t                  tx_dreq;
    uint32_t                  rx_dreq;
    uint8_t                   tx_channel;
    uint8_t                   rx_channel;
    bool                      tx_owned;
    bool                      rx_owned;
    bool                      dma_active;
    atomic_uint               dma_events;

    bool enabled;
    bool native_owned;
    esy_spi_t *bound_spi;
} stm32f4_spi_ctx_t;

_Static_assert(ESY_SPI_PRIV_BYTES >= sizeof(stm32f4_spi_ctx_t),
               "ESY_SPI_PRIV_BYTES too small for stm32f4_spi_ctx_t");

static inline void cs_low_if_needed(const stm32f4_spi_ctx_t *c) {
    if (c->role == ESY_SPI_ROLE_MASTER && c->cs_pin != ESY_PIN_UNUSED) esy_hb_gpio_write(c->cs_pin, false);
}
static inline void cs_high_if_needed(const stm32f4_spi_ctx_t *c) {
    if (c->role == ESY_SPI_ROLE_MASTER && c->cs_pin != ESY_PIN_UNUSED) esy_hb_gpio_write(c->cs_pin, true);
}

static uint32_t calc_prescaler(uint32_t pclk, uint32_t target_hz) {
    if (!target_hz) target_hz = 1000000u;
    struct { uint16_t div; uint32_t hal; } opts[] = {
        { 2,   SPI_BAUDRATEPRESCALER_2   },
        { 4,   SPI_BAUDRATEPRESCALER_4   },
        { 8,   SPI_BAUDRATEPRESCALER_8   },
        { 16,  SPI_BAUDRATEPRESCALER_16  },
        { 32,  SPI_BAUDRATEPRESCALER_32  },
        { 64,  SPI_BAUDRATEPRESCALER_64  },
        { 128, SPI_BAUDRATEPRESCALER_128 },
        { 256, SPI_BAUDRATEPRESCALER_256 },
    };
    for (unsigned i = 0; i < sizeof(opts)/sizeof(opts[0]); ++i) {
        uint32_t baud = pclk / opts[i].div;
        if (baud <= target_hz) return opts[i].hal;
    }
    return SPI_BAUDRATEPRESCALER_256;
}

static uint32_t spi_data_size_from_bits(uint8_t data_bits) {
    if (data_bits == 8u) {
        return SPI_DATASIZE_8BIT;
    }
    return SPI_DATASIZE_16BIT;
}

static bool spi_wait_flag_set(SPI_TypeDef *spi_inst, uint32_t flag) {
    uint32_t timeout = STM32F4_SPI_BLOCKING_TIMEOUT_LOOPS;

    while ((spi_inst->SR & flag) == 0u) {
        if (timeout == 0u) {
            return false;
        }
        timeout--;
    }
    return true;
}

static bool spi_wait_flag_clear(SPI_TypeDef *spi_inst, uint32_t flag) {
    uint32_t timeout = STM32F4_SPI_BLOCKING_TIMEOUT_LOOPS;

    while ((spi_inst->SR & flag) != 0u) {
        if (timeout == 0u) {
            return false;
        }
        timeout--;
    }
    return true;
}

static inline __IO uint8_t *spi_dr8(SPI_TypeDef *spi_inst) {
    return (__IO uint8_t *)(uintptr_t)&spi_inst->DR;
}

static inline __IO uint16_t *spi_dr16(SPI_TypeDef *spi_inst) {
    return (__IO uint16_t *)(uintptr_t)&spi_inst->DR;
}

/* Task lifecycle is serialized per handle. Atomic controller claims exclude
 * competing handles across cores before any board or native side effects. */
typedef struct {
    _Atomic(esy_spi_t *) owners[STM32F4_SPI_MAX];
} esy_hb_spi_ctx_t;
static esy_hb_spi_ctx_t s_ctx;

static stm32f4_spi_ctx_t *spi_context(const esy_spi_t *spi) {
    if (spi == NULL || spi->ctx != (const void *)spi->backend.bytes) return NULL;
    for (size_t i = 0u; i < STM32F4_SPI_MAX; ++i) {
        if (atomic_load_explicit(&s_ctx.owners[i], memory_order_acquire) == spi) {
            stm32f4_spi_ctx_t *c = (stm32f4_spi_ctx_t *)(void *)spi->backend.bytes;
            return c->bound_spi == spi && c->phys_index == i ? c : NULL;
        }
    }
    return NULL;
}

enum { SPI_DMA_RX_DONE = 1u, SPI_DMA_TX_DONE = 2u, SPI_DMA_ERROR = 4u };

/* ISR callbacks publish only; the owner polls to retire both DMA directions. */
static void spi_dma_internal_callback(void *arg) {
    stm32f4_spi_ctx_t *c = arg;
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_RX_DONE, memory_order_release);
}
static void spi_dma_tx_callback(void *arg) {
    stm32f4_spi_ctx_t *c = arg;
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_TX_DONE, memory_order_release);
}
static void spi_dma_error_callback(void *arg) {
    stm32f4_spi_ctx_t *c = arg;
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_ERROR, memory_order_release);
}

/* Task-only retirement. Only streams prepared by this transaction may be
 * aborted; a failed second prepare must not steal another stream's owner. */
static esy_err_t spi_retire_dma_streams(stm32f4_spi_ctx_t *c) {
    esy_err_t tx_result = ESY_OK;
    esy_err_t rx_result = ESY_OK;
    CLEAR_BIT(c->inst->CR2, SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    if (c->tx_owned) {
        (void)esy_hb_dma_get_trans_count_remaining(c->tx_dreq, c->tx_channel,
                                                  &c->tx_remaining);
        tx_result = esy_hb_dma_abort(c, c->tx_dreq, c->tx_channel);
        if (tx_result == ESY_OK) c->tx_owned = false;
    }
    if (c->rx_owned) {
        (void)esy_hb_dma_get_trans_count_remaining(c->rx_dreq, c->rx_channel,
                                                  &c->rx_remaining);
        rx_result = esy_hb_dma_abort(c, c->rx_dreq, c->rx_channel);
        if (rx_result == ESY_OK) c->rx_owned = false;
    }
    return tx_result != ESY_OK ? tx_result : rx_result;
}

static void spi_release_transaction(stm32f4_spi_ctx_t *c) {
    c->pending_cb = NULL;
    c->pending_arg = NULL;
    c->dma_active = false;
    cs_high_if_needed(c);
}

static esy_err_t spi_reject_dma_start(stm32f4_spi_ctx_t *c, esy_err_t error) {
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_ERROR, memory_order_release);
    esy_err_t cleanup = spi_retire_dma_streams(c);
    if (cleanup != ESY_OK || (c->inst->SR & SPI_SR_BSY) != 0u) {
        /* A failed submit can retain hardware. Keep callbacks/storage borrowed;
         * the owner must poll or retry abort before reusing the buffers. */
        return cleanup != ESY_OK ? cleanup : ESY_ERR_BUSY;
    }
    spi_release_transaction(c);
    return error;
}

esy_err_t esy_hb_spi_claim(esy_spi_t *spi, const esy_spi_config_t *cfg) {
    if (spi == NULL || cfg == NULL || cfg->phys_index == 0u ||
        cfg->phys_index >= STM32F4_SPI_MAX || cfg->data_bits == 0u ||
        cfg->data_bits > 16u || (unsigned)cfg->mode > (unsigned)ESY_SPI_MODE_3 ||
        (cfg->role != ESY_SPI_ROLE_MASTER &&
         cfg->role != ESY_SPI_ROLE_MASTER_NO_CS && cfg->role != ESY_SPI_ROLE_SLAVE) ||
        (cfg->role != ESY_SPI_ROLE_SLAVE && cfg->speed_hz == 0u))
        return ESY_ERR_INVALID_ARG;
    if (cfg->data_bits != 8u && cfg->data_bits != 16u)
        return ESY_ERR_NOT_SUPPORTED;
    if (spi->ctx != NULL) {
        stm32f4_spi_ctx_t *c = spi_context(spi);
        return c != NULL && c->phys_index == cfg->phys_index &&
                       !c->native_owned && !c->dma_active
                   ? ESY_OK : ESY_ERR_STATE;
    }
    esy_spi_t *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(
            &s_ctx.owners[cfg->phys_index], &expected, spi,
            memory_order_acq_rel, memory_order_acquire)) return ESY_ERR_BUSY;
    stm32f4_spi_ctx_t *c = (stm32f4_spi_ctx_t *)(void *)spi->backend.bytes;
    memset(c, 0, sizeof(*c));
    atomic_init(&c->dma_events, 0u);
    c->inst = cfg->phys_index == 1u ? SPI1 : cfg->phys_index == 2u ? SPI2 : SPI3;
    c->phys_index = cfg->phys_index;
    c->bound_spi = spi;
    spi->ctx = c;
    return ESY_OK;
}

esy_err_t esy_hb_spi_release(esy_spi_t *spi) {
    if (spi == NULL) return ESY_ERR_INVALID_ARG;
    if (spi->ctx == NULL) return ESY_OK;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (c == NULL) return ESY_ERR_STATE;
    if (c->native_owned || c->dma_active || c->tx_owned || c->rx_owned)
        return ESY_ERR_BUSY;
    uint8_t index = c->phys_index;
    memset(c, 0, sizeof(*c));
    spi->ctx = NULL;
    atomic_store_explicit(&s_ctx.owners[index], NULL, memory_order_release);
    return ESY_OK;
}

extern esy_err_t esy_hb_spi_init(esy_spi_t *spi, const esy_spi_config_t *cfg) {
    esy_err_t result = esy_hb_spi_claim(spi, cfg);
    if (result != ESY_OK) return result;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    c->phys_index   = cfg->phys_index;
    c->role         = cfg->role;
    c->speed_hz     = cfg->speed_hz;
    c->data_bits    = cfg->data_bits;
    c->is_msb_first = cfg->is_msb_first;
    c->mode         = (uint8_t)cfg->mode & 0x3u;
    c->cs_pin       = (cfg->role == ESY_SPI_ROLE_MASTER)
                          ? cfg->inner.cs_pin
                          : ESY_PIN_UNUSED;

    SPI_HandleTypeDef *h = &c->hspi;
    h->Instance           = c->inst;
    h->Init.Mode          = (c->role == ESY_SPI_ROLE_SLAVE) ? SPI_MODE_SLAVE : SPI_MODE_MASTER;
    h->Init.Direction     = SPI_DIRECTION_2LINES;
    h->Init.DataSize      = spi_data_size_from_bits(c->data_bits);
    switch (c->mode) {
        case 0: h->Init.CLKPolarity = SPI_POLARITY_LOW;  h->Init.CLKPhase = SPI_PHASE_1EDGE; break;
        case 1: h->Init.CLKPolarity = SPI_POLARITY_LOW;  h->Init.CLKPhase = SPI_PHASE_2EDGE; break;
        case 2: h->Init.CLKPolarity = SPI_POLARITY_HIGH; h->Init.CLKPhase = SPI_PHASE_1EDGE; break;
        case 3: h->Init.CLKPolarity = SPI_POLARITY_HIGH; h->Init.CLKPhase = SPI_PHASE_2EDGE; break;
        default: return ESY_ERR_INVALID_ARG;
    }
    h->Init.NSS            = (c->role == ESY_SPI_ROLE_SLAVE)
                                 ? SPI_NSS_HARD_INPUT
                                 : SPI_NSS_SOFT;
    uint32_t pclk          = (cfg->phys_index == 1) ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
    if (!pclk) return ESY_ERR_INVALID_ARG;
    h->Init.BaudRatePrescaler = calc_prescaler(pclk, c->speed_hz);
    h->Init.FirstBit       = c->is_msb_first ? SPI_FIRSTBIT_MSB : SPI_FIRSTBIT_LSB;
    h->Init.TIMode         = SPI_TIMODE_DISABLE;
    h->Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    h->Init.CRCPolynomial  = 7;

    c->native_owned = true;
    if (HAL_SPI_Init(h) != HAL_OK) return ESY_ERR_FAIL;
    __HAL_SPI_ENABLE(h);

    if (c->role == ESY_SPI_ROLE_MASTER && c->cs_pin != ESY_PIN_UNUSED) {
        esy_hb_gpio_write(c->cs_pin, true);
    }

    c->enabled = true;
    return ESY_OK;
}

esy_err_t esy_hb_spi_deinit(esy_spi_t *spi) {
    if (spi == NULL) return ESY_ERR_INVALID_ARG;
    if (spi->ctx == NULL) return ESY_OK;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (c == NULL) return ESY_ERR_STATE;
    if (c->dma_active || c->tx_owned || c->rx_owned ||
        (c->enabled && (c->inst->SR & SPI_SR_BSY) != 0u)) return ESY_ERR_BUSY;
    if (c->native_owned && HAL_SPI_DeInit(&c->hspi) != HAL_OK)
        return ESY_ERR_FAIL;
    c->enabled = false;
    c->native_owned = false;
    return ESY_OK;
}

extern int32_t esy_hb_spi_write_blocking(esy_spi_t *spi,
                                             const uint8_t *tx_buf,
                                             size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !len) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;
    SPI_TypeDef *spi_inst = c->inst;

    cs_low_if_needed(c);
    for (size_t i = 0; i < len; ++i) {
        if (!spi_wait_flag_set(spi_inst, SPI_SR_TXE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        *spi_dr8(spi_inst) = tx_buf[i];
        if (!spi_wait_flag_set(spi_inst, SPI_SR_RXNE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        (void)*spi_dr8(spi_inst);
    }
    if (!spi_wait_flag_clear(spi_inst, SPI_SR_BSY)) {
        cs_high_if_needed(c);
        return ESY_ERR_TIMEOUT;
    }
    cs_high_if_needed(c);
    return (int32_t)len;
}

extern int32_t esy_hb_spi_read_blocking(esy_spi_t *spi,
                                            uint8_t *rx_buf,
                                            size_t len) {
    if (!spi || !spi_context(spi) || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;
    SPI_TypeDef *spi_inst = c->inst;

    cs_low_if_needed(c);
    for (size_t i = 0; i < len; ++i) {
        if (!spi_wait_flag_set(spi_inst, SPI_SR_TXE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        *spi_dr8(spi_inst) = 0xFF;
        if (!spi_wait_flag_set(spi_inst, SPI_SR_RXNE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        rx_buf[i] = *spi_dr8(spi_inst);
    }
    if (!spi_wait_flag_clear(spi_inst, SPI_SR_BSY)) {
        cs_high_if_needed(c);
        return ESY_ERR_TIMEOUT;
    }
    cs_high_if_needed(c);
    return (int32_t)len;
}

extern int32_t esy_hb_spi_transfer_blocking(esy_spi_t *spi,
                                                const uint8_t *tx_buf,
                                                uint8_t *rx_buf,
                                                size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;
    SPI_TypeDef *spi_inst = c->inst;

    cs_low_if_needed(c);
    for (size_t i = 0; i < len; ++i) {
        if (!spi_wait_flag_set(spi_inst, SPI_SR_TXE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        *spi_dr8(spi_inst) = tx_buf[i];
        if (!spi_wait_flag_set(spi_inst, SPI_SR_RXNE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        rx_buf[i] = *spi_dr8(spi_inst);
    }
    if (!spi_wait_flag_clear(spi_inst, SPI_SR_BSY)) {
        cs_high_if_needed(c);
        return ESY_ERR_TIMEOUT;
    }
    cs_high_if_needed(c);
    return (int32_t)len;
}

extern int32_t esy_hb_spi_write16_blocking(esy_spi_t *spi,
                                               const uint16_t *tx_buf,
                                               size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !len) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;
    SPI_TypeDef *spi_inst = c->inst;

    cs_low_if_needed(c);
    for (size_t i = 0; i < len; ++i) {
        if (!spi_wait_flag_set(spi_inst, SPI_SR_TXE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        *spi_dr16(spi_inst) = tx_buf[i];
        if (!spi_wait_flag_set(spi_inst, SPI_SR_RXNE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        (void)*spi_dr16(spi_inst);
    }
    if (!spi_wait_flag_clear(spi_inst, SPI_SR_BSY)) {
        cs_high_if_needed(c);
        return ESY_ERR_TIMEOUT;
    }
    cs_high_if_needed(c);
    return (int32_t)len;
}

extern int32_t esy_hb_spi_read16_blocking(esy_spi_t *spi,
                                              uint16_t *rx_buf,
                                              size_t len) {
    if (!spi || !spi_context(spi) || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;
    SPI_TypeDef *spi_inst = c->inst;

    cs_low_if_needed(c);
    for (size_t i = 0; i < len; ++i) {
        if (!spi_wait_flag_set(spi_inst, SPI_SR_TXE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        *spi_dr16(spi_inst) = 0xFFFF;
        if (!spi_wait_flag_set(spi_inst, SPI_SR_RXNE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        rx_buf[i] = *spi_dr16(spi_inst);
    }
    if (!spi_wait_flag_clear(spi_inst, SPI_SR_BSY)) {
        cs_high_if_needed(c);
        return ESY_ERR_TIMEOUT;
    }
    cs_high_if_needed(c);
    return (int32_t)len;
}

extern int32_t esy_hb_spi_transfer16_blocking(esy_spi_t *spi,
                                                  const uint16_t *tx_buf,
                                                  uint16_t *rx_buf,
                                                  size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;
    SPI_TypeDef *spi_inst = c->inst;

    cs_low_if_needed(c);
    for (size_t i = 0; i < len; ++i) {
        if (!spi_wait_flag_set(spi_inst, SPI_SR_TXE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        *spi_dr16(spi_inst) = tx_buf[i];
        if (!spi_wait_flag_set(spi_inst, SPI_SR_RXNE)) {
            cs_high_if_needed(c);
            return ESY_ERR_TIMEOUT;
        }
        rx_buf[i] = *spi_dr16(spi_inst);
    }
    if (!spi_wait_flag_clear(spi_inst, SPI_SR_BSY)) {
        cs_high_if_needed(c);
        return ESY_ERR_TIMEOUT;
    }
    cs_high_if_needed(c);
    return (int32_t)len;
}

extern esy_err_t esy_hb_spi_transfer_dma(esy_spi_t *spi,
                                             esy_dma_config_t *tx_dma_config,
                                             esy_dma_config_t *rx_dma_config,
                                             esy_spi_dma_complete_cb_t complete_cb,
                                             void *user_data) {
    if (!spi || !spi_context(spi) || !tx_dma_config || !rx_dma_config) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active || (c->inst->SR & SPI_SR_BSY) != 0u) return ESY_ERR_BUSY;
    if (tx_dma_config->circular || rx_dma_config->circular)
        return ESY_ERR_NOT_SUPPORTED;
    const esy_dma_transfer_size_t width = c->data_bits <= 8u
        ? ESY_DMA_TRANSFER_SIZE_8 : ESY_DMA_TRANSFER_SIZE_16;
    if (tx_dma_config->length == 0u ||
        tx_dma_config->length != rx_dma_config->length ||
        tx_dma_config->transfer_size != width ||
        rx_dma_config->transfer_size != width ||
        (tx_dma_config->src == NULL && rx_dma_config->dst == NULL))
        return ESY_ERR_INVALID_ARG;
    uint8_t tx_resource, rx_resource;
    esy_err_t route = esy_hb_dma_get_resource_id(tx_dma_config->inner.dreq,
        tx_dma_config->inner.channel, &tx_resource);
    if (route != ESY_OK) return route;
    route = esy_hb_dma_get_resource_id(rx_dma_config->inner.dreq,
        rx_dma_config->inner.channel, &rx_resource);
    if (route != ESY_OK) return route;
    if (tx_resource == rx_resource) return ESY_ERR_INVALID_ARG;

    while (c->inst->SR & SPI_SR_RXNE) {
        (void)c->inst->DR;
    }

    volatile void *dr = (volatile void *)&c->inst->DR;

    c->pending_cb = complete_cb;
    c->pending_arg = user_data;
    c->tx_length = tx_dma_config->length;
    c->rx_length = rx_dma_config->length;
    c->tx_remaining = SIZE_MAX;
    c->rx_remaining = SIZE_MAX;
    c->tx_unit_bytes = tx_dma_config->transfer_size == ESY_DMA_TRANSFER_SIZE_32 ? 4u
        : (tx_dma_config->transfer_size == ESY_DMA_TRANSFER_SIZE_16 ? 2u : 1u);
    c->rx_unit_bytes = rx_dma_config->transfer_size == ESY_DMA_TRANSFER_SIZE_32 ? 4u
        : (rx_dma_config->transfer_size == ESY_DMA_TRANSFER_SIZE_16 ? 2u : 1u);
    c->tx_dreq = tx_dma_config->inner.dreq;
    c->rx_dreq = rx_dma_config->inner.dreq;
    c->tx_channel = tx_dma_config->inner.channel;
    c->rx_channel = rx_dma_config->inner.channel;
    atomic_store_explicit(&c->dma_events, 0u, memory_order_relaxed);
    c->tx_owned = false;
    c->rx_owned = false;
    c->dma_active = true;

    rx_dma_config->inner.direction = ESY_DMA_PERIPH_TO_MEM;
    rx_dma_config->src             = (void *)dr;
    rx_dma_config->callback        = spi_dma_internal_callback;
    rx_dma_config->callback_arg    = c;
    rx_dma_config->error_callback = spi_dma_error_callback;
    rx_dma_config->error_callback_arg = c;

    tx_dma_config->inner.direction = ESY_DMA_MEM_TO_PERIPH;
    tx_dma_config->dst             = (void *)dr;
    tx_dma_config->callback        = spi_dma_tx_callback;
    tx_dma_config->callback_arg    = c;
    tx_dma_config->error_callback = spi_dma_error_callback;
    tx_dma_config->error_callback_arg = c;

    cs_low_if_needed(c);

    esy_err_t err = esy_hb_dma_prepare(c, rx_dma_config);
    if (err != ESY_OK) return spi_reject_dma_start(c, err);
    c->rx_owned = true;

    err = esy_hb_dma_prepare(c, tx_dma_config);
    if (err != ESY_OK) return spi_reject_dma_start(c, err);
    c->tx_owned = true;

    uint32_t mask = (1u << tx_resource) | (1u << rx_resource);
    esy_err_t started = esy_hb_dma_start(c, mask);
    if (started != ESY_OK) return spi_reject_dma_start(c, started);
    SET_BIT(c->inst->CR2, SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    return ESY_OK;
}

extern esy_err_t esy_hb_spi_get_dma_trans_count_remaining(const esy_spi_t *spi,
                                                              size_t *out_tx_remaining,
                                                              size_t *out_rx_remaining) {
    if (!spi || !spi_context(spi)) return ESY_ERR_INVALID_ARG;
    if ((out_tx_remaining == NULL) && (out_rx_remaining == NULL)) return ESY_ERR_INVALID_ARG;

    if (out_tx_remaining) *out_tx_remaining = 0u;
    if (out_rx_remaining) *out_rx_remaining = 0u;
    return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_spi_set_speed(esy_spi_t *spi, uint32_t speed_hz) {
    if (!spi || !spi_context(spi) || !speed_hz) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active || (c->inst->SR & SPI_SR_BSY) != 0u) return ESY_ERR_BUSY;
    if (c->role != ESY_SPI_ROLE_MASTER &&
        c->role != ESY_SPI_ROLE_MASTER_NO_CS) return ESY_ERR_INVALID_ARG;

    SPI_HandleTypeDef *h = &c->hspi;
    uint32_t pclk = (c->phys_index == 1) ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
    if (!pclk) return ESY_ERR_INVALID_ARG;

    h->Init.BaudRatePrescaler = calc_prescaler(pclk, speed_hz);
    if (HAL_SPI_Init(h) != HAL_OK) return ESY_ERR_FAIL;
    __HAL_SPI_ENABLE(h);

    c->speed_hz = speed_hz;
    return ESY_OK;
}

extern esy_err_t esy_hb_spi_abort(esy_spi_t *spi) {
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (c == NULL) return ESY_ERR_INVALID_ARG;
    if (!c->enabled) return ESY_ERR_STATE;
    if (!c->dma_active)
        return (c->inst->SR & SPI_SR_BSY) != 0u ? ESY_ERR_BUSY : ESY_OK;
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_ERROR, memory_order_release);
    esy_err_t result = spi_retire_dma_streams(c);
    if (result != ESY_OK) return result;
    if ((c->inst->SR & SPI_SR_BSY) != 0u) return ESY_ERR_BUSY;
    spi_release_transaction(c);
    return ESY_OK;
}

extern esy_err_t esy_hb_spi_get_caps(const esy_spi_t *spi, uint32_t *out_caps) {
    if (!spi || !spi_context(spi) || !out_caps) return ESY_ERR_INVALID_ARG;
    if (!spi_context(spi)->enabled) return ESY_ERR_STATE;
    *out_caps = ESY_SPI_CAP_DMA_TRANSFER | ESY_SPI_CAP_TRANSFER16;
    return ESY_OK;
}

extern esy_err_t esy_hb_spi_get_phys_index(const esy_spi_t *spi, uint8_t *out_index) {
    if (!spi || !spi_context(spi) || !out_index) return ESY_ERR_INVALID_ARG;
    const stm32f4_spi_ctx_t *c = spi_context(spi);
    *out_index = c->phys_index;
    return ESY_OK;
}

extern void esy_hb_spi_handle_irq(esy_spi_t *spi) {
    (void)spi;
}

/* Owner task only; callbacks may enqueue after state and CS are retired. */
esy_err_t esy_hb_spi_poll(esy_spi_t *spi) {
    if (!spi || !spi_context(spi)) return ESY_ERR_INVALID_ARG;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (!c->dma_active) return ESY_OK;
    unsigned events = atomic_load_explicit(&c->dma_events, memory_order_acquire);
    /* Hardware idle alone cannot distinguish success from an IRQ still pending
     * on the other direction. Observe both terminal events before success. */
    if (!(events & SPI_DMA_ERROR) &&
        (events & (SPI_DMA_RX_DONE | SPI_DMA_TX_DONE)) !=
            (SPI_DMA_RX_DONE | SPI_DMA_TX_DONE)) return ESY_OK;
    if (!(events & SPI_DMA_ERROR) &&
        ((c->tx_owned && esy_hb_dma_is_busy(c->tx_dreq, c->tx_channel)) ||
         (c->rx_owned && esy_hb_dma_is_busy(c->rx_dreq, c->rx_channel)))) return ESY_OK;
    /* Retire even idle channels: a pending or peer-core ISR may still hold
     * callback storage after hardware has cleared BUSY. */
    if (spi_retire_dma_streams(c) != ESY_OK) return ESY_ERR_BUSY;
    if ((c->inst->SR & SPI_SR_BSY) != 0u) return ESY_OK;
    events = atomic_load_explicit(&c->dma_events, memory_order_acquire);
    esy_spi_dma_transfer_result_t result = {ESY_SPI_DMA_TRANSFER_DMA_ERROR, 0u, 0u};
    size_t tx_left = c->tx_remaining, rx_left = c->rx_remaining;
    if (!(events & SPI_DMA_ERROR) &&
        tx_left <= c->tx_length / c->tx_unit_bytes && rx_left <= c->rx_length / c->rx_unit_bytes) {
        result.tx_transferred = c->tx_length - tx_left * c->tx_unit_bytes;
        result.rx_transferred = c->rx_length - rx_left * c->rx_unit_bytes;
        result.status = (tx_left || rx_left) ? ESY_SPI_DMA_TRANSFER_SHORT : ESY_SPI_DMA_TRANSFER_OK;
    }
    esy_spi_dma_complete_cb_t cb = c->pending_cb;
    void *arg = c->pending_arg;
    spi_release_transaction(c);
    if (cb) cb(spi, &result, arg);
    return ESY_OK;
}

bool esy_hb_spi_is_busy(esy_spi_t *spi) {
    if (!spi || !spi_context(spi)) return false;
    stm32f4_spi_ctx_t *c = spi_context(spi);
    return c->dma_active || (c->enabled && ((c->inst->SR & SPI_SR_BSY) != 0u));
}
