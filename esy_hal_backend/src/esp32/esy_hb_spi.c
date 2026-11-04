/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_spi.h"

#include <string.h>
#include <stdatomic.h>

#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"

#define ESP32_SPI_MAX 3
#define ESP32_SPI_DMA_CHAN SPI_DMA_CH_AUTO
#define ESP32_SPI_MAX_TRANSFER_BYTES 4094u

typedef struct {
    spi_host_device_t host_id;
    spi_device_handle_t device_handle;
    uint8_t phys_index;
    esy_spi_role_t role;
    bool enabled;
    bool bus_owned;
    bool slave_owned;
    
    uint32_t speed_hz;
    uint8_t data_bits;
    bool is_msb_first;
    uint8_t mode;
    uint32_t cs_pin;
    
    esy_spi_t *bound_handle;
} esp32_spi_ctx_t;

_Static_assert(ESY_SPI_PRIV_BYTES >= sizeof(esp32_spi_ctx_t),
               "ESY_SPI_PRIV_BYTES too small");

/* Task lifecycle is serialized per handle. Atomic controller claims exclude
 * competing handles across cores before any board or native side effects. */
typedef struct {
    _Atomic(esy_spi_t *) owners[ESP32_SPI_MAX];
} esy_hb_spi_ctx_t;
static esy_hb_spi_ctx_t s_ctx;

static esp32_spi_ctx_t *spi_context(const esy_spi_t *spi) {
    if (spi == NULL || spi->ctx != (const void *)spi->backend.bytes) return NULL;
    for (size_t i = 0u; i < ESP32_SPI_MAX; ++i) {
        if (atomic_load_explicit(&s_ctx.owners[i], memory_order_acquire) == spi) {
            esp32_spi_ctx_t *c = (esp32_spi_ctx_t *)(void *)spi->backend.bytes;
            return c->bound_handle == spi && c->phys_index == i ? c : NULL;
        }
    }
    return NULL;
}

static inline spi_host_device_t get_spi_host(uint8_t phys_index) {
    switch (phys_index) {
        case 0: return SPI1_HOST;
        case 1: return SPI2_HOST;
        case 2: return SPI3_HOST;
        default: return SPI1_HOST;
    }
}

static inline void cs_low_if_needed(const esp32_spi_ctx_t *c) {
    if (c->role == ESY_SPI_ROLE_MASTER && c->cs_pin != ESY_PIN_UNUSED) {
        gpio_set_level(c->cs_pin, 0);
    }
}

static inline void cs_high_if_needed(const esp32_spi_ctx_t *c) {
    if (c->role == ESY_SPI_ROLE_MASTER && c->cs_pin != ESY_PIN_UNUSED) {
        gpio_set_level(c->cs_pin, 1);
    }
}

/* Native DMA writes RX in whole words. Use the descriptor's inline word for
 * short reads and copy only the requested bytes; longer reads need whole words
 * because the public buffer contract grants no trailing padding capacity. */
static esy_err_t spi_transmit(esp32_spi_ctx_t *c, spi_transaction_t *transaction) {
    const size_t rx_bytes = transaction->rx_buffer != NULL
        ? transaction->rxlength / 8u : 0u;
    if (transaction->length == 0u ||
        transaction->length > ESP32_SPI_MAX_TRANSFER_BYTES * 8u) {
        return ESY_ERR_INVALID_ARG;
    }
    if (rx_bytes > sizeof(transaction->rx_data) &&
        rx_bytes % sizeof(transaction->rx_data) != 0u) {
        return ESY_ERR_NOT_SUPPORTED;
    }
    void *rx_memory = transaction->rx_buffer;
    if (rx_bytes != 0u && rx_bytes <= sizeof(transaction->rx_data)) {
        transaction->flags |= SPI_TRANS_USE_RXDATA;
        memset(transaction->rx_data, 0, sizeof(transaction->rx_data));
    }
    cs_low_if_needed(c);
    esp_err_t result = spi_device_transmit(c->device_handle, transaction);
    cs_high_if_needed(c);
    if (result != ESP_OK) return ESY_ERR_FAIL;
    if ((transaction->flags & SPI_TRANS_USE_RXDATA) != 0u) {
        memcpy(rx_memory, transaction->rx_data, rx_bytes);
    }
    return ESY_OK;
}

