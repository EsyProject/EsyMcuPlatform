/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_spi.h"
#include "esy_hal/esy_hal_spi_device.h"
#include <stdatomic.h>
#include <string.h>

/*
 * Keep a simple fixed-size instance table so the test fake avoids dynamic
 * allocation while still mapping each esy_spi_t handle to observable state.
 */
#ifndef FAKE_ESY_HAL_SPI_MAX_INSTANCES
#define FAKE_ESY_HAL_SPI_MAX_INSTANCES 4
#endif

#ifndef FAKE_ESY_HAL_SPI_BUF_SIZE
#define FAKE_ESY_HAL_SPI_BUF_SIZE 256
#endif

#ifndef FAKE_ESY_HAL_SPI_TX_HISTORY
#define FAKE_ESY_HAL_SPI_TX_HISTORY 64
#endif

typedef struct
{
    esy_spi_t *spi;   /* External handle passed by the caller. */
    const esy_device_t *device;
    esy_err_t (*poll_hook)(esy_spi_t *spi);
    int32_t (*pm_write_hook)(esy_spi_t *spi, const uint8_t *data, size_t length);
    bool       in_use;

    /* Last TX payload observed through the 8-bit API. */
    uint8_t    last_tx[FAKE_ESY_HAL_SPI_BUF_SIZE];
    size_t     last_tx_len;
    uint8_t    tx_history[FAKE_ESY_HAL_SPI_TX_HISTORY][FAKE_ESY_HAL_SPI_BUF_SIZE];
    size_t     tx_history_len[FAKE_ESY_HAL_SPI_TX_HISTORY];
    size_t     tx_count;

    /* Preset RX data for the 8-bit API. */
    uint8_t    rx_data[FAKE_ESY_HAL_SPI_BUF_SIZE];
    size_t     rx_data_len;
    size_t     rx_data_pos;

    bool       initialized;
    size_t     dma_tx_remaining; /* DMA transfer units, independently observable. */
    size_t     dma_rx_remaining;
    /* Terminal results report bytes regardless of descriptor element width. */
    size_t     dma_tx_requested_bytes;
    size_t     dma_rx_requested_bytes;
    bool       dma_busy;
    bool       failed_submit;
    const void *borrowed_tx;
    void *borrowed_rx;
    size_t poll_count;
    size_t abort_count;
    esy_spi_dma_complete_cb_t last_dma_callback;
    void *last_dma_callback_arg;
} fake_spi_slot_t;

typedef struct {
  fake_spi_slot_t slots[FAKE_ESY_HAL_SPI_MAX_INSTANCES];
  esy_err_t init_result;
  esy_err_t write_dma_result;
  esy_err_t read_dma_result;
  esy_err_t transfer_dma_result;
  esy_err_t get_dma_remaining_result;
  int32_t write_blocking_result;
  bool transfer_dma_async;
  bool failed_submit_retains_borrow;
  esy_err_t poll_result;
  esy_err_t abort_result;
  bool write_fault_armed;
  size_t write_skip;
  int32_t write_fault;
  bool read_fault_armed;
  size_t read_skip;
  int32_t read_fault;
} fake_esy_hal_spi_ctx_t;

static fake_esy_hal_spi_ctx_t s_ctx = {
    .write_dma_result = ESY_ERR_NOT_SUPPORTED,
    .read_dma_result = ESY_ERR_NOT_SUPPORTED,
};

