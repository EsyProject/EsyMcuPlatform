/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_i2c.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#define ESP32_I2C_MAX_SPEED_HZ 1000000u
#define ESP32_I2C_GLITCH_IGNORE_COUNT 7
#define ESP32_I2C_MAX_OPERATIONS 8
#define ESP32_I2C_10BIT_ADDRESS_PREFIX 0xf0u

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t device;
    esy_i2c_t *bound_handle;
    esy_i2c_addr_mode_t addr_mode;
    uint8_t phys_index;
    bool enabled;
    bool transfer_active;
    bool callback_active;
    bool cleanup_pending;
} esp32_i2c_ctx_t;

typedef struct {
    _Atomic(esy_i2c_t *) owners[SOC_I2C_NUM];
} esy_hb_i2c_module_ctx_t;

static esy_hb_i2c_module_ctx_t s_ctx;

_Static_assert(ESY_I2C_PRIV_BYTES >= sizeof(esp32_i2c_ctx_t),
               "ESY_I2C_PRIV_BYTES too small");
_Static_assert(SOC_I2C_CMD_REG_NUM >= ESP32_I2C_MAX_OPERATIONS,
               "ESP32 I2C requires eight native command slots");

static esy_err_t i2c_error(esp_err_t error) {
    switch (error) {
        case ESP_OK: return ESY_OK;
        case ESP_ERR_INVALID_ARG: return ESY_ERR_INVALID_ARG;
        case ESP_ERR_INVALID_STATE: return ESY_ERR_STATE;
        case ESP_ERR_NO_MEM: return ESY_ERR_NO_MEM;
        case ESP_ERR_NOT_FOUND: return ESY_ERR_BUSY;
        case ESP_ERR_TIMEOUT: return ESY_ERR_TIMEOUT;
        default: return ESY_ERR_FAIL;
    }
}

static esp32_i2c_ctx_t *i2c_context(const esy_i2c_t *i2c) {
    if (i2c == NULL || i2c->ctx != (const void *)i2c->backend.bytes) {
        return NULL;
    }
    esp32_i2c_ctx_t *context = i2c->ctx;
    return context->bound_handle == i2c && context->phys_index < SOC_I2C_NUM &&
                   atomic_load_explicit(&s_ctx.owners[context->phys_index],
                                        memory_order_acquire) == i2c
               ? context : NULL;
}

esy_err_t esy_hb_i2c_claim(esy_i2c_t *i2c, uint8_t phys_index) {
    if (i2c == NULL || phys_index >= SOC_I2C_NUM) return ESY_ERR_INVALID_ARG;
    if (i2c->ctx != NULL) {
        const esp32_i2c_ctx_t *context = i2c_context(i2c);
        return context != NULL && context->phys_index == phys_index &&
                       !context->enabled && !context->cleanup_pending &&
                       context->bus == NULL && context->device == NULL
                   ? ESY_OK : ESY_ERR_STATE;
    }
    esy_i2c_t *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(
            &s_ctx.owners[phys_index], &expected, i2c, memory_order_acq_rel,
            memory_order_acquire)) return ESY_ERR_BUSY;
    esp32_i2c_ctx_t *context = (esp32_i2c_ctx_t *)i2c->backend.bytes;
    memset(context, 0, sizeof(*context));
    context->bound_handle = i2c;
    context->phys_index = phys_index;
    i2c->ctx = context;
    return ESY_OK;
}

bool esy_hb_i2c_is_busy(const esy_i2c_t *i2c) {
    const esp32_i2c_ctx_t *context = i2c_context(i2c);
    return context != NULL &&
           (context->transfer_active || context->callback_active);
}

static bool i2c_pins_are_valid(const esy_i2c_inner_config_t *inner) {
    const int sda = inner->sda_pin;
    const int scl = inner->scl_pin;
    return inner->sda_pin != ESY_PIN_UNUSED &&
           inner->scl_pin != ESY_PIN_UNUSED && sda != scl &&
           GPIO_IS_VALID_OUTPUT_GPIO(sda) && GPIO_IS_VALID_OUTPUT_GPIO(scl);
}

