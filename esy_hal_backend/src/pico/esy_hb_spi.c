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

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#include "esy_hal_backend/esy_hb_dma.h"
#include "esy_board/esy_board.h"

#define RP2040_SPI_MAX 2

typedef struct {
    spi_inst_t     *inst;
    uint8_t         phys_index;
    esy_spi_role_t  role;

    /* master settings */
    uint32_t        speed_hz;
    uint8_t         data_bits;
    bool            is_msb_first;
    uint8_t         mode;
    uint32_t        cs_pin;
    esy_spi_dma_complete_cb_t pending_cb;
    void                     *pending_arg;
    size_t                    tx_length;
    size_t                    rx_length;
    uint8_t                   tx_unit_bytes;
    uint8_t                   rx_unit_bytes;
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
} rp2040_spi_ctx_t;

_Static_assert(ESY_SPI_PRIV_BYTES >= sizeof(rp2040_spi_ctx_t),
               "spi->backend.bytes is too small for rp2040_spi_ctx_t");

/* Task lifecycle is serialized per handle. Atomic controller claims exclude
 * competing handles across cores before any board or native side effects. */
typedef struct {
    _Atomic(esy_spi_t *) owners[RP2040_SPI_MAX];
} esy_hb_spi_ctx_t;
static esy_hb_spi_ctx_t s_ctx;

static rp2040_spi_ctx_t *spi_context(const esy_spi_t *spi) {
    if (spi == NULL || spi->ctx != (const void *)spi->backend.bytes) return NULL;
    for (size_t i = 0u; i < RP2040_SPI_MAX; ++i) {
        if (atomic_load_explicit(&s_ctx.owners[i], memory_order_acquire) == spi) {
            rp2040_spi_ctx_t *c = (rp2040_spi_ctx_t *)(void *)spi->backend.bytes;
            return c->bound_spi == spi && c->phys_index == i ? c : NULL;
        }
    }
    return NULL;
}

enum { SPI_DMA_RX_DONE = 1u, SPI_DMA_TX_DONE = 2u, SPI_DMA_ERROR = 4u };

/* ISR callbacks publish only; the owner polls to retire both DMA directions. */
static void spi_dma_internal_callback(void *arg) {
    rp2040_spi_ctx_t *c = arg;
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_RX_DONE, memory_order_release);
}
static void spi_dma_tx_callback(void *arg) {
    rp2040_spi_ctx_t *c = arg;
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_TX_DONE, memory_order_release);
}
static void spi_dma_error_callback(void *arg) {
    rp2040_spi_ctx_t *c = arg;
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_ERROR, memory_order_release);
}

static esy_err_t spi_retire_dma_channels(rp2040_spi_ctx_t *c) {
    esy_err_t tx = ESY_OK, rx = ESY_OK;
    if (c->tx_owned) {
        (void)esy_hb_dma_get_trans_count_remaining(c->tx_dreq, c->tx_channel,
                                                  &c->tx_remaining);
        tx = esy_hb_dma_abort(c, c->tx_dreq, c->tx_channel);
        if (tx == ESY_OK) c->tx_owned = false;
    }
    if (c->rx_owned) {
        (void)esy_hb_dma_get_trans_count_remaining(c->rx_dreq, c->rx_channel,
                                                  &c->rx_remaining);
        rx = esy_hb_dma_abort(c, c->rx_dreq, c->rx_channel);
        if (rx == ESY_OK) c->rx_owned = false;
    }
    return tx != ESY_OK ? tx : rx;
}

static inline void cs_low_if_needed(const rp2040_spi_ctx_t *c) {
    if (c->role == ESY_SPI_ROLE_MASTER && c->cs_pin != ESY_PIN_UNUSED) {
        gpio_put(c->cs_pin, 0);
    }
}
static inline void cs_high_if_needed(const rp2040_spi_ctx_t *c) {
    if (c->role == ESY_SPI_ROLE_MASTER && c->cs_pin != ESY_PIN_UNUSED) {
        gpio_put(c->cs_pin, 1);
    }
}