esy_err_t esy_hb_spi_claim(esy_spi_t *spi, const esy_spi_config_t *cfg) {
    if (spi == NULL || cfg == NULL || cfg->phys_index >= ESP32_SPI_MAX ||
        cfg->data_bits == 0u || cfg->data_bits > 16u ||
        (unsigned)cfg->mode > (unsigned)ESY_SPI_MODE_3 ||
        (cfg->role != ESY_SPI_ROLE_MASTER &&
         cfg->role != ESY_SPI_ROLE_MASTER_NO_CS && cfg->role != ESY_SPI_ROLE_SLAVE) ||
        (cfg->role != ESY_SPI_ROLE_SLAVE && cfg->speed_hz == 0u))
        return ESY_ERR_INVALID_ARG;
    /* Blocking buffers contain complete byte or half-word frames. Reject
     * widths that cannot be represented before claiming or routing pins. */
    if (cfg->data_bits != 8u && cfg->data_bits != 16u)
        return ESY_ERR_NOT_SUPPORTED;
    /* SPI1 is reserved for flash and is not a reusable peripheral controller. */
    if (cfg->phys_index == 0u) return ESY_ERR_NOT_SUPPORTED;
    if (spi->ctx != NULL) {
        esp32_spi_ctx_t *c = spi_context(spi);
        return c != NULL && c->phys_index == cfg->phys_index &&
                       !c->bus_owned && !c->slave_owned
                   ? ESY_OK : ESY_ERR_STATE;
    }
    esy_spi_t *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(
            &s_ctx.owners[cfg->phys_index], &expected, spi,
            memory_order_acq_rel, memory_order_acquire)) return ESY_ERR_BUSY;
    size_t max_bytes = 0u;
    if (spi_bus_get_max_transaction_len(get_spi_host(cfg->phys_index),
                                         &max_bytes) == ESP_OK) {
        atomic_store_explicit(&s_ctx.owners[cfg->phys_index], NULL,
                              memory_order_release);
        return ESY_ERR_BUSY;
    }
    esp32_spi_ctx_t *c = (esp32_spi_ctx_t *)(void *)spi->backend.bytes;
    memset(c, 0, sizeof(*c));
    c->host_id = get_spi_host(cfg->phys_index);
    c->phys_index = cfg->phys_index;
    c->bound_handle = spi;
    spi->ctx = c;
    return ESY_OK;
}

esy_err_t esy_hb_spi_release(esy_spi_t *spi) {
    if (spi == NULL) return ESY_ERR_INVALID_ARG;
    if (spi->ctx == NULL) return ESY_OK;
    esp32_spi_ctx_t *c = spi_context(spi);
    if (c == NULL) return ESY_ERR_STATE;
    if (c->bus_owned || c->slave_owned || c->device_handle != NULL) return ESY_ERR_BUSY;
    uint8_t index = c->phys_index;
    memset(c, 0, sizeof(*c));
    spi->ctx = NULL;
    atomic_store_explicit(&s_ctx.owners[index], NULL, memory_order_release);
    return ESY_OK;
}

esy_err_t esy_hb_spi_init(esy_spi_t *spi, const esy_spi_config_t *cfg) {
    esy_err_t result = esy_hb_spi_claim(spi, cfg);
    if (result != ESY_OK) return result;
    esp32_spi_ctx_t *c = spi_context(spi);
    if (cfg->inner.cs_pin != ESY_PIN_UNUSED &&
        (cfg->role == ESY_SPI_ROLE_SLAVE
             ? !GPIO_IS_VALID_GPIO(cfg->inner.cs_pin)
             : !GPIO_IS_VALID_OUTPUT_GPIO(cfg->inner.cs_pin)))
        return ESY_ERR_INVALID_ARG;
    c->host_id = get_spi_host(cfg->phys_index);
    c->phys_index = cfg->phys_index;
    c->role = cfg->role;
    c->speed_hz = cfg->speed_hz;
    c->data_bits = cfg->data_bits;
    c->is_msb_first = cfg->is_msb_first;
    c->mode = cfg->mode & 0x3u;
    c->cs_pin = cfg->inner.cs_pin;

    esp_err_t ret;
    
    if (c->role == ESY_SPI_ROLE_SLAVE) {
        spi_slave_interface_config_t slave_config = {
            .mode = c->mode,
            .spics_io_num = (c->cs_pin != ESY_PIN_UNUSED) ? (int)c->cs_pin : -1,
            .queue_size = 3,
            .flags = c->is_msb_first ? 0 : SPI_SLAVE_RXBIT_LSBFIRST | SPI_SLAVE_TXBIT_LSBFIRST,
        };

        spi_bus_config_t bus_config = {
            .miso_io_num = -1,
            .mosi_io_num = -1,
            .sclk_io_num = -1,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = ESP32_SPI_MAX_TRANSFER_BYTES,
            /* Native transaction resources remain private to synchronous calls. */
            .intr_flags = 0,
        };

        ret = spi_slave_initialize(c->host_id, &bus_config, &slave_config, ESP32_SPI_DMA_CHAN);
        if (ret != ESP_OK) return ESY_ERR_FAIL;
        c->slave_owned = true;
    } else {
        spi_bus_config_t bus_config = {
            .miso_io_num = -1,
            .mosi_io_num = -1,
            .sclk_io_num = -1,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = ESP32_SPI_MAX_TRANSFER_BYTES,
            /* Native transaction resources remain private to synchronous calls. */
            .intr_flags = 0,
        };

        ret = spi_bus_initialize(c->host_id, &bus_config, ESP32_SPI_DMA_CHAN);
        if (ret != ESP_OK) return ESY_ERR_FAIL;
        c->bus_owned = true;

        spi_device_interface_config_t dev_config = {
            .clock_speed_hz = c->speed_hz,
            .mode = c->mode,
            .spics_io_num =
                (c->cs_pin != ESY_PIN_UNUSED &&
                 c->role == ESY_SPI_ROLE_MASTER)
                    ? (int)c->cs_pin
                    : -1,
            .queue_size = 3,
            .flags = c->is_msb_first ? 0 : SPI_DEVICE_RXBIT_LSBFIRST | SPI_DEVICE_TXBIT_LSBFIRST,
        };

        ret = spi_bus_add_device(c->host_id, &dev_config, &c->device_handle);
        if (ret != ESP_OK) return ESY_ERR_FAIL;
    }

    if (c->cs_pin != ESY_PIN_UNUSED && c->role == ESY_SPI_ROLE_MASTER) {
        gpio_config_t cs_config = {
            .pin_bit_mask = (1ULL << c->cs_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&cs_config) != ESP_OK ||
            gpio_set_level(c->cs_pin, 1) != ESP_OK) return ESY_ERR_FAIL;
    }

    c->enabled = true;
    return ESY_OK;
}

