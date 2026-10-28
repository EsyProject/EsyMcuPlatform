/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_UART_DEFS_H
#define ESY_UART_DEFS_H

/**
 * @file esy_uart_defs.h
 * @brief Generic UART type definitions and backend vtable.
 *
 * This file defines the platform-agnostic UART abstraction:
 * - a unified handle type,
 * - configuration structures,
 * - and a backend vtable (function table) bound by the HAL wrapper.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reserved backend private context size in the handle (bytes).
 *
 * HAL backend implementations may store internal state here to avoid dynamic
 * allocation. Override globally when an embedded backend needs more capacity;
 * all HAL/backend/consumer translation units must use the same value and be
 * rebuilt together. Backend capacity assertions validate the selected size.
 */
#ifndef ESY_UART_PRIV_BYTES
#define ESY_UART_PRIV_BYTES 448
#endif

/* -------------------------------------------------------------------------- */
/* Forward declarations and handle */
/* -------------------------------------------------------------------------- */

/**
 * @brief Forward declaration of the unified UART handle.
 *
 * Kept consistent with the definition used by the HAL backend implementation
 * (e.g. in esy_hb_uart.h), so it can be referenced by callbacks and
 * vtables.
 */
struct esy_uart_s;
struct esy_device_s;

/**
 * @brief UART handle type.
 */
typedef struct esy_uart_s esy_uart_t;

/**
 * @brief Public capability bits returned by the initialized handle's get_caps.
 *
 * Bits describe implemented operations for the current handle/role, subject to
 * board resources, configuration limits and normal runtime errors. A set bit
 * neither reserves a resource nor guarantees every parameter combination.
 * DMA-named bits describe the corresponding API, not native DMA or asynchronous
 * completion. Unknown bits must be ignored; zero is a valid capability mask.
 */
typedef enum {
  ESY_UART_CAP_TX_DMA = 1u << 0, /**< send_dma entry point; may complete synchronously. */
  ESY_UART_CAP_RX_DMA = 1u << 1, /**< receive_dma entry point; not a promise of native hardware DMA. */
  ESY_UART_CAP_RX_STREAM = 1u << 2, /**< Caller-owned continuous RX stream start/peek/consume/stop. */
  ESY_UART_CAP_DMA_REMAINING = 1u << 3, /**< DMA remaining-count query for board-provided routes. */
} esy_uart_cap_t;

/**
 * @brief UART RX stream notification reason.
 *
 * Notifications may run in interrupt context. They indicate that the owner
 * should wake its consumer; they do not identify protocol frame boundaries.
 */
typedef enum {
  ESY_UART_RX_STREAM_EVENT_DATA = 0,
  ESY_UART_RX_STREAM_EVENT_OVERFLOW,
  ESY_UART_RX_STREAM_EVENT_ERROR
} esy_uart_rx_stream_event_t;

/**
 * @brief UART RX stream notification callback.
 *
 * @param[in] uart UART handle that produced the event.
 * @param[in] event Notification reason.
 * @param[in] user_data Caller argument supplied at stream start.
 */
typedef void (*esy_uart_rx_stream_callback_t)(esy_uart_t *uart,
                                              esy_uart_rx_stream_event_t event,
                                              void *user_data);

/* -------------------------------------------------------------------------- */
/* Enums and configuration */
/* -------------------------------------------------------------------------- */

/**
 * @brief UART parity configuration.
 */
typedef enum {
  ESY_UART_PARITY_NONE, /**< No parity */
  ESY_UART_PARITY_EVEN, /**< Even parity */
  ESY_UART_PARITY_ODD   /**< Odd parity */
} esy_uart_parity_t;

/**
 * @brief UART controller internal configuration.
 *
 * This struct holds platform-specific parameters that should not leak into the
 * top-level API fields (similar to the "inner" pattern used by other
 * peripherals).
 */
typedef struct {
  uint8_t irq_priority; /**< Interrupt priority (platform-defined; lower value =
                           higher priority) */
} esy_uart_inner_config_t;

/**
 * @brief UART configuration.
 *
 * @note @ref phys_index selects the physical UART controller instance.
 */
typedef struct {
  uint8_t phys_index;            /**< Physical controller index */
  uint32_t baudrate;             /**< Baud rate (bit/s) */
  uint8_t data_bits;             /**< Data bits (typically 7 or 8) */
  uint8_t stop_bits;             /**< Stop bits (typically 1 or 2) */
  esy_uart_parity_t parity;      /**< Parity setting */
  esy_uart_inner_config_t inner; /**< Controller internal configuration */
} esy_uart_config_t;

/* -------------------------------------------------------------------------- */
/* Vtable definition */
/* -------------------------------------------------------------------------- */

/**
 * @brief UART backend vtable.
 *
 * All functions take @ref esy_uart_t* as the first parameter.
 *
 * Return convention:
 * - Blocking I/O APIs return a non-negative value on success (bytes
 * transferred), and a negative error code on failure.
 * - Control APIs return @ref esy_err_t (ESY_OK or a negative error code).
 * - Query APIs return `true/false` where applicable.
 */