static esy_err_t spi_reject_dma_start(rp2040_spi_ctx_t *c, esy_err_t error) {
    atomic_fetch_or_explicit(&c->dma_events, SPI_DMA_ERROR, memory_order_release);
    esy_err_t cleanup = spi_retire_dma_channels(c);
    if (cleanup != ESY_OK || spi_is_busy(c->inst))
        return cleanup != ESY_OK ? cleanup : ESY_ERR_BUSY;
    c->pending_cb = NULL;
    c->pending_arg = NULL;
    c->dma_active = false;
    cs_high_if_needed(c);
    return error;
}

static bool is_valid_spi_config(const esy_spi_config_t *cfg) {
    if (!cfg) return false;
    if (cfg->phys_index >= RP2040_SPI_MAX) return false;
    if ((cfg->role != ESY_SPI_ROLE_MASTER) &&
        (cfg->role != ESY_SPI_ROLE_MASTER_NO_CS) &&
        (cfg->role != ESY_SPI_ROLE_SLAVE)) {
        return false;
    }
    if ((cfg->role != ESY_SPI_ROLE_SLAVE) && (cfg->speed_hz == 0u)) {
        return false;
    }
    if ((unsigned)cfg->mode > (unsigned)ESY_SPI_MODE_3) return false;
    return (cfg->data_bits >= 4u) && (cfg->data_bits <= 16u);
}

esy_err_t esy_hb_spi_claim(esy_spi_t *spi, const esy_spi_config_t *cfg) {
    if (spi == NULL || !is_valid_spi_config(cfg)) return ESY_ERR_INVALID_ARG;
    if (!cfg->is_msb_first) return ESY_ERR_NOT_SUPPORTED;
    if (spi->ctx != NULL) {
        rp2040_spi_ctx_t *c = spi_context(spi);
        return c != NULL && c->phys_index == cfg->phys_index &&
                       !c->native_owned && !c->dma_active
                   ? ESY_OK : ESY_ERR_STATE;
    }
    esy_spi_t *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(
            &s_ctx.owners[cfg->phys_index], &expected, spi,
            memory_order_acq_rel, memory_order_acquire)) return ESY_ERR_BUSY;
    rp2040_spi_ctx_t *c = (rp2040_spi_ctx_t *)(void *)spi->backend.bytes;
    memset(c, 0, sizeof(*c));
    atomic_init(&c->dma_events, 0u);
    c->inst = cfg->phys_index == 0u ? spi0 : spi1;
    c->phys_index = cfg->phys_index;
    c->bound_spi = spi;
    spi->ctx = c;
    return ESY_OK;
}

esy_err_t esy_hb_spi_release(esy_spi_t *spi) {
    if (spi == NULL) return ESY_ERR_INVALID_ARG;
    if (spi->ctx == NULL) return ESY_OK;
    rp2040_spi_ctx_t *c = spi_context(spi);
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
    rp2040_spi_ctx_t *c = spi_context(spi);
    c->role = cfg->role;
    c->speed_hz = cfg->speed_hz;
    c->data_bits = cfg->data_bits;
    c->is_msb_first = cfg->is_msb_first;
    c->mode = (uint8_t)cfg->mode;
    c->cs_pin = cfg->role == ESY_SPI_ROLE_MASTER
                    ? cfg->inner.cs_pin : ESY_PIN_UNUSED;
    /* SDK initialization always programs a divider, including in slave mode.
     * Slave SCK is external; use a valid divider independently of speed_hz. */
    const uint32_t init_speed_hz = c->role == ESY_SPI_ROLE_SLAVE
                                      ? clock_get_hz(clk_peri) / 2u : c->speed_hz;
    spi_init(c->inst, init_speed_hz);
    c->native_owned = true;
    spi_set_slave(c->inst, c->role == ESY_SPI_ROLE_SLAVE);
    spi_set_format(c->inst, c->data_bits, (c->mode >> 1) & 1u,
                   c->mode & 1u, SPI_MSB_FIRST);
    c->enabled = true;
    return ESY_OK;
}