esy_err_t esy_hb_spi_deinit(esy_spi_t *spi) {
    if (spi == NULL) return ESY_ERR_INVALID_ARG;
    if (spi->ctx == NULL) return ESY_OK;
    esp32_spi_ctx_t *c = spi_context(spi);
    if (c == NULL) return ESY_ERR_STATE;
    if (c->slave_owned) {
        if (spi_slave_free(c->host_id) != ESP_OK) return ESY_ERR_FAIL;
        c->slave_owned = false;
    }
    if (c->device_handle != NULL) {
        if (spi_bus_remove_device(c->device_handle) != ESP_OK) return ESY_ERR_FAIL;
        c->device_handle = NULL;
        c->enabled = false;
    }
    if (c->bus_owned) {
        if (spi_bus_free(c->host_id) != ESP_OK) return ESY_ERR_FAIL;
        c->bus_owned = false;
    }
    c->enabled = false;
    return ESY_OK;
}

int32_t esy_hb_spi_write_blocking(esy_spi_t *spi, const uint8_t *tx_buf, size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !len || len > ESP32_SPI_MAX_TRANSFER_BYTES) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled || c->role == ESY_SPI_ROLE_SLAVE) {
        return ESY_ERR_STATE;
    }


    spi_transaction_t trans = {0};
    trans.length = len * 8;
    trans.tx_buffer = tx_buf;

    esy_err_t ret = spi_transmit(c, &trans);
    if (ret != ESY_OK) return ret;
    
    return (int32_t)len;
}

int32_t esy_hb_spi_read_blocking(esy_spi_t *spi, uint8_t *rx_buf, size_t len) {
    if (!spi || !spi_context(spi) || !rx_buf || !len || len > ESP32_SPI_MAX_TRANSFER_BYTES) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled || c->role == ESY_SPI_ROLE_SLAVE) {
        return ESY_ERR_STATE;
    }


    spi_transaction_t trans = {0};
    trans.length = len * 8;
    trans.rxlength = len * 8;
    trans.rx_buffer = rx_buf;

    esy_err_t ret = spi_transmit(c, &trans);
    if (ret != ESY_OK) return ret;
    
    return (int32_t)len;
}

int32_t esy_hb_spi_transfer_blocking(esy_spi_t *spi, const uint8_t *tx_buf,
                                        uint8_t *rx_buf, size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !rx_buf || !len ||
        len > ESP32_SPI_MAX_TRANSFER_BYTES) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled || c->role == ESY_SPI_ROLE_SLAVE) {
        return ESY_ERR_STATE;
    }


    spi_transaction_t trans = {0};
    trans.length = len * 8;
    trans.rxlength = len * 8;
    trans.tx_buffer = tx_buf;
    trans.rx_buffer = rx_buf;

    esy_err_t ret = spi_transmit(c, &trans);
    if (ret != ESY_OK) return ret;
    
    return (int32_t)len;
}