static fake_spi_slot_t *fake_spi_get_slot(esy_spi_t *spi, bool create_if_missing)
{
    /* Find an existing slot first. */
    for (int i = 0; i < FAKE_ESY_HAL_SPI_MAX_INSTANCES; ++i) {
        if (s_ctx.slots[i].in_use && s_ctx.slots[i].spi == spi) {
            return &s_ctx.slots[i];
        }
    }

    if (!create_if_missing) {
        return NULL;
    }

    /* Create a slot from the fixed pool. */
    for (int i = 0; i < FAKE_ESY_HAL_SPI_MAX_INSTANCES; ++i) {
        if (!s_ctx.slots[i].in_use) {
            s_ctx.slots[i].in_use = true;
            s_ctx.slots[i].spi = spi;
            s_ctx.slots[i].last_tx_len = 0;
            s_ctx.slots[i].tx_count = 0;
            s_ctx.slots[i].rx_data_len = 0;
            s_ctx.slots[i].rx_data_pos = 0;
            s_ctx.slots[i].initialized = false;
            s_ctx.slots[i].dma_tx_remaining = 0u;
            s_ctx.slots[i].dma_rx_remaining = 0u;
            return &s_ctx.slots[i];
        }
    }

    /* No free slot is available. */
    return NULL;
}

void fake_esy_hal_spi_reset(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.init_result = ESY_OK;
    s_ctx.write_dma_result = ESY_ERR_NOT_SUPPORTED;
    s_ctx.read_dma_result = ESY_ERR_NOT_SUPPORTED;
    s_ctx.transfer_dma_result = ESY_OK;
    s_ctx.get_dma_remaining_result = ESY_OK;
    s_ctx.write_blocking_result = 0;
    s_ctx.transfer_dma_async = false;
}

void fake_esy_hal_spi_set_init_result(esy_err_t ret)
{
    s_ctx.init_result = ret;
}

static const esy_spi_vtable_t k_fake_spi_ops = {
    .write_blocking = esy_hal_spi_write_blocking,
    .read_blocking = esy_hal_spi_read_blocking,
    .transfer_blocking = esy_hal_spi_transfer_blocking,
    .write_then_read_blocking = esy_hal_spi_write_then_read_blocking,
    .write16_blocking = esy_hal_spi_write16_blocking,
    .read16_blocking = esy_hal_spi_read16_blocking,
    .transfer16_blocking = esy_hal_spi_transfer16_blocking,
    .transfer_dma = esy_hal_spi_transfer_dma,
    .poll = esy_hal_spi_poll,
    .abort = esy_hal_spi_abort,
    .is_busy = esy_hal_spi_is_busy,
    .set_speed = esy_hal_spi_set_speed,
    .get_caps = esy_hal_spi_get_caps,
    .get_phys_index = esy_hal_spi_get_phys_index,
};

/* Fake implementation of the HAL SPI API. */

void esy_hal_spi_default_cfg(esy_spi_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    /* Provide conservative defaults for tests that inspect the config. */
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = ESY_SPI_MODE_0;
    cfg->is_msb_first = true;
    cfg->speed_hz = 1000000u;
    cfg->inner.cs_pin = ESY_PIN_UNUSED;
}

esy_err_t esy_hal_spi_init(esy_spi_t *spi,
                                const esy_spi_config_t *cfg)
{
    if (spi == NULL || cfg == NULL) return ESY_ERR_INVALID_ARG;
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
    if (!slot) {
        return ESY_ERR_NO_MEM; /* The fixed fake slot pool is exhausted. */
    }
    if (slot->initialized || slot->dma_busy || slot->device != NULL || spi->ops != NULL)
        return ESY_ERR_STATE;
    if (s_ctx.init_result != ESY_OK) {
        return s_ctx.init_result;
    }
    slot->initialized = true;
    spi->ops = &k_fake_spi_ops;
    return ESY_OK;
}

esy_err_t esy_hal_spi_deinit(esy_spi_t *spi)
{
    if (spi == NULL) return ESY_ERR_INVALID_ARG;
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    if (!slot) {
        return ESY_OK;
    }
    if (slot->device != NULL) return ESY_ERR_STATE;
    if (slot->dma_busy) return ESY_ERR_BUSY;
    /* Clear the matching slot directly. */
    memset(slot, 0, sizeof(*slot));
    memset(spi, 0, sizeof(*spi));
    return ESY_OK;
}