esy_err_t esy_hb_spi_deinit(esy_spi_t *spi) {
    if (spi == NULL) return ESY_ERR_INVALID_ARG;
    if (spi->ctx == NULL) return ESY_OK;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (c == NULL) return ESY_ERR_STATE;
    if (c->dma_active || c->tx_owned || c->rx_owned ||
        (c->native_owned && spi_is_busy(c->inst))) return ESY_ERR_BUSY;
    if (c->native_owned) spi_deinit(c->inst);
    c->enabled = false;
    c->native_owned = false;
    return ESY_OK;
}

extern int32_t esy_hb_spi_write_blocking(esy_spi_t *spi,
                                             const uint8_t *tx_buf,
                                             size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !len) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;

    cs_low_if_needed(c);
    int32_t n = spi_write_blocking(c->inst, tx_buf, len);
    cs_high_if_needed(c);
    return n;
}

extern int32_t esy_hb_spi_read_blocking(esy_spi_t *spi,
                                            uint8_t *rx_buf,
                                            size_t len) {
    if (!spi || !spi_context(spi) || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;

    cs_low_if_needed(c);
    int32_t n = spi_read_blocking(c->inst, 0xFF, rx_buf, len);
    cs_high_if_needed(c);
    return n;
}

extern int32_t esy_hb_spi_transfer_blocking(esy_spi_t *spi,
                                                const uint8_t *tx_buf,
                                                uint8_t *rx_buf,
                                                size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;

    cs_low_if_needed(c);
    int32_t n = spi_write_read_blocking(c->inst, tx_buf, rx_buf, len);
    cs_high_if_needed(c);
    return n;
}

extern int32_t esy_hb_spi_write16_blocking(esy_spi_t *spi,
                                               const uint16_t *tx_buf,
                                               size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !len) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;

    cs_low_if_needed(c);
    for (size_t i = 0; i < len; ++i) {
        spi_write16_blocking(c->inst, &tx_buf[i], 1);
    }
    cs_high_if_needed(c);
    return (int32_t)len;
}

extern int32_t esy_hb_spi_read16_blocking(esy_spi_t *spi,
                                              uint16_t *rx_buf,
                                              size_t len) {
    if (!spi || !spi_context(spi) || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;

    cs_low_if_needed(c);
    for (size_t i = 0; i < len; ++i) {
        spi_read16_blocking(c->inst, 0xFFFF, &rx_buf[i], 1);
    }
    cs_high_if_needed(c);
    return (int32_t)len;
}

extern int32_t esy_hb_spi_transfer16_blocking(esy_spi_t *spi,
                                                  const uint16_t *tx_buf,
                                                  uint16_t *rx_buf,
                                                  size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !rx_buf || !len) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;

    cs_low_if_needed(c);
    int32_t transferred = spi_write16_read16_blocking(c->inst, tx_buf, rx_buf, len);
    cs_high_if_needed(c);
    return transferred;
}

extern esy_err_t esy_hb_spi_transfer_dma(esy_spi_t *spi,
                                             esy_dma_config_t *tx_dma_config,
                                             esy_dma_config_t *rx_dma_config,
                                             esy_spi_dma_complete_cb_t complete_cb,
                                             void *user_data) {
    if (!spi || !spi_context(spi) || !tx_dma_config || !rx_dma_config) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active) return ESY_ERR_BUSY;

    while (spi_is_readable(c->inst)) {
        (void)spi_get_hw(c->inst)->dr;
    }

    void *spi_data_reg = (void *)&spi_get_hw(c->inst)->dr;

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
    c->dma_active = true;

    rx_dma_config->inner.direction = ESY_DMA_PERIPH_TO_MEM;
    rx_dma_config->src             = spi_data_reg;
    rx_dma_config->callback        = spi_dma_internal_callback;
    rx_dma_config->callback_arg    = c;
    rx_dma_config->error_callback = spi_dma_error_callback;
    rx_dma_config->error_callback_arg = c;

    tx_dma_config->inner.direction = ESY_DMA_MEM_TO_PERIPH;
    tx_dma_config->dst             = spi_data_reg;
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

    uint32_t mask = (1u << tx_dma_config->inner.channel) |
                    (1u << rx_dma_config->inner.channel);
    err = esy_hb_dma_start(c, mask);
    if (err != ESY_OK) return spi_reject_dma_start(c, err);
    return err;
}