int32_t esy_hb_spi_write16_blocking(esy_spi_t *spi, const uint16_t *tx_buf, size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !len ||
        len > ESP32_SPI_MAX_TRANSFER_BYTES / sizeof(uint16_t)) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled || c->role == ESY_SPI_ROLE_SLAVE) {
        return ESY_ERR_STATE;
    }


    spi_transaction_t trans = {0};
    trans.length = len * 16;
    trans.tx_buffer = tx_buf;

    esy_err_t ret = spi_transmit(c, &trans);
    if (ret != ESY_OK) return ret;
    
    return (int32_t)len;
}

int32_t esy_hb_spi_read16_blocking(esy_spi_t *spi, uint16_t *rx_buf, size_t len) {
    if (!spi || !spi_context(spi) || !rx_buf || !len ||
        len > ESP32_SPI_MAX_TRANSFER_BYTES / sizeof(uint16_t)) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled || c->role == ESY_SPI_ROLE_SLAVE) {
        return ESY_ERR_STATE;
    }


    spi_transaction_t trans = {0};
    trans.length = len * 16;
    trans.rxlength = len * 16;
    trans.rx_buffer = rx_buf;

    esy_err_t ret = spi_transmit(c, &trans);
    if (ret != ESY_OK) return ret;
    
    return (int32_t)len;
}

int32_t esy_hb_spi_transfer16_blocking(esy_spi_t *spi, const uint16_t *tx_buf,
                                          uint16_t *rx_buf, size_t len) {
    if (!spi || !spi_context(spi) || !tx_buf || !rx_buf || !len ||
        len > ESP32_SPI_MAX_TRANSFER_BYTES / sizeof(uint16_t)) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled || c->role == ESY_SPI_ROLE_SLAVE) {
        return ESY_ERR_STATE;
    }


    spi_transaction_t trans = {0};
    trans.length = len * 16;
    trans.rxlength = len * 16;
    trans.tx_buffer = tx_buf;
    trans.rx_buffer = rx_buf;

    esy_err_t ret = spi_transmit(c, &trans);
    if (ret != ESY_OK) return ret;
    
    return (int32_t)len;
}

esy_err_t esy_hb_spi_transfer_dma(esy_spi_t *spi, esy_dma_config_t *tx_dma_config,
                                     esy_dma_config_t *rx_dma_config,
                                     esy_spi_dma_complete_cb_t complete_cb,
                                     void *user_data) {
    (void)spi; (void)tx_dma_config; (void)rx_dma_config;
    (void)complete_cb; (void)user_data;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_spi_get_dma_trans_count_remaining(const esy_spi_t *spi,
                                                       size_t *out_tx_remaining,
                                                       size_t *out_rx_remaining) {
    if (!spi || !spi_context(spi)) {
        return ESY_ERR_INVALID_ARG;
    }
    if ((out_tx_remaining == NULL) && (out_rx_remaining == NULL)) {
        return ESY_ERR_INVALID_ARG;
    }

    if (out_tx_remaining) {
        *out_tx_remaining = 0u;
    }
    if (out_rx_remaining) {
        *out_rx_remaining = 0u;
    }
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_spi_set_speed(esy_spi_t *spi, uint32_t speed_hz) {
    if (!spi || !spi_context(spi) || !speed_hz) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled || c->role != ESY_SPI_ROLE_MASTER) {
        return ESY_ERR_STATE;
    }


    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_spi_abort(esy_spi_t *spi) {
    if (spi_context(spi) == NULL) return ESY_ERR_INVALID_ARG;
    return ESY_OK;
}

esy_err_t esy_hb_spi_get_caps(const esy_spi_t *spi, uint32_t *out_caps) {
    if (!spi || !spi_context(spi) || !out_caps) return ESY_ERR_INVALID_ARG;
    const esp32_spi_ctx_t *c = spi_context(spi);
    if (!c->enabled) return ESY_ERR_STATE;
    *out_caps = c->role != ESY_SPI_ROLE_SLAVE
                    ? ESY_SPI_CAP_TRANSFER16
                    : 0u;
    return ESY_OK;
}

esy_err_t esy_hb_spi_get_phys_index(const esy_spi_t *spi, uint8_t *out_index) {
    if (!spi || !spi_context(spi) || !out_index) {
        return ESY_ERR_INVALID_ARG;
    }
    
    const esp32_spi_ctx_t *c = spi_context(spi);
    *out_index = c->phys_index;
    return ESY_OK;
}

void esy_hb_spi_handle_irq(esy_spi_t *spi) {
    (void)spi;
}

bool esy_hb_spi_is_busy(esy_spi_t *spi) {
    (void)spi;
    return false;
}