int32_t esy_hal_spi_write_blocking(esy_spi_t *spi,
                                        const uint8_t *data,
                                        size_t len)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
    if (!slot) {
        return ESY_ERR_NO_MEM;
    }

    /* Record the payload for later test assertions. */
    size_t to_copy = len;
    if (to_copy > FAKE_ESY_HAL_SPI_BUF_SIZE) {
        to_copy = FAKE_ESY_HAL_SPI_BUF_SIZE;
    }
    if (data && to_copy > 0) {
        memcpy(slot->last_tx, data, to_copy);
    }
    slot->last_tx_len = to_copy;
    if (slot->tx_count < FAKE_ESY_HAL_SPI_TX_HISTORY) {
        if (data && to_copy > 0) {
            memcpy(slot->tx_history[slot->tx_count], data, to_copy);
        }
        slot->tx_history_len[slot->tx_count] = to_copy;
    }
    slot->tx_count++;

    if (s_ctx.write_fault_armed && s_ctx.write_skip-- == 0u) {
        s_ctx.write_fault_armed = false;
        return s_ctx.write_fault;
    }
    if (s_ctx.write_blocking_result < 0) {
        return s_ctx.write_blocking_result;
    }

    return (int32_t)len;
}

int32_t esy_hal_spi_read_blocking(esy_spi_t *spi,
                                       uint8_t *data,
                                       size_t len)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
    if (!slot) {
        return ESY_ERR_NO_MEM;
    }

    if (s_ctx.read_fault_armed && s_ctx.read_skip-- == 0u) {
        s_ctx.read_fault_armed = false;
        return s_ctx.read_fault;
    }

    size_t remain = 0;
    if (slot->rx_data_len > slot->rx_data_pos) {
        remain = slot->rx_data_len - slot->rx_data_pos;
    }

    size_t to_copy = len;
    if (to_copy > remain) {
        to_copy = remain;
    }

    if (data && to_copy > 0) {
        memcpy(data, &slot->rx_data[slot->rx_data_pos], to_copy);
    }
    slot->rx_data_pos += to_copy;

    /* Pad with zeroes when the read is longer than the preset RX data. */
    if (data && to_copy < len) {
        memset(data + to_copy, 0, len - to_copy);
    }

    return (int32_t)len;
}

int32_t esy_hal_spi_transfer_blocking(esy_spi_t *spi,
                                           const uint8_t *tx_buf,
                                           uint8_t *rx_buf,
                                           size_t len)
{
    /* Record TX through the same path as write_blocking. */
    int32_t written = esy_hal_spi_write_blocking(spi, tx_buf, len);
    if (written != (int32_t)len) return written;
    /* Fill RX through the same path as read_blocking. */
    return esy_hal_spi_read_blocking(spi, rx_buf, len);
}

esy_err_t esy_hal_spi_write_then_read_blocking(
    esy_spi_t *spi, const uint8_t *tx_buf, size_t tx_len, uint8_t *rx_buf,
    size_t rx_len)
{
    if (esy_hal_spi_write_blocking(spi, tx_buf, tx_len) !=
        (int32_t)tx_len) {
        return ESY_ERR_IO;
    }
    if (esy_hal_spi_read_blocking(spi, rx_buf, rx_len) !=
        (int32_t)rx_len) {
        return ESY_ERR_IO;
    }
    return ESY_OK;
}

/* Keep byte-oriented history, but report completed halfwords to callers. */
static int32_t halfword_count(int32_t bytes) {
    return bytes < 0 ? bytes : bytes / (int32_t)sizeof(uint16_t);
}

int32_t esy_hal_spi_write16_blocking(esy_spi_t *spi,
    const uint16_t *data, size_t len) {
    if (len > INT32_MAX / sizeof(uint16_t)) return ESY_ERR_INVALID_ARG;
    return halfword_count(esy_hal_spi_write_blocking(spi,
        (const uint8_t *)data, len * sizeof(uint16_t)));
}