esy_err_t esy_hb_i2c_init_master(esy_i2c_t *i2c, const esy_i2c_config_t *config) {
    if (i2c == NULL || config == NULL || config->phys_index >= SOC_I2C_NUM ||
        !i2c_pins_are_valid(&config->inner) || config->speed_hz == 0u ||
        config->speed_hz > ESP32_I2C_MAX_SPEED_HZ ||
        (config->addr_mode != ESY_I2C_ADDR_7BIT &&
         config->addr_mode != ESY_I2C_ADDR_10BIT)) {
        return ESY_ERR_INVALID_ARG;
    }
    esy_err_t result = esy_hb_i2c_claim(i2c, config->phys_index);
    if (result != ESY_OK) return result;
    esp32_i2c_ctx_t *context = i2c_context(i2c);
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = config->phys_index,
        .sda_io_num = config->inner.sda_pin,
        .scl_io_num = config->inner.scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = ESP32_I2C_GLITCH_IGNORE_COUNT,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &context->bus);
    if (error != ESP_OK) return i2c_error(error);

    /* One addressless device avoids SDK allocation when the target changes.
     * No async queue or native callback is installed: stack jobs and buffers
     * are retired before each synchronous transaction returns. */
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_DEVICE_ADDRESS_NOT_USED,
        .scl_speed_hz = config->speed_hz,
    };
    error = i2c_master_bus_add_device(context->bus, &device_config,
                                    &context->device);
    if (error != ESP_OK) return i2c_error(error);

    context->addr_mode = config->addr_mode;
    context->enabled = true;
    return ESY_OK;
}

