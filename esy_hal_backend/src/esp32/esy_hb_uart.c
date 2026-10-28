/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_uart.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/uart.h"
#include "hal/uart_ll.h"
#include "esp_rom_sys.h"

#define ESP32_UART_MAX 3
#define ESP32_UART_BUF_SIZE 1024
#define ESP32_UART_RX_POLL_BUDGET 1024u
#define ESP32_UART_BLOCKING_POLL_US 50u

typedef struct {
    uart_port_t uart_num;
    uint8_t phys_index;
    bool enabled;
    bool suspended;
    bool driver_installed;

    uint32_t baudrate;
    uint8_t data_bits;
    uint8_t stop_bits;
    esy_uart_parity_t parity;

    bool tx_in_progress;
    _Atomic uint32_t rx_in_progress;
    
    esy_dma_callback_t tx_callback;
    void *tx_callback_arg;

    uint8_t *rx_buffer;
    size_t rx_length;
    size_t rx_received;
    esy_dma_callback_t rx_callback;
    void *rx_callback_arg;
    esy_dma_callback_t rx_error_callback;
    void *rx_error_arg;

    esy_uart_t *bound_handle;
} esp32_uart_ctx_t;

_Static_assert(ESY_UART_PRIV_BYTES >= sizeof(esp32_uart_ctx_t),
               "ESY_UART_PRIV_BYTES too small");

typedef struct {
  _Atomic(esy_uart_t *) owners[ESP32_UART_MAX];
} esy_hb_uart_ctx_t;

static esy_hb_uart_ctx_t s_ctx;

static inline uart_parity_t esp_parity(esy_uart_parity_t p) {
    return (p == ESY_UART_PARITY_NONE) ? UART_PARITY_DISABLE
         : (p == ESY_UART_PARITY_EVEN) ? UART_PARITY_EVEN
                                       : UART_PARITY_ODD;
}

static inline uart_stop_bits_t esp_stop_bits(uint8_t stop_bits) {
    return (stop_bits == 1) ? UART_STOP_BITS_1 : UART_STOP_BITS_2;
}

static inline uart_word_length_t esp_data_bits(uint8_t data_bits) {
    switch (data_bits) {
    case 5:
      return UART_DATA_5_BITS;
    case 6:
      return UART_DATA_6_BITS;
    case 7:
      return UART_DATA_7_BITS;
    case 8:
      return UART_DATA_8_BITS;
    default:
      return UART_DATA_8_BITS;
    }
}

/* One externally serialized owner submits, polls, aborts and tears down UART.
 * Poll alone consumes finite RX storage; it reads at most one fixed byte budget
 * and invokes completion in that owner context. Keep busy through callback
 * return; callbacks may notify but must not reenter UART lifecycle APIs. */
static void finish_receive(esp32_uart_ctx_t *c, bool success) {
    esy_dma_callback_t callback = success ? c->rx_callback : c->rx_error_callback;
    void *arg = success ? c->rx_callback_arg : c->rx_error_arg;
    if (callback != NULL) callback(arg);
    atomic_store_explicit(&c->rx_in_progress, 0u, memory_order_release);
}

static void receive_pending(esp32_uart_ctx_t *c) {
    uint32_t expected = 1u;
    if (!atomic_compare_exchange_strong_explicit(&c->rx_in_progress, &expected,
            2u, memory_order_acq_rel, memory_order_acquire)) return;
    uint32_t budget = ESP32_UART_RX_POLL_BUDGET;
    while (budget != 0u) {
        size_t available = 0u;
        if (uart_get_buffered_data_len(c->uart_num, &available) != ESP_OK) {
            finish_receive(c, false);
            return;
        }
        size_t to_read = c->rx_length - c->rx_received;
        if (to_read > available) to_read = available;
        if (to_read > budget) to_read = budget;
        if (to_read == 0u) break;
        const int count = uart_read_bytes(c->uart_num,
            c->rx_buffer + c->rx_received, to_read, 0);
        if (count < 0) {
            finish_receive(c, false);
            return;
        }
        if (count == 0) break;
        c->rx_received += (size_t)count;
        budget -= (uint32_t)count;
        if (c->rx_received == c->rx_length) {
            finish_receive(c, true);
            return;
        }
    }
    atomic_store_explicit(&c->rx_in_progress, 1u, memory_order_release);
}