int32_t esy_hal_spi_read16_blocking(esy_spi_t *spi, uint16_t *data, size_t len) {
    if (len > INT32_MAX / sizeof(uint16_t)) return ESY_ERR_INVALID_ARG;
    return halfword_count(esy_hal_spi_read_blocking(spi,
        (uint8_t *)data, len * sizeof(uint16_t)));
}

int32_t esy_hal_spi_transfer16_blocking(esy_spi_t *spi,
    const uint16_t *tx_buf, uint16_t *rx_buf, size_t len) {
    if (len > INT32_MAX / sizeof(uint16_t)) return ESY_ERR_INVALID_ARG;
    return halfword_count(esy_hal_spi_transfer_blocking(spi,
        (const uint8_t *)tx_buf, (uint8_t *)rx_buf, len * sizeof(uint16_t)));
}

/* Match the public wrapper before applying injected provider behavior. */
static esy_err_t validate_dma(esy_spi_t *spi,
    const esy_dma_config_t *tx, const esy_dma_config_t *rx) {
    if (spi == NULL || (tx == NULL && rx == NULL)) return ESY_ERR_INVALID_ARG;
    const esy_dma_config_t *configs[] = {tx, rx};
    for (size_t i = 0u; i < 2u; ++i) {
        const esy_dma_config_t *config = configs[i];
        if (config == NULL) continue;
        const void *memory = i == 0u ? config->src : config->dst;
        if (memory == NULL || config->length == 0u ||
            config->length > SIZE_MAX / 8u) return ESY_ERR_INVALID_ARG;
        if (config->circular || config->transfer_size == ESY_DMA_TRANSFER_SIZE_32)
            return ESY_ERR_NOT_SUPPORTED;
        if (config->transfer_size != ESY_DMA_TRANSFER_SIZE_8 &&
            config->transfer_size != ESY_DMA_TRANSFER_SIZE_16)
            return ESY_ERR_INVALID_ARG;
        const size_t unit = config->transfer_size == ESY_DMA_TRANSFER_SIZE_16
            ? sizeof(uint16_t) : sizeof(uint8_t);
        if (config->length % unit != 0u || (uintptr_t)memory % unit != 0u)
            return ESY_ERR_INVALID_ARG;
    }
    if (tx != NULL && rx != NULL &&
        (tx->length != rx->length || tx->transfer_size != rx->transfer_size))
        return ESY_ERR_INVALID_ARG;
    return ESY_OK;
}

/* DMA APIs are unsupported by default unless a test overrides the result. */
esy_err_t esy_hal_spi_write_dma(esy_spi_t *spi,
                                     esy_dma_config_t *dma_cfg)
{
    esy_err_t result = validate_dma(spi, dma_cfg, NULL);
    if (result != ESY_OK) return result;
    return s_ctx.write_dma_result;
}

esy_err_t esy_hal_spi_read_dma(esy_spi_t *spi,
                                    esy_dma_config_t *dma_cfg)
{
    esy_err_t result = validate_dma(spi, NULL, dma_cfg);
    if (result != ESY_OK) return result;
    return s_ctx.read_dma_result;
}

