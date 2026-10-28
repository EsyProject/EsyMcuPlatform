/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SPI_DEFS_H
#define ESY_SPI_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_spi_defs.h
 * @brief Generic SPI type definitions and backend vtable.
 *
 * This header defines a platform-agnostic SPI abstraction:
 * - a unified SPI handle type,
 * - configuration/enumerations,
 * - and a backend vtable (function table) bound by the HAL wrapper.
 *
 * The upper layers interact only with @ref esy_spi_t* and do not depend on
 * controller indices directly. Platform-specific details are stored in the
 * handle's backend storage and accessed through the vtable.
 */

/**
 * @brief Reserved backend private context size in the handle (bytes).
 *
 * HAL backend implementations may use this fixed-size storage to keep private
 * state and avoid dynamic memory allocation. Override globally when an embedded
 * backend needs more capacity; all HAL/backend/consumer translation units must
 * use the same value and be rebuilt together. Backend capacity assertions
 * validate the selected size.
 */
#ifndef ESY_SPI_PRIV_BYTES
#define ESY_SPI_PRIV_BYTES 256
#endif

/* -------------------------------------------------------------------------- */
/* Forward Declarations and Handle Types */
/* -------------------------------------------------------------------------- */

/**
 * @brief Forward declaration of the unified SPI handle.
 *
 * Kept consistent with the HAL backend definition so that callback types and
 * the vtable can reference @ref esy_spi_t without including backend headers.
 */
struct esy_spi_s;
struct esy_device_s;

/**
 * @brief SPI handle type.
 */
typedef struct esy_spi_s esy_spi_t;

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
  ESY_SPI_CAP_DMA_TRANSFER = 1u << 0, /**< transfer_dma entry point; may complete synchronously. */
  ESY_SPI_CAP_TRANSFER16 = 1u << 1, /**< Blocking uint16_t buffer entry points; not a wire byte-order guarantee. */
  ESY_SPI_CAP_DMA_REMAINING = 1u << 2, /**< DMA remaining-count query for board-provided routes. */
} esy_spi_cap_t;

/** @brief Normalized terminal state of an asynchronous SPI DMA transfer. */
typedef enum {
  ESY_SPI_DMA_TRANSFER_OK = 0,
  ESY_SPI_DMA_TRANSFER_SHORT,
  ESY_SPI_DMA_TRANSFER_DMA_ERROR,
  ESY_SPI_DMA_TRANSFER_BUS_ERROR,
  ESY_SPI_DMA_TRANSFER_ABORTED,
} esy_spi_dma_transfer_status_t;

/** @brief Result reported exactly once for a started SPI DMA transfer. */
typedef struct {
  esy_spi_dma_transfer_status_t status;
  size_t tx_transferred;
  size_t rx_transferred;
} esy_spi_dma_transfer_result_t;

/** @brief SPI DMA terminal-result callback. */
typedef void (*esy_spi_dma_complete_cb_t)(
    esy_spi_t *spi, const esy_spi_dma_transfer_result_t *result,
    void *user_data);

/** @brief Task-context terminal SPI result. RX bytes are valid only on ESY_OK.
 * The provider retains bus ownership during cleanup; context and buffers are
 * borrowed through callback return. A failed transfer may have partial effects.
 */
typedef void (*esy_spi_transfer_done_cb_t)(esy_err_t result, uint8_t *rx, void *arg);
/** @brief Queued byte transfer with explicit status; one of TX/RX must be non-NULL.
 * Producer calls are externally serialized. Success borrows buffers/context
 * through exactly one task-context completion; rejection retains nothing and
 * invokes no callbacks. Optional pre runs before I/O; done is required.
 */
typedef esy_err_t (*esy_spi_submit_result_t)(uint8_t *tx, uint8_t *rx, uint32_t len,
    void (*pre)(void *), void *pre_arg, esy_spi_transfer_done_cb_t done,
    void *done_arg, void *provider_arg);

/**
 * @brief Prepare a queued transfer while the provider exclusively owns the bus.
 *
 * Runs once in task context before the payload. May perform bounded blocking
 * SPI/GPIO operations on that bus. Return ESY_OK to send the payload, or an error
 * to skip it. Cleanup is performed by the completion callback in either case.
 * The callback must not enqueue onto, quiesce, or destroy its own provider.
 */