typedef struct {
  /**
   * @brief Blocking transmit.
   * @param[in] uart UART handle
   * @param[in] data Transmit buffer
   * @param[in] len  Number of bytes to send
   * @return Bytes sent on success; negative error code on failure
   */
  int32_t (*send_blocking)(esy_uart_t *uart, const uint8_t *data, size_t len);

  /**
   * @brief Blocking receive.
   * @param[in]  uart UART handle
   * @param[out] data Receive buffer
   * @param[in]  len  Number of bytes expected to receive
   * @return Bytes received on success; negative error code on failure
   */
  int32_t (*receive_blocking)(esy_uart_t *uart, uint8_t *data, size_t len);

  /**
   * @brief Start DMA transmit.
   * @param[in] uart    UART handle
   * @param[in] dma_cfg DMA configuration
   * @return ESY_OK on success; negative error code on failure
   */
  esy_err_t (*send_dma)(esy_uart_t *uart, esy_dma_config_t *dma_cfg);

  /**
   * @brief Start DMA receive.
   * @param[in] uart    UART handle
   * @param[in] dma_cfg DMA configuration
   * @return ESY_OK on success; negative error code on failure
   */
  esy_err_t (*receive_dma)(esy_uart_t *uart, esy_dma_config_t *dma_cfg);

  /**
   * @brief Start continuous background RX into caller-owned storage.
   */
  esy_err_t (*rx_stream_start)(esy_uart_t *uart, uint8_t *storage,
                               size_t storage_size,
                               esy_uart_rx_stream_callback_t callback,
                               void *user_data);

  /**
   * @brief Peek the next contiguous readable RX stream span.
   */
  esy_err_t (*rx_stream_peek)(esy_uart_t *uart, const uint8_t **data,
                              size_t *length);

  /**
   * @brief Consume bytes from the span returned by rx_stream_peek.
   */
  esy_err_t (*rx_stream_consume)(esy_uart_t *uart, size_t length);

  /**
   * @brief Stop continuous RX and detach caller-owned storage.
   */
  esy_err_t (*rx_stream_stop)(esy_uart_t *uart);

  /**
   * @brief Suspend UART (save/stop as needed).
   * @param[in] uart UART handle
   * @return ESY_OK on success; negative error code on failure
   */
  esy_err_t (*suspend)(esy_uart_t *uart);

  /**
   * @brief Resume UART from suspended state.
   * @param[in] uart UART handle
   * @return ESY_OK on success; negative error code on failure
   */
  esy_err_t (*resume)(esy_uart_t *uart);

  /**
   * @brief Check whether data is readable (RX FIFO/buffer not empty).
   * @param[in] uart UART handle
   * @retval true  Readable
   * @retval false Not readable / no data available
   */
  bool (*is_readable)(const esy_uart_t *uart);

  /**
   * @brief Check whether TX or RX is busy.
   * @param[in] uart  UART handle
   * @param[in] is_tx true to query TX busy; false to query RX busy
   * @retval true  Busy (transfer in progress)
   * @retval false Idle
   */
  bool (*is_busy)(esy_uart_t *uart, bool is_tx);

  /**
   * @brief Set baud rate at runtime.
   * @param[in] uart     UART handle
   * @param[in] baudrate Target baud rate (bit/s)
   * @return ESY_OK on success; negative error code on failure
   */
  esy_err_t (*set_baudrate)(esy_uart_t *uart, uint32_t baudrate);

  /**
   * @brief Get hardware capability flags.
   * @param[in]  uart     UART handle
   * @param[out] out_caps Capability bitmask
   * @return ESY_OK on success; negative error code on failure (or
   * ESY_ERR_NOT_SUPPORTED)
   */
  esy_err_t (*get_caps)(const esy_uart_t *uart, uint32_t *out_caps);

  /**
   * @brief Get physical controller index.
   * @param[in]  uart      UART handle
   * @param[out] out_index Physical controller index
   * @return ESY_OK on success; negative error code on failure (or
   * ESY_ERR_NOT_SUPPORTED)
   */
  esy_err_t (*get_phys_index)(const esy_uart_t *uart, uint8_t *out_index);
  /** @brief Required task-owned terminal retirement; callbacks may run here. */
  esy_err_t (*poll)(esy_uart_t *uart);
  /** @brief Stop one DMA direction; success retires all its callbacks. */
  esy_err_t (*abort_dma)(esy_uart_t *uart, bool is_tx);
  /** @brief Read remaining DMA transfer units for optional directions. */
  esy_err_t (*get_dma_trans_count_remaining)(
      const esy_uart_t *uart, size_t *tx_remaining, size_t *rx_remaining);
} esy_uart_vtable_t;

/* -------------------------------------------------------------------------- */
/* Handle definition */
/* -------------------------------------------------------------------------- */

/** @brief Private HAL storage capacity; rebuild all consumers after layout changes. */
#define ESY_UART_HAL_STORAGE_BYTES (20u * sizeof(void *))

/**
 * @brief Caller-owned UART handle; initialize with {0} before first use.
 *
 * The HAL selects one shared immutable ops table. Callers must not replace it,
 * copy a live handle, inspect private state or clear storage to bypass cleanup.
 * ctx and backend remain the port's opaque storage contract; hal is owned only
 * by the HAL lifecycle and I/O implementation. Keep the handle and every borrowed
 * resource alive until successful deinit, including failed cleanup retries.
 */
typedef struct esy_uart_s {
  const esy_uart_vtable_t *ops; /**< Shared immutable HAL/provider dispatch. */
  void *ctx[2]; /**< Backend private context pointers (read-only; do not
                   interpret/modify) */
  union {
    void *alignment; /**< Natural pointer alignment for private HAL state. */
    uint8_t bytes[ESY_UART_HAL_STORAGE_BYTES]; /**< Do not inspect or modify. */
  } hal; /**< Opaque HAL-owned lifecycle state. */

  union {
    max_align_t _a;                     /**< Alignment guarantee */
    uint8_t bytes[ESY_UART_PRIV_BYTES]; /**< Backend private storage */
  } backend;
} esy_uart_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_UART_DEFS_H */