esy_err_t esy_hal_spi_transfer_dma(esy_spi_t *spi,
                                        esy_dma_config_t *tx_dma_cfg,
                                        esy_dma_config_t *rx_dma_cfg,
                                        esy_spi_dma_complete_cb_t complete_cb,
                                        void *user_data)
{
    esy_err_t result = validate_dma(spi, tx_dma_cfg, rx_dma_cfg);
    if (result != ESY_OK) return result;
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
    if (!slot) {
        return ESY_ERR_NO_MEM;
    }
    if (slot->dma_busy) return ESY_ERR_BUSY;
    if (s_ctx.transfer_dma_result != ESY_OK) {
        if (s_ctx.failed_submit_retains_borrow) {
            slot->failed_submit = true;
            slot->dma_busy = true;
            slot->borrowed_tx = tx_dma_cfg != NULL ? tx_dma_cfg->src : NULL;
            slot->borrowed_rx = rx_dma_cfg != NULL ? rx_dma_cfg->dst : NULL;
            slot->last_dma_callback = complete_cb;
            slot->last_dma_callback_arg = user_data;
        }
        return s_ctx.transfer_dma_result;
    }
    slot->borrowed_tx = tx_dma_cfg != NULL ? tx_dma_cfg->src : NULL;
    slot->borrowed_rx = rx_dma_cfg != NULL ? rx_dma_cfg->dst : NULL;

    if (tx_dma_cfg != NULL) {
        slot->dma_tx_requested_bytes = tx_dma_cfg->length;
        slot->dma_tx_remaining = tx_dma_cfg->length /
            (tx_dma_cfg->transfer_size == ESY_DMA_TRANSFER_SIZE_16 ?
                sizeof(uint16_t) : sizeof(uint8_t));
        if ((tx_dma_cfg->src != NULL) && (tx_dma_cfg->length > 0u)) {
            (void)esy_hal_spi_write_blocking(spi,
                                                  (const uint8_t *)tx_dma_cfg->src,
                                                  tx_dma_cfg->length);
        }
    } else {
        slot->dma_tx_remaining = 0u;
        slot->dma_tx_requested_bytes = 0u;
    }

    if (rx_dma_cfg != NULL) {
        slot->dma_rx_requested_bytes = rx_dma_cfg->length;
        slot->dma_rx_remaining = rx_dma_cfg->length /
            (rx_dma_cfg->transfer_size == ESY_DMA_TRANSFER_SIZE_16 ?
                sizeof(uint16_t) : sizeof(uint8_t));
        if ((rx_dma_cfg->dst != NULL) && (rx_dma_cfg->length > 0u)) {
            (void)esy_hal_spi_read_blocking(spi,
                                                 (uint8_t *)rx_dma_cfg->dst,
                                                 rx_dma_cfg->length);
        }
    } else {
        slot->dma_rx_remaining = 0u;
        slot->dma_rx_requested_bytes = 0u;
    }

    slot->last_dma_callback = complete_cb;
    slot->last_dma_callback_arg = user_data;

    if (s_ctx.transfer_dma_async) {
        slot->dma_busy = true;
        return ESY_OK;
    }

    if (complete_cb != NULL) {
        const esy_spi_dma_transfer_result_t result = {
            .status = ESY_SPI_DMA_TRANSFER_OK,
            .tx_transferred = tx_dma_cfg != NULL ? tx_dma_cfg->length : 0u,
            .rx_transferred = rx_dma_cfg != NULL ? rx_dma_cfg->length : 0u,
        };
        complete_cb(spi, &result, user_data);
    }

    slot->dma_tx_remaining = 0u;
    slot->dma_rx_remaining = 0u;
    slot->dma_tx_requested_bytes = 0u;
    slot->dma_rx_requested_bytes = 0u;
    slot->dma_busy = false;
    slot->borrowed_tx = NULL;
    slot->borrowed_rx = NULL;
    slot->last_dma_callback = NULL;
    slot->last_dma_callback_arg = NULL;
    return ESY_OK;
}

esy_err_t esy_hal_spi_get_dma_trans_count_remaining(const esy_spi_t *spi,
                                                         size_t *out_tx_remaining,
                                                         size_t *out_rx_remaining)
{
    fake_spi_slot_t *slot = fake_spi_get_slot((esy_spi_t *)spi, false);
    if (s_ctx.get_dma_remaining_result != ESY_OK) {
        return s_ctx.get_dma_remaining_result;
    }
    if (slot == NULL) {
        return ESY_ERR_INVALID_ARG;
    }

    if (out_tx_remaining != NULL) {
        *out_tx_remaining = slot->dma_tx_remaining;
    }
    if (out_rx_remaining != NULL) {
        *out_rx_remaining = slot->dma_rx_remaining;
    }

    return ESY_OK;
}