typedef esy_err_t (*esy_spi_prepare_cb_t)(void *arg);

/**
 * @brief Complete an accepted prepared transfer, including preparation failure.
 *
 * Runs exactly once in task context after payload I/O has stopped, while the
 * provider retains bus ownership until this callback returns (for CS cleanup).
 * ESY_OK means the entire payload was transferred; negative values report
 * preparation or transfer failure. The callback may enqueue another request,
 * but must not destroy the provider. The provider must not access the callback
 * argument or payload again after invoking this callback.
 */
typedef void (*esy_spi_prepared_done_cb_t)(esy_err_t result, void *arg);

/**
 * @brief Submit one indivisible prepare/payload/completion transaction.
 *
 * All arguments and TX storage are borrowed until completion starts. A provider
 * serializes preparation and payload against all other requests on the same
 * bus. ESY_OK accepts the complete transaction; a negative return rejects it
 * without invoking either callback or retaining any argument. Completion may
 * run inline before submit returns. Calls are producer-task-only and multiple
 * producers must be externally serialized. No callback runs in ISR context.
 * Providers must drain accepted transactions before destruction.
 *
 * @param[in] tx_buf Immutable payload, non-NULL for len > 0.
 * @param[in] len Payload size in bytes, must be nonzero.
 * @param[in] prepare Required bounded preparation callback.
 * @param[in] done Required terminal callback, including error completion.
 * @param[in] callback_arg Caller-owned context shared by both callbacks.
 * @param[in] provider_arg Provider instance, owned by the caller.
 * @retval ESY_OK Accepted; exactly one completion is owed.
 * @retval <0 Rejected; no callbacks or retained resources.
 */
typedef esy_err_t (*esy_spi_submit_prepared_t)(
    const uint8_t *tx_buf, uint32_t len, esy_spi_prepare_cb_t prepare,
    esy_spi_prepared_done_cb_t done, void *callback_arg, void *provider_arg);

/* -------------------------------------------------------------------------- */
/* Enumerations and Configuration */
/* -------------------------------------------------------------------------- */

/**
 * @brief SPI mode (CPOL/CPHA).
 */
typedef enum {
  ESY_SPI_MODE_0, /**< CPOL=0, CPHA=0 */
  ESY_SPI_MODE_1, /**< CPOL=0, CPHA=1 */
  ESY_SPI_MODE_2, /**< CPOL=1, CPHA=0 */
  ESY_SPI_MODE_3  /**< CPOL=1, CPHA=1 */
} esy_spi_mode_t;

/**
 * @brief SPI role.
 */
typedef enum {
  ESY_SPI_ROLE_MASTER, /**< Master mode (HAL backend may control CS automatically) */
  ESY_SPI_ROLE_MASTER_NO_CS, /**< Master mode without CS control (CS handled by
                                upper layer) */
  ESY_SPI_ROLE_SLAVE         /**< Slave mode */
} esy_spi_role_t;

/**
 * @brief SPI inner configuration (platform-specific).
 *
 * This structure carries platform-specific CS wiring information.
 * @ref ESY_PIN_UNUSED indicates that CS is not managed by the HAL backend.
 */
typedef struct {
  uint8_t cs_pin; /**< Chip-select pin; ESY_PIN_UNUSED means unused */
} esy_spi_inner_config_t;

/**
 * @brief SPI initialization configuration.
 *
 * @note
 * - @ref phys_index selects the physical SPI controller instance (e.g.,
 * spi0/spi1).
 * - When @ref role is @ref ESY_SPI_ROLE_MASTER_NO_CS, the HAL backend must not
 * toggle CS regardless of @ref esy_spi_inner_config_t::cs_pin. In that case, it
 * is recommended to set `inner.cs_pin = ESY_PIN_UNUSED` and let the upper layer
 * manage CS explicitly.
 */
typedef struct {
  uint8_t phys_index;  /**< Physical controller index */
  esy_spi_role_t role; /**< Master/slave role */
  esy_spi_mode_t mode; /**< SPI mode 0..3 */
  uint32_t speed_hz;   /**< SCK frequency in Hz (effective in master mode) */

  uint8_t data_bits; /**< Frame width in bits (commonly 8 or 16) */
  bool is_msb_first; /**< Bit order: true=MSB first, false=LSB first */

  esy_spi_inner_config_t inner; /**< Platform-specific inner configuration */
} esy_spi_config_t;