/* Each controller has one owner across init, suspend and failed cleanup.
 * Atomic slot claims cover independent owner tasks/cores; per-handle operations
 * remain serialized by its caller. IRQ dispatch is published only after init. */
static esp32_uart_ctx_t *uart_context(const esy_uart_t *uart) {
  if (uart == NULL || uart->ctx[0] != (const void *)uart->backend.bytes)
    return NULL;
  /* Establish the concrete backend type through the claim table before reading
   * opaque storage: an XIO handle may also place ctx[0] at backend.bytes. */
  for (uint8_t index = 0u; index < ESP32_UART_MAX; ++index) {
    if (atomic_load_explicit(&s_ctx.owners[index], memory_order_acquire) != uart)
      continue;
    esp32_uart_ctx_t *c = (esp32_uart_ctx_t *)uart->ctx[0];
    return c->phys_index == index && c->bound_handle == uart ? c : NULL;
  }
  return NULL;
}

esy_err_t esy_hb_uart_claim(esy_uart_t *uart, uint8_t phys_index) {
  if (uart == NULL || phys_index >= ESP32_UART_MAX) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] != NULL || uart->ctx[1] != NULL) {
    esp32_uart_ctx_t *c = uart_context(uart);
    return c != NULL && c->phys_index == phys_index && !c->enabled && !c->suspended &&
           !c->driver_installed
               ? ESY_OK : ESY_ERR_STATE;
  }
  esy_uart_t *expected = NULL;
  if (!atomic_compare_exchange_strong_explicit(
          &s_ctx.owners[phys_index], &expected, uart, memory_order_acq_rel,
          memory_order_acquire)) return ESY_ERR_BUSY;
  /* SDK console/native users must not be reconfigured by this facade. Their
   * lifecycle must be externally excluded from this controller after claim. */
  if (uart_is_driver_installed((uart_port_t)phys_index)) {
    atomic_store_explicit(&s_ctx.owners[phys_index], NULL, memory_order_release);
    return ESY_ERR_BUSY;
  }
  esp32_uart_ctx_t *c = (esp32_uart_ctx_t *)uart->backend.bytes;
  memset(c, 0, sizeof(*c));
  c->phys_index = phys_index;
  c->bound_handle = uart;
  atomic_init(&c->rx_in_progress, 0u);
  c->uart_num = (uart_port_t)phys_index;
  uart->ctx[0] = c;
  return ESY_OK;
}

esy_err_t esy_hb_uart_release(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] == NULL) return ESY_OK;
  esp32_uart_ctx_t *c = uart_context(uart);
  if (c == NULL) return ESY_ERR_STATE;
  if (c->enabled || c->driver_installed) return ESY_ERR_BUSY;
  uint8_t index = c->phys_index;
  c->bound_handle = NULL;
  uart->ctx[0] = NULL;
  atomic_store_explicit(&s_ctx.owners[index], NULL, memory_order_release);
  return ESY_OK;
}

esy_err_t esy_hb_uart_validate_config(const esy_uart_config_t *cfg) {
    if (!cfg || cfg->phys_index >= ESP32_UART_MAX ||
        cfg->baudrate == 0u || cfg->data_bits < 5u || cfg->data_bits > 8u ||
        (cfg->stop_bits != 1u && cfg->stop_bits != 2u) ||
        (cfg->parity != ESY_UART_PARITY_NONE &&
         cfg->parity != ESY_UART_PARITY_EVEN &&
         cfg->parity != ESY_UART_PARITY_ODD)) return ESY_ERR_INVALID_ARG;
    return ESY_OK;
}