extern esy_err_t esy_hb_spi_set_speed(esy_spi_t *spi, uint32_t speed_hz) {
    if (!spi || !spi_context(spi) || !speed_hz) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (c->dma_active || spi_is_busy(c->inst)) return ESY_ERR_BUSY;
    if (c->role != ESY_SPI_ROLE_MASTER &&
        c->role != ESY_SPI_ROLE_MASTER_NO_CS) return ESY_ERR_INVALID_ARG;

    spi_set_baudrate(c->inst, speed_hz);
    c->speed_hz = speed_hz;
    return ESY_OK;
}

extern esy_err_t esy_hb_spi_abort(esy_spi_t *spi) {
    rp2040_spi_ctx_t *c = spi_context(spi);
    if (c == NULL) return ESY_ERR_INVALID_ARG;
    if (!c->enabled) return ESY_ERR_STATE;
    return ESY_ERR_NOT_SUPPORTED;
}

extern esy_err_t esy_hb_spi_get_caps(const esy_spi_t *spi, uint32_t *out_caps) {
    if (!spi || !spi_context(spi) || !out_caps) return ESY_ERR_INVALID_ARG;
    if (!spi_context(spi)->enabled) return ESY_ERR_STATE;
    *out_caps = ESY_SPI_CAP_DMA_TRANSFER | ESY_SPI_CAP_TRANSFER16 |
                ESY_SPI_CAP_DMA_REMAINING;
    return ESY_OK;
}

extern esy_err_t esy_hb_spi_get_phys_index(const esy_spi_t *spi, uint8_t *out_index) {
    if (!spi || !spi_context(spi) || !out_index) return ESY_ERR_INVALID_ARG;
    const rp2040_spi_ctx_t *c = spi_context(spi);
    *out_index = c->phys_index;
    return ESY_OK;
}

extern esy_err_t esy_hb_spi_get_dma_trans_count_remaining(const esy_spi_t *spi,
                                                              size_t *out_tx_remaining,
                                                              size_t *out_rx_remaining) {
    esy_dma_inner_config_t tx_inner = {0};
    esy_dma_inner_config_t rx_inner = {0};
    const rp2040_spi_ctx_t *c;
    esy_err_t ret;

    if (!spi || !spi_context(spi)) return ESY_ERR_INVALID_ARG;
    if ((out_tx_remaining == NULL) && (out_rx_remaining == NULL)) return ESY_ERR_INVALID_ARG;

    c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    if (!esy_board_spi_get_dma_info(c->phys_index, &tx_inner, &rx_inner)) {
        return ESY_ERR_NOT_SUPPORTED;
    }

    if (out_tx_remaining != NULL) {
        ret = esy_hb_dma_get_trans_count_remaining(tx_inner.dreq,
                                                       tx_inner.channel,
                                                       out_tx_remaining);
        if (ret != ESY_OK) return ret;
    }
    if (out_rx_remaining != NULL) {
        ret = esy_hb_dma_get_trans_count_remaining(rx_inner.dreq,
                                                       rx_inner.channel,
                                                       out_rx_remaining);
        if (ret != ESY_OK) return ret;
    }

    return ESY_OK;
}

extern void esy_hb_spi_handle_irq(esy_spi_t *spi) {
    (void)spi;
}

/* Owner task only; callbacks may enqueue after state and CS are retired. */
esy_err_t esy_hb_spi_poll(esy_spi_t *spi) {
    if (!spi || !spi_context(spi)) return ESY_ERR_INVALID_ARG;
    rp2040_spi_ctx_t *c = spi_context(spi);
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
    if (spi_retire_dma_channels(c) != ESY_OK) return ESY_ERR_BUSY;
    if (spi_is_busy(c->inst)) return ESY_OK;
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
    c->pending_cb = NULL;
    c->pending_arg = NULL;
    c->dma_active = false;
    cs_high_if_needed(c);
    if (cb) cb(spi, &result, arg);
    return ESY_OK;
}

bool esy_hb_spi_is_busy(esy_spi_t *spi) {
    if (!spi || !spi_context(spi)) return false;
    rp2040_spi_ctx_t *c = spi_context(spi);
    return c->dma_active || (c->enabled && spi_is_busy(c->inst));
}