/* -------------------------------------------------------------------------- */
/* Backend Vtable */
/* -------------------------------------------------------------------------- */

/**
 * @brief SPI backend function table (vtable).
 *
 * All functions take @ref esy_spi_t* as the first argument.
 *
 * Return convention:
 * - Blocking transfer functions return a non-negative count (bytes or 16-bit
 * units), or a negative error code on failure.
 * - Control/config functions return @ref esy_err_t.
 * - Optional/unsupported features should return @ref ESY_ERR_NOT_SUPPORTED or
 *   @ref ESY_ERR_NOT_IMPLEMENTED.
 */
typedef struct {
  /**
   * @brief Blocking write (8-bit frames).
   *
   * @param[in] spi    SPI handle
   * @param[in] tx_buf Transmit buffer
   * @param[in] len    Number of bytes to write
   * @return Number of bytes written on success; negative error code on failure
   */
  int32_t (*write_blocking)(esy_spi_t *spi, const uint8_t *tx_buf, size_t len);

  /**
   * @brief Blocking read (8-bit frames).
   *
   * @param[in]  spi    SPI handle
   * @param[out] rx_buf Receive buffer
   * @param[in]  len    Number of bytes to read
   * @return Number of bytes read on success; negative error code on failure
   */
  int32_t (*read_blocking)(esy_spi_t *spi, uint8_t *rx_buf, size_t len);

  /**
   * @brief Blocking full-duplex transfer (8-bit frames).
   *
   * @param[in]  spi    SPI handle
   * @param[in]  tx_buf Transmit buffer
   * @param[out] rx_buf Receive buffer
   * @param[in]  len    Number of bytes to transfer
   * @return Number of bytes transferred on success; negative error code on
   * failure
   */
  int32_t (*transfer_blocking)(esy_spi_t *spi, const uint8_t *tx_buf,
                               uint8_t *rx_buf, size_t len);

  /**
   * @brief Blocking half-duplex write followed by read.
   *
   * Shared-data-line backends must release the output direction after the
   * final transmit sampling edge and before the following clock edge.
   *
   * @param[in] spi       SPI handle
   * @param[in] tx_buf    Transmit buffer
   * @param[in] tx_len    Number of bytes to write
   * @param[out] rx_buf   Receive buffer
   * @param[in] rx_len    Number of bytes to read
   * @retval ESY_OK Success
   * @retval <0 Error code
   */
  esy_err_t (*write_then_read_blocking)(esy_spi_t *spi, const uint8_t *tx_buf,
                                        size_t tx_len, uint8_t *rx_buf,
                                        size_t rx_len);

  /**
   * @brief Blocking write (16-bit frames).
   *
   * @param[in] spi    SPI handle
   * @param[in] tx_buf Transmit buffer (16-bit units)
   * @param[in] len    Number of 16-bit units to write
   * @return Number of units written on success; negative error code on failure
   */
  int32_t (*write16_blocking)(esy_spi_t *spi, const uint16_t *tx_buf,
                              size_t len);

  /**
   * @brief Blocking read (16-bit frames).
   *
   * @param[in]  spi    SPI handle
   * @param[out] rx_buf Receive buffer (16-bit units)
   * @param[in]  len    Number of 16-bit units to read
   * @return Number of units read on success; negative error code on failure
   */
  int32_t (*read16_blocking)(esy_spi_t *spi, uint16_t *rx_buf, size_t len);

  /**
   * @brief Blocking full-duplex transfer (16-bit frames).
   *
   * @param[in]  spi    SPI handle
   * @param[in]  tx_buf Transmit buffer (16-bit units)
   * @param[out] rx_buf Receive buffer (16-bit units)
   * @param[in]  len    Number of 16-bit units to transfer
   * @return Number of units transferred on success; negative error code on
   * failure
   */
  int32_t (*transfer16_blocking)(esy_spi_t *spi, const uint16_t *tx_buf,
                                 uint16_t *rx_buf, size_t len);

  /**
   * @brief Full-duplex transfer using DMA.
   *
   * Either direction may be NULL to indicate "no TX" or "no RX" depending on
   * backend support.
   *
   * @param[in] spi           SPI handle
   * @param[in] tx_dma_config TX DMA configuration (may be NULL)
   * @param[in] rx_dma_config RX DMA configuration (may be NULL)
   * @return Result code
   */
  esy_err_t (*transfer_dma)(esy_spi_t *spi, esy_dma_config_t *tx_dma_config,
                            esy_dma_config_t *rx_dma_config,
                            esy_spi_dma_complete_cb_t complete_cb,
                            void *user_data);

  /**
   * @brief Query whether the SPI peripheral is busy (TX/RX in progress).
   *
   * @param[in] spi SPI handle
   * @retval true  Busy
   * @retval false Idle
   */
  bool (*is_busy)(esy_spi_t *spi);

  /**
   * @brief Set SPI clock frequency (master mode).
   *
   * @param[in] spi      SPI handle
   * @param[in] speed_hz Target SCK frequency in Hz
   * @return Result code
   */
  esy_err_t (*set_speed)(esy_spi_t *spi, uint32_t speed_hz);

  /**
   * @brief Abort the current transfer.
   *
   * @param[in] spi SPI handle
   * @return Result code
   */
  esy_err_t (*abort)(esy_spi_t *spi);

  /* ---------------------------- Optional queries ---------------------------
   */

  /**
   * @brief Get hardware capability flags (optional).
   *
   * @param[in]  spi      SPI handle
   * @param[out] out_caps Capability bitmask defined by the platform
   * @return Result code
   */
  esy_err_t (*get_caps)(const esy_spi_t *spi, uint32_t *out_caps);

  /**
   * @brief Get physical controller index (optional; useful for debugging).
   *
   * @param[in]  spi       SPI handle
   * @param[out] out_index Physical controller index
   * @return Result code
   */
  esy_err_t (*get_phys_index)(const esy_spi_t *spi, uint8_t *out_index);

  /**
   * @brief Interrupt dispatch hook (optional).
   *
   * If the HAL backend needs IRQ forwarding from the ISR entry,
   * implement this hook and call it from the interrupt vector.
   *
   * @param[in] spi SPI handle
   */
  void (*handle_irq)(esy_spi_t *spi);
  /** @brief Required owner-task DMA retirement; may dispatch terminal callbacks.
   * Synchronous backends return OK when no retirement work is pending. */
  esy_err_t (*poll)(esy_spi_t *spi);
  /** @brief Optional remaining transfer-unit query; NULL means unsupported.
   * Units follow each direction's configured DMA element width, not bytes. */
  esy_err_t (*get_dma_trans_count_remaining)(const esy_spi_t *spi,
      size_t *out_tx_remaining, size_t *out_rx_remaining);
} esy_spi_vtable_t;