esy_err_t esy_hb_uart_init(esy_uart_t *uart, const esy_uart_config_t *cfg) {
    if (!uart) return ESY_ERR_INVALID_ARG;
    esy_err_t validation = esy_hb_uart_validate_config(cfg);
    if (validation != ESY_OK) return validation;
    esy_err_t claim = esy_hb_uart_claim(uart, cfg->phys_index);
    if (claim != ESY_OK) return claim;
    esp32_uart_ctx_t *c = uart_context(uart);
    
    c->uart_num = (uart_port_t)cfg->phys_index;
    c->phys_index = cfg->phys_index;
    c->baudrate = cfg->baudrate;
    c->data_bits = cfg->data_bits;
    c->stop_bits = cfg->stop_bits;
    c->parity = cfg->parity;
    atomic_init(&c->rx_in_progress, 0u);
    uart_config_t uart_config = {
        .baud_rate = cfg->baudrate,
        .data_bits = esp_data_bits(cfg->data_bits),
        .parity = esp_parity(cfg->parity),
        .stop_bits = esp_stop_bits(cfg->stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* Installation arbitrates with an externally installed SDK driver before
     * any parameter write. The public claim already excluded facade owners. */
    esp_err_t ret = uart_driver_install(
        c->uart_num, ESP32_UART_BUF_SIZE, ESP32_UART_BUF_SIZE,
        0, NULL, 0);
    if (ret != ESP_OK) return ret == ESP_ERR_INVALID_STATE ? ESY_ERR_BUSY : ESY_ERR_IO;
    c->driver_installed = true;
    ret = uart_param_config(c->uart_num, &uart_config);
    if (ret != ESP_OK) return ESY_ERR_IO;

    c->enabled = true;
    c->suspended = false;
    c->tx_in_progress = false;
    c->bound_handle = uart;
    uart->ctx[0] = c;

    return ESY_OK;
}

esy_err_t esy_hb_uart_deinit(esy_uart_t *uart) {
  if (uart == NULL) return ESY_ERR_INVALID_ARG;
  if (uart->ctx[0] == NULL) return ESY_OK;
  esp32_uart_ctx_t *c = uart_context(uart);
  if (c == NULL) return ESY_ERR_STATE;
  if (c->tx_in_progress ||
      atomic_load_explicit(&c->rx_in_progress, memory_order_acquire) != 0u ||
      esy_hb_uart_is_transmitting(uart)) return ESY_ERR_BUSY;
  /* No worker can retain caller storage: finite RX and callbacks retire
   * synchronously in the owner poll before this cleanup is admitted. */
  c->enabled = false;
  c->suspended = true;
  if (c->driver_installed) {
    if (uart_driver_delete(c->uart_num) != ESP_OK) return ESY_ERR_IO;
    c->driver_installed = false;
  }
  return ESY_OK;
}

int32_t esy_hb_uart_send_blocking(esy_uart_t *uart, const uint8_t *data,
                                      size_t len) {
    if (!uart || !data || !len) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_uart_ctx_t *c = uart_context(uart);
    if (!c || !c->enabled || c->suspended) {
        return ESY_ERR_STATE;
    }

    int bytes_written = uart_write_bytes(c->uart_num, data, len);
    if (bytes_written < 0) {
        return ESY_ERR_FAIL;
    }

    esp_err_t completion;
    while ((completion = uart_wait_tx_done(c->uart_num, 0)) == ESP_ERR_TIMEOUT)
        esp_rom_delay_us(ESP32_UART_BLOCKING_POLL_US);
    if (completion != ESP_OK) return ESY_ERR_IO;
    
    return bytes_written;
}

int32_t esy_hb_uart_receive_blocking(esy_uart_t *uart, uint8_t *data,
                                         size_t len) {
    if (!uart || !data || !len) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_uart_ctx_t *c = uart_context(uart);
    if (!c || !c->enabled || c->suspended) {
        return ESY_ERR_STATE;
    }
    if (atomic_load_explicit(&c->rx_in_progress, memory_order_acquire) != 0u)
        return ESY_ERR_BUSY;

    size_t received = 0u;
    while (received < len) {
        const int count = uart_read_bytes(c->uart_num, data + received,
                                         len - received, 0);
        if (count < 0) return ESY_ERR_IO;
        received += (size_t)count;
        if (count == 0) esp_rom_delay_us(ESP32_UART_BLOCKING_POLL_US);
    }
    return (int32_t)received;
}

esy_err_t esy_hb_uart_send_dma(esy_uart_t *uart,
                                   esy_dma_config_t *dma_cfg) {
    if (!uart || !dma_cfg || !dma_cfg->src || !dma_cfg->length) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_uart_ctx_t *c = uart_context(uart);
    if (!c || !c->enabled || c->suspended) {
        return ESY_ERR_STATE;
    }

    if (c->tx_in_progress) {
        return ESY_ERR_BUSY;
    }

    if (dma_cfg->circular) return ESY_ERR_NOT_SUPPORTED;
    c->tx_callback = dma_cfg->callback;
    c->tx_callback_arg = dma_cfg->callback_arg;
    c->tx_in_progress = true;

  int bytes_written =
      uart_write_bytes(c->uart_num, dma_cfg->src, dma_cfg->length);
    if (bytes_written < 0 || (size_t)bytes_written != dma_cfg->length) {
        c->tx_in_progress = false;
        return ESY_ERR_FAIL;
    }

    esp_err_t completion;
    while ((completion = uart_wait_tx_done(c->uart_num, 0)) == ESP_ERR_TIMEOUT)
        esp_rom_delay_us(ESP32_UART_BLOCKING_POLL_US);
    c->tx_in_progress = false;
    if (completion != ESP_OK) return ESY_ERR_IO;

    if (c->tx_callback) {
        c->tx_callback(c->tx_callback_arg);
    }

    return ESY_OK;
}

esy_err_t esy_hb_uart_receive_dma(esy_uart_t *uart,
                                      esy_dma_config_t *dma_cfg) {
    if (!uart || !dma_cfg || !dma_cfg->dst || !dma_cfg->length) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_uart_ctx_t *c = uart_context(uart);
    if (!c || !c->enabled || c->suspended) {
        return ESY_ERR_STATE;
    }

    if (atomic_load_explicit(&c->rx_in_progress, memory_order_acquire) != 0u) {
        return ESY_ERR_BUSY;
    }

    if (dma_cfg->circular) return ESY_ERR_NOT_SUPPORTED;
    c->rx_buffer = (uint8_t*)dma_cfg->dst;
    c->rx_length = dma_cfg->length;
    c->rx_received = 0;
    c->rx_callback = dma_cfg->callback;
    c->rx_callback_arg = dma_cfg->callback_arg;
    c->rx_error_callback = dma_cfg->error_callback;
    c->rx_error_arg = dma_cfg->error_callback_arg;
    atomic_store_explicit(&c->rx_in_progress, 1u, memory_order_release);

    /* The owner must poll even when bytes predate this submission; no
     * background task or future UART event is required for progress. */
    return ESY_OK;
}

esy_err_t esy_hb_uart_abort_dma(esy_uart_t *uart, bool is_tx) {
    if (uart_context(uart) == NULL) return ESY_ERR_INVALID_ARG;
    esp32_uart_ctx_t *c = uart_context(uart);
    if (is_tx) return c->tx_in_progress ? ESY_ERR_BUSY : ESY_OK;
    /* A reentrant abort cannot detach storage while poll executes a callback. */
    uint32_t expected = 1u;
    if (!atomic_compare_exchange_strong_explicit(&c->rx_in_progress, &expected,
            0u, memory_order_acq_rel, memory_order_acquire))
        return expected == 0u ? ESY_OK : ESY_ERR_BUSY;
    c->rx_buffer = NULL;
    c->rx_length = 0u;
    c->rx_received = 0u;
    c->rx_callback = NULL;
    c->rx_callback_arg = NULL;
    c->rx_error_callback = NULL;
    c->rx_error_arg = NULL;
    return ESY_OK;
}

esy_err_t esy_hb_uart_poll(esy_uart_t *uart) {
    esp32_uart_ctx_t *c = uart_context(uart);
    if (c == NULL) return ESY_ERR_INVALID_ARG;
    if (!c->enabled || c->suspended) return ESY_ERR_STATE;
    receive_pending(c);
    return ESY_OK;
}

bool esy_hb_uart_is_dma_busy(const esy_uart_t *uart, bool is_tx) {
    if (uart_context(uart) == NULL) return false;
    const esp32_uart_ctx_t *c = uart_context(uart);
    return is_tx ? c->tx_in_progress :
        atomic_load_explicit(&c->rx_in_progress, memory_order_acquire) != 0u;
}

esy_err_t esy_hb_uart_rx_stream_start(
    esy_uart_t *uart, uint8_t *storage, size_t storage_size,
    esy_uart_rx_stream_callback_t callback, void *user_data) {
  (void)uart;
  (void)storage;
  (void)storage_size;
  (void)callback;
  (void)user_data;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_uart_rx_stream_peek(esy_uart_t *uart, const uint8_t **data,
                                         size_t *length) {
  (void)uart;
  if (data) *data = NULL;
  if (length) *length = 0u;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_uart_rx_stream_consume(esy_uart_t *uart, size_t length) {
  (void)uart;
  (void)length;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_uart_rx_stream_stop(esy_uart_t *uart) {
  (void)uart;
  return ESY_ERR_NOT_SUPPORTED;
}

void esy_hb_uart_handle_irq(uint8_t phys_index) { (void)phys_index; }

esy_err_t esy_hb_uart_get_dma_trans_count_remaining(
    const esy_uart_t *uart, size_t *out_tx_remaining,
                                                       size_t *out_rx_remaining) {
    if (!uart || uart_context(uart) == NULL) {
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

bool esy_hb_uart_is_transmitting(const esy_uart_t *uart) {
    const esp32_uart_ctx_t *c;

    if (!uart || uart_context(uart) == NULL) return false;
    c = uart_context(uart);
    if (!c->enabled || c->suspended) return false;
    return c->tx_in_progress ||
           !uart_ll_is_tx_idle(UART_LL_GET_HW(c->uart_num));
}

esy_err_t esy_hb_uart_suspend(esy_uart_t *uart) {
    if (!uart) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_uart_ctx_t *c = uart_context(uart);
    if (!c || !c->enabled) {
        return ESY_ERR_STATE;
    }

    if (c->suspended) return ESY_OK;
    if (c->tx_in_progress ||
        atomic_load_explicit(&c->rx_in_progress, memory_order_acquire) != 0u ||
        !uart_ll_is_tx_idle(UART_LL_GET_HW(c->uart_num))) {
        return ESY_ERR_BUSY;
    }

    /* Preserve the SDK ring buffers and configuration while RX IRQs are
     * gated. No platform-owned task or event queue participates in suspend. */
    if (uart_disable_rx_intr(c->uart_num) != ESP_OK) return ESY_ERR_IO;
    c->suspended = true;
    
    return ESY_OK;
}

esy_err_t esy_hb_uart_resume(esy_uart_t *uart) {
    if (!uart) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_uart_ctx_t *c = uart_context(uart);
    if (!c || !c->enabled) {
        return ESY_ERR_STATE;
    }
    if (!c->suspended) return ESY_OK;
    /* The installed driver retains configuration and its RX ring buffer.
     * On failure leave the suspended state and all resources available to retry. */
    if (uart_enable_rx_intr(c->uart_num) != ESP_OK) return ESY_ERR_IO;

    c->suspended = false;
    
    return ESY_OK;
}

bool esy_hb_uart_is_readable(const esy_uart_t *uart) {
    if (!uart) return false;
    
    const esp32_uart_ctx_t *c = uart_context(uart);
    if (!c || !c->enabled || c->suspended) return false;

    size_t bytes_available = 0;
    uart_get_buffered_data_len(c->uart_num, &bytes_available);
    return (bytes_available > 0);
}

esy_err_t esy_hb_uart_set_baudrate(esy_uart_t *uart, uint32_t baudrate) {
    if (!uart || !baudrate) {
        return ESY_ERR_INVALID_ARG;
    }
    
    esp32_uart_ctx_t *c = uart_context(uart);
    if (!c || !c->enabled || c->suspended) {
        return ESY_ERR_STATE;
    }

    esp_err_t ret = uart_set_baudrate(c->uart_num, baudrate);
    if (ret != ESP_OK) {
        return ESY_ERR_FAIL;
    }

    c->baudrate = baudrate;
    
    return ESY_OK;
}

esy_err_t esy_hb_uart_get_caps(const esy_uart_t *uart, uint32_t *out_caps) {
    if (!uart || uart_context(uart) == NULL || !out_caps) return ESY_ERR_INVALID_ARG;
    *out_caps = ESY_UART_CAP_TX_DMA | ESY_UART_CAP_RX_DMA;
    return ESY_OK;
}

esy_err_t esy_hb_uart_get_phys_index(const esy_uart_t *uart,
                                         uint8_t *out_index) {
    if (!uart || !out_index) {
        return ESY_ERR_INVALID_ARG;
    }
    
    const esp32_uart_ctx_t *c = uart_context(uart);
    if (!c) {
        return ESY_ERR_STATE;
    }
    
    *out_index = c->phys_index;
    return ESY_OK;
}