esy_err_t esy_hb_i2c_validate_slave_config(
    const esy_i2c_slave_config_t *config) {
    if (config == NULL) return ESY_ERR_INVALID_ARG;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_i2c_init_slave(esy_i2c_t *i2c,
                               const esy_i2c_slave_config_t *config) {
    if (i2c == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
    return esy_hb_i2c_validate_slave_config(config);
}

esy_err_t esy_hb_i2c_suspend(esy_i2c_t *i2c) {
    if (i2c == NULL) return ESY_ERR_INVALID_ARG;
    esp32_i2c_ctx_t *context = i2c_context(i2c);
    if (context == NULL) return ESY_ERR_STATE;
    if (esy_hb_i2c_is_busy(i2c)) return ESY_ERR_BUSY;
    context->cleanup_pending = true;
    context->enabled = false;

    if (context->device != NULL) {
        esp_err_t error = i2c_master_bus_rm_device(context->device);
        if (error != ESP_OK) return i2c_error(error);
        context->device = NULL;
    }
    if (context->bus != NULL) {
        esp_err_t error = i2c_del_master_bus(context->bus);
        if (error != ESP_OK) return i2c_error(error);
        context->bus = NULL;
    }
    context->cleanup_pending = false;
    return ESY_OK;
}

esy_err_t esy_hb_i2c_deinit(esy_i2c_t *i2c) {
    if (i2c == NULL) return ESY_ERR_INVALID_ARG;
    if (i2c->ctx == NULL) return ESY_OK;
    esp32_i2c_ctx_t *context = i2c_context(i2c);
    if (context == NULL) return ESY_ERR_STATE;
    esy_err_t result = esy_hb_i2c_suspend(i2c);
    if (result != ESY_OK) return result;
    uint8_t phys_index = context->phys_index;
    memset(context, 0, sizeof(*context));
    i2c->ctx = NULL;
    atomic_store_explicit(&s_ctx.owners[phys_index], NULL, memory_order_release);
    return ESY_OK;
}

static int32_t i2c_transfer_until(esp32_i2c_ctx_t *context,
                                  const esy_i2c_xfer_t *xfer,
                                  int64_t deadline_us) {
    const bool ten_bit = context->addr_mode == ESY_I2C_ADDR_10BIT;

    /* A STOP completes a native transaction. Separate write/read phases each
     * share the original deadline and rebuild the full 10-bit read prefix. */
    if (xfer->tx_len > 0u && xfer->rx_len > 0u && !xfer->repeated_start) {
        esy_i2c_xfer_t phase = *xfer;
        phase.rx = NULL;
        phase.rx_len = 0u;
        int32_t result = i2c_transfer_until(context, &phase, deadline_us);
        if (result < 0) return result;
        phase = *xfer;
        phase.tx = NULL;
        phase.tx_len = 0u;
        result = i2c_transfer_until(context, &phase, deadline_us);
        return result < 0 ? result : (int32_t)(xfer->tx_len + xfer->rx_len);
    }

    uint8_t write_address[2] = {
        ten_bit ? (uint8_t)(ESP32_I2C_10BIT_ADDRESS_PREFIX |
                            ((xfer->addr >> 7u) & 0x06u))
                : (uint8_t)(xfer->addr << 1u),
        (uint8_t)xfer->addr,
    };
    uint8_t read_address = write_address[0] | 1u;
    i2c_operation_job_t operations[ESP32_I2C_MAX_OPERATIONS] = {0};
    size_t count = 0u;
    if (xfer->tx_len > 0u || ten_bit) {
        operations[count++] = (i2c_operation_job_t){.command = I2C_MASTER_CMD_START};
        operations[count++] = (i2c_operation_job_t){
            .command = I2C_MASTER_CMD_WRITE,
            .write = {.ack_check = true, .data = write_address,
                      .total_bytes = ten_bit ? 2u : 1u},
        };
        if (xfer->tx_len > 0u) {
            operations[count++] = (i2c_operation_job_t){
                .command = I2C_MASTER_CMD_WRITE,
                .write = {.ack_check = true, .data = (uint8_t *)xfer->tx,
                          .total_bytes = xfer->tx_len},
            };
        }
        if (xfer->rx_len == 0u) {
            operations[count++] = (i2c_operation_job_t){.command = I2C_MASTER_CMD_STOP};
        }
    }
    if (xfer->rx_len > 0u) {
        operations[count++] = (i2c_operation_job_t){.command = I2C_MASTER_CMD_START};
        operations[count++] = (i2c_operation_job_t){
            .command = I2C_MASTER_CMD_WRITE,
            .write = {.ack_check = true, .data = &read_address, .total_bytes = 1u},
        };
        if (xfer->rx_len > 1u) {
            operations[count++] = (i2c_operation_job_t){
                .command = I2C_MASTER_CMD_READ,
                .read = {.ack_value = I2C_ACK_VAL, .data = xfer->rx,
                         .total_bytes = xfer->rx_len - 1u},
            };
        }
        operations[count++] = (i2c_operation_job_t){
            .command = I2C_MASTER_CMD_READ,
            .read = {.ack_value = I2C_NACK_VAL,
                     .data = xfer->rx + xfer->rx_len - 1u, .total_bytes = 1u},
        };
        operations[count++] = (i2c_operation_job_t){.command = I2C_MASTER_CMD_STOP};
    }

    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) return ESY_ERR_TIMEOUT;
    /* Native waits have millisecond/tick granularity; round the remaining
     * fraction once for this submission, never reset the transaction budget. */
    const int timeout_ms = (int)((remaining_us + 999) / 1000);
    esp_err_t error = i2c_master_execute_defined_operations(
        context->device, operations, count, timeout_ms);
    if (error == ESP_ERR_TIMEOUT || esp_timer_get_time() >= deadline_us)
        return ESY_ERR_TIMEOUT;
    return error == ESP_OK ? (int32_t)(xfer->tx_len + xfer->rx_len)
                           : i2c_error(error);
}

int32_t esy_hb_i2c_transfer(esy_i2c_t *i2c, const esy_i2c_xfer_t *xfer) {
    if (i2c == NULL || xfer == NULL ||
        (xfer->tx_len == 0u && xfer->rx_len == 0u) ||
        (xfer->tx_len > 0u && xfer->tx == NULL) ||
        (xfer->rx_len > 0u && xfer->rx == NULL) ||
        xfer->tx_len > INT32_MAX ||
        xfer->rx_len > (size_t)INT32_MAX - xfer->tx_len ||
        xfer->timeout_ms > INT32_MAX) return ESY_ERR_INVALID_ARG;
    esp32_i2c_ctx_t *context = i2c_context(i2c);
    if (context == NULL || !context->enabled) return ESY_ERR_STATE;
    if (context->transfer_active) return ESY_ERR_BUSY;
    const bool ten_bit = context->addr_mode == ESY_I2C_ADDR_10BIT;
    if (xfer->addr > (ten_bit ? ESY_I2C_ADDR_10BIT_MAX : ESY_I2C_ADDR_7BIT_MAX))
        return ESY_ERR_INVALID_ARG;
    const uint32_t budget_ms = xfer->timeout_ms == 0u
                                  ? ESY_I2C_DEFAULT_TIMEOUT_MS : xfer->timeout_ms;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)budget_ms * 1000;
    context->transfer_active = true;
    int32_t result = i2c_transfer_until(context, xfer, deadline_us);
    context->transfer_active = false;
    return result;
}