/* -------------------------------------------------------------------------- */
/* Handle Definition */
/* -------------------------------------------------------------------------- */

/** @brief Private HAL storage capacity; rebuild all consumers after layout changes. */
#define ESY_SPI_HAL_STORAGE_BYTES (2u * sizeof(void *))

/**
 * @brief Caller-owned SPI handle; initialize with {0} before first use.
 *
 * The HAL selects one shared immutable ops table. Callers must not replace it,
 * copy a live handle, inspect private state or clear storage to bypass cleanup.
 * ctx and backend remain the port's opaque storage contract; hal is owned only
 * by the HAL lifecycle and I/O implementation. Keep the handle and every borrowed
 * resource alive until successful deinit, including failed cleanup retries.
 */
typedef struct esy_spi_s {
  const esy_spi_vtable_t *ops; /**< Shared immutable HAL/provider dispatch. */
  void *ctx;                   /**< Backend private context pointer */
  union {
    void *alignment; /**< Natural pointer alignment for private HAL state. */
    uint8_t bytes[ESY_SPI_HAL_STORAGE_BYTES]; /**< Do not inspect or modify. */
  } hal; /**< Opaque HAL-owned lifecycle state. */

  union {
    max_align_t _a;                    /**< Alignment guarantee */
    uint8_t bytes[ESY_SPI_PRIV_BYTES]; /**< Backend private storage */
  } backend;
} esy_spi_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_SPI_DEFS_H */