bool esy_hal_spi_is_busy(esy_spi_t *spi)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    return slot != NULL && slot->dma_busy;
}

esy_err_t esy_hal_spi_set_speed(esy_spi_t *spi, uint32_t speed_hz)
{
    (void)spi;
    (void)speed_hz;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hal_spi_abort(esy_spi_t *spi)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    if (!slot) {
        return ESY_ERR_INVALID_ARG;
    }
    ++slot->abort_count;
    if (s_ctx.abort_result != ESY_OK) return s_ctx.abort_result;
    slot->dma_busy = false;
    slot->failed_submit = false;
    slot->borrowed_tx = NULL;
    slot->borrowed_rx = NULL;
    slot->dma_tx_remaining = 0u;
    slot->dma_rx_remaining = 0u;
    slot->dma_tx_requested_bytes = 0u;
    slot->dma_rx_requested_bytes = 0u;
    slot->last_dma_callback = NULL;
    slot->last_dma_callback_arg = NULL;
    return ESY_OK;
}

esy_err_t esy_hal_spi_get_caps(const esy_spi_t *spi,
                                    uint32_t *out_caps)
{
    if (spi == NULL || out_caps == NULL) return ESY_ERR_INVALID_ARG;
    fake_spi_slot_t *slot = fake_spi_get_slot((esy_spi_t *)spi, false);
    if (slot == NULL || !slot->initialized) return ESY_ERR_NOT_IMPLEMENTED;
    uint32_t caps = ESY_SPI_CAP_TRANSFER16;
    if (s_ctx.transfer_dma_result != ESY_ERR_NOT_SUPPORTED)
        caps |= ESY_SPI_CAP_DMA_TRANSFER;
    if (s_ctx.get_dma_remaining_result != ESY_ERR_NOT_SUPPORTED)
        caps |= ESY_SPI_CAP_DMA_REMAINING;
    *out_caps = caps;
    return ESY_OK;
}

esy_err_t esy_hal_spi_get_phys_index(const esy_spi_t *spi,
                                          uint8_t *out_index)
{
    (void)spi;
    (void)out_index;
    return ESY_ERR_NOT_SUPPORTED;
}

/* Fake extension API implementation. */

void fake_esy_hal_spi_set_rx_data(esy_spi_t *spi,
                                       const uint8_t *data,
                                       size_t len)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
    if (!slot) {
        return;
    }

    size_t to_copy = len;
    if (to_copy > FAKE_ESY_HAL_SPI_BUF_SIZE) {
        to_copy = FAKE_ESY_HAL_SPI_BUF_SIZE;
    }

    if (data && to_copy > 0) {
        memcpy(slot->rx_data, data, to_copy);
    }
    slot->rx_data_len = to_copy;
    slot->rx_data_pos = 0;
}

size_t fake_esy_hal_spi_get_last_tx_len(esy_spi_t *spi)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    if (!slot) {
        return 0;
    }
    return slot->last_tx_len;
}

size_t fake_esy_hal_spi_copy_last_tx(esy_spi_t *spi,
                                          uint8_t *out_buf,
                                          size_t max_len)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    if (!slot || !out_buf || max_len == 0) {
        return 0;
    }

    size_t to_copy = slot->last_tx_len;
    if (to_copy > max_len) {
        to_copy = max_len;
    }

    memcpy(out_buf, slot->last_tx, to_copy);
    return to_copy;
}

size_t fake_esy_hal_spi_get_tx_count(esy_spi_t *spi)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    if (!slot) {
        return 0;
    }
    return slot->tx_count;
}