int32_t esy_hb_i2c_write_blocking(esy_i2c_t *i2c, uint16_t addr,
                                 const uint8_t *data, size_t len) {
    if (data == NULL || len == 0u) return ESY_ERR_INVALID_ARG;
    const esy_i2c_xfer_t transfer = {.addr = addr, .tx = data, .tx_len = len};
    return esy_hb_i2c_transfer(i2c, &transfer);
}

int32_t esy_hb_i2c_read_blocking(esy_i2c_t *i2c, uint16_t addr,
                                uint8_t *data, size_t len) {
    if (data == NULL || len == 0u) return ESY_ERR_INVALID_ARG;
    const esy_i2c_xfer_t transfer = {.addr = addr, .rx = data, .rx_len = len};
    return esy_hb_i2c_transfer(i2c, &transfer);
}

esy_err_t esy_hb_i2c_write_dma(esy_i2c_t *i2c, uint16_t addr,
                              esy_dma_config_t *dma_config) {
    if (dma_config == NULL) return ESY_ERR_INVALID_ARG;
    int32_t result = esy_hb_i2c_write_blocking(
        i2c, addr, dma_config->src, dma_config->length);
    if (result < 0) return (esy_err_t)result;
    esp32_i2c_ctx_t *context = i2c_context(i2c);
    context->callback_active = true;
    if (dma_config->callback != NULL) dma_config->callback(dma_config->callback_arg);
    context->callback_active = false;
    return ESY_OK;
}

esy_err_t esy_hb_i2c_read_dma(esy_i2c_t *i2c, uint16_t addr,
                             esy_dma_config_t *dma_config) {
    if (dma_config == NULL) return ESY_ERR_INVALID_ARG;
    int32_t result = esy_hb_i2c_read_blocking(
        i2c, addr, dma_config->dst, dma_config->length);
    if (result < 0) return (esy_err_t)result;
    esp32_i2c_ctx_t *context = i2c_context(i2c);
    context->callback_active = true;
    if (dma_config->callback != NULL) dma_config->callback(dma_config->callback_arg);
    context->callback_active = false;
    return ESY_OK;
}

esy_err_t esy_hb_i2c_set_speed(esy_i2c_t *i2c, uint32_t speed_hz) {
    if (i2c == NULL || speed_hz == 0u) return ESY_ERR_INVALID_ARG;
    const esp32_i2c_ctx_t *context = i2c_context(i2c);
    if (context == NULL || !context->enabled) return ESY_ERR_STATE;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_i2c_abort(esy_i2c_t *i2c) {
    if (i2c == NULL) return ESY_ERR_INVALID_ARG;
    const esp32_i2c_ctx_t *context = i2c_context(i2c);
    if (context == NULL || !context->enabled) return ESY_ERR_STATE;
    return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_i2c_recover_bus(esy_i2c_t *i2c) {
    if (i2c == NULL) return ESY_ERR_INVALID_ARG;
    esp32_i2c_ctx_t *context = i2c_context(i2c);
    if (context == NULL || !context->enabled) return ESY_ERR_STATE;
    if (context->transfer_active) return ESY_ERR_BUSY;
    return i2c_error(i2c_master_bus_reset(context->bus));
}

esy_err_t esy_hb_i2c_get_caps(const esy_i2c_t *i2c, uint32_t *out_caps) {
    if (i2c == NULL || out_caps == NULL) return ESY_ERR_INVALID_ARG;
    const esp32_i2c_ctx_t *context = i2c_context(i2c);
    if (context == NULL || !context->enabled) return ESY_ERR_STATE;
    *out_caps = ESY_I2C_CAP_MASTER_TRANSFER | ESY_I2C_CAP_REPEATED_START |
                ESY_I2C_CAP_DMA_TRANSFER | ESY_I2C_CAP_RECOVERY;
    return ESY_OK;
}

esy_err_t esy_hb_i2c_get_phys_index(const esy_i2c_t *i2c, uint8_t *out_index) {
    if (i2c == NULL || out_index == NULL) return ESY_ERR_INVALID_ARG;
    const esp32_i2c_ctx_t *context = i2c_context(i2c);
    if (context == NULL) return ESY_ERR_STATE;
    *out_index = context->phys_index;
    return ESY_OK;
}