size_t fake_esy_hal_spi_copy_tx_at(esy_spi_t *spi,
                                        size_t index,
                                        uint8_t *out_buf,
                                        size_t max_len)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    if (!slot || !out_buf || max_len == 0 || index >= slot->tx_count ||
        index >= FAKE_ESY_HAL_SPI_TX_HISTORY) {
        return 0;
    }

    size_t to_copy = slot->tx_history_len[index];
    if (to_copy > max_len) {
        to_copy = max_len;
    }

    memcpy(out_buf, slot->tx_history[index], to_copy);
    return to_copy;
}

void fake_esy_hal_spi_set_write_blocking_result(int32_t ret)
{
    s_ctx.write_blocking_result = ret;
}

bool fake_esy_hal_spi_is_initialized(esy_spi_t *spi)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    return slot ? slot->initialized : false;
}

void fake_esy_hal_spi_set_dma_remaining(esy_spi_t *spi,
                                             size_t tx_remaining,
                                             size_t rx_remaining)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
    if (!slot) {
        return;
    }
    slot->dma_tx_remaining = tx_remaining;
    slot->dma_rx_remaining = rx_remaining;
}

void fake_esy_hal_spi_set_write_dma_result(esy_err_t ret)
{
    s_ctx.write_dma_result = ret;
}

void fake_esy_hal_spi_set_read_dma_result(esy_err_t ret)
{
    s_ctx.read_dma_result = ret;
}

void fake_esy_hal_spi_set_transfer_dma_result(esy_err_t ret)
{
    s_ctx.transfer_dma_result = ret;
}

void fake_esy_hal_spi_set_failed_submit_retains_borrow(bool retain) {
    s_ctx.failed_submit_retains_borrow = retain;
}

void fake_esy_hal_spi_set_poll_result(esy_err_t ret) { s_ctx.poll_result = ret; }
void fake_esy_hal_spi_set_abort_result(esy_err_t ret) { s_ctx.abort_result = ret; }

size_t fake_esy_hal_spi_get_poll_count(esy_spi_t *spi) {
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    return slot != NULL ? slot->poll_count : 0u;
}
size_t fake_esy_hal_spi_get_abort_count(esy_spi_t *spi) {
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    return slot != NULL ? slot->abort_count : 0u;
}
const void *fake_esy_hal_spi_get_borrowed_tx(esy_spi_t *spi) {
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    return slot != NULL ? slot->borrowed_tx : NULL;
}
void *fake_esy_hal_spi_get_borrowed_rx(esy_spi_t *spi) {
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    return slot != NULL ? slot->borrowed_rx : NULL;
}

void fake_esy_hal_spi_set_transfer_dma_async(bool async)
{
    s_ctx.transfer_dma_async = async;
}

void fake_esy_hal_spi_complete_last_dma_with_result(
    esy_spi_t *spi, const esy_spi_dma_transfer_result_t *result)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    if (!slot || !slot->dma_busy || !result) {
        return;
    }

    esy_spi_dma_complete_cb_t callback = slot->last_dma_callback;
    void *callback_arg = slot->last_dma_callback_arg;
    const esy_spi_dma_transfer_result_t completed = *result;
    slot->dma_busy = false;
    slot->failed_submit = false;
    slot->borrowed_tx = NULL;
    slot->borrowed_rx = NULL;
    slot->dma_tx_remaining = 0u;
    slot->dma_rx_remaining = 0u;
    slot->dma_tx_requested_bytes = 0u;
    slot->dma_rx_requested_bytes = 0u;
    slot->last_dma_callback = NULL;
    slot->last_dma_callback_arg = NULL;
    if (callback != NULL) callback(spi, &completed, callback_arg);
}

void fake_esy_hal_spi_complete_last_dma(esy_spi_t *spi)
{
    fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
    if (!slot || !slot->dma_busy) return;
    const esy_spi_dma_transfer_result_t result = {
        .status = slot->failed_submit ? ESY_SPI_DMA_TRANSFER_DMA_ERROR :
            ESY_SPI_DMA_TRANSFER_OK,
        .tx_transferred = slot->dma_tx_requested_bytes,
        .rx_transferred = slot->dma_rx_requested_bytes,
    };
    fake_esy_hal_spi_complete_last_dma_with_result(spi, &result);
}

void fake_esy_hal_spi_set_get_dma_remaining_result(esy_err_t ret)
{
    s_ctx.get_dma_remaining_result = ret;
}

void fake_esy_hal_spi_fail_write_after(size_t successful_calls, int32_t result) {
    s_ctx.write_fault_armed = true;
    s_ctx.write_skip = successful_calls;
    s_ctx.write_fault = result;
}

void fake_esy_hal_spi_fail_read_after(size_t successful_calls, int32_t result) {
    s_ctx.read_fault_armed = true;
    s_ctx.read_skip = successful_calls;
    s_ctx.read_fault = result;
}

esy_err_t esy_hal_spi_poll(esy_spi_t *spi) {
  if (!spi) return ESY_ERR_INVALID_ARG;
  fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
  if (slot == NULL) return ESY_ERR_NO_MEM;
  ++slot->poll_count;
  if (slot->poll_hook != NULL) return slot->poll_hook(spi);
  if (s_ctx.poll_result != ESY_OK) return s_ctx.poll_result;
  if (slot->failed_submit) fake_esy_hal_spi_complete_last_dma(spi);
  return ESY_OK;
}

const esy_device_t *esy_hal_spi_get_device(const esy_spi_t *spi) {
  const fake_spi_slot_t *slot = fake_spi_get_slot((esy_spi_t *)spi, false);
  return slot == NULL ? NULL : slot->device;
}

void fake_esy_hal_spi_set_device(esy_spi_t *spi, const esy_device_t *device) {
  if (spi == NULL) return;
  fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
  if (slot == NULL) return;
  slot->device = device;
  spi->ops = &k_fake_spi_ops;
}

void fake_esy_hal_spi_set_poll_hook(esy_spi_t *spi, esy_err_t (*hook)(esy_spi_t *)) {
  if (spi == NULL) return;
  fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
  if (slot != NULL) slot->poll_hook = hook;
}

void fake_esy_hal_spi_set_pm_write_hook(esy_spi_t *spi,
    int32_t (*hook)(esy_spi_t *, const uint8_t *, size_t)) {
  if (spi == NULL) return;
  fake_spi_slot_t *slot = fake_spi_get_slot(spi, true);
  if (slot != NULL) slot->pm_write_hook = hook;
}

int32_t esy_hal_spi_write_blocking_for_pm(esy_spi_t *spi,
    const esy_device_t *requester, const uint8_t *data, size_t length) {
  if (spi == NULL || requester == NULL || data == NULL || length == 0u)
    return ESY_ERR_INVALID_ARG;
  fake_spi_slot_t *slot = fake_spi_get_slot(spi, false);
  const esy_device_t *parent = slot == NULL ? NULL : slot->device;
  if (parent == NULL || parent->registry == NULL || requester->registry != parent->registry ||
      atomic_load_explicit(&requester->state, memory_order_acquire) != ESY_DEVICE_STATE_TRANSITIONING ||
      atomic_load_explicit(&parent->state, memory_order_acquire) != ESY_DEVICE_STATE_ACTIVE)
    return ESY_ERR_STATE;
  bool dependency = false;
  for (uint32_t i = 0u; i < requester->dependency_count; ++i) {
    if (requester->dependencies[i] == parent) dependency = true;
  }
  if (!dependency) return ESY_ERR_STATE;
  if (slot->dma_busy) return ESY_ERR_BUSY;
  if (slot->pm_write_hook != NULL) return slot->pm_write_hook(spi, data, length);
  return esy_hal_spi_write_blocking(spi, data, length);
}
