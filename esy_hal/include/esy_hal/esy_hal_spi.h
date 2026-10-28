/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_spi.h
 * @brief HAL SPI API (handle-based).
 *
 * Design notes:
 * - The caller provides storage for the handle; the HAL wires
 * backend vtable/functions and dispatches calls.
 * - No "controller index/id" is used at call sites; all operations target an
 * @ref esy_spi_t* handle.
 * - Return convention:
 *   - Functions returning @ref esy_err_t: ESY_OK on success, negative error
 * code on failure.
 *   - Functions returning a count: >=0 indicates number of bytes/halfwords
 * transferred, negative indicates error code.
 *
 * Concurrency:
 * - If the same SPI instance can be accessed from multiple tasks/cores, the
 * caller must provide external synchronization.
 *
 * ESP32 buffer limits apply to both blocking and DMA-named transfer APIs:
 * - A transaction transfers at most 4094 bytes. Byte-count/halfword-count
 *   overflow or a larger transaction returns ESY_ERR_INVALID_ARG.
 * - RX up to four bytes uses the SDK's inline word and copies only the requested
 *   bytes. Longer RX requires a multiple of four bytes (maximum 4092), because
 *   native DMA writes complete words and callers grant no padding capacity.
 *   Other RX lengths return ESY_ERR_NOT_SUPPORTED before a native transaction
 *   starts or CS is asserted. Device-graph resume may precede backend checks.
 */

#ifndef ESY_HAL_SPI_H
#define ESY_HAL_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

/**
 * @note Handle lifecycle: initialize caller-owned handle storage with {0} before
 * its first init. Serialize lifecycle and data operations on one owner context.
 * Repeated init, including init while registered, returns ESY_ERR_STATE without
 * changing the live handle. Stop asynchronous work before unregister/deinit.
 * A physical controller is claimed before board/native changes and remains
 * claimed while suspended. Another handle returns BUSY without side effects.
 * Do not copy live handles. A rejected copy cannot operate or release its owner.
 * Failed init cleans up when possible. A cleanup error retains the claim and
 * handle storage: retry deinit before init or storage reuse. Only successful
 * deinit releases ownership. Native SDK lifecycle for the same controller must
 * not run outside this HAL, including while suspended.
 * Registered handles cannot be deinitialized directly. Successful deinit returns
 * an unregistered, quiescent handle to the zero state, ready for another init.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @note This initializer replaces every field. Apply caller overrides afterward.
 * @brief Fill an SPI configuration structure with platform defaults.
 *
 * The caller may override any field after this call.
 *
 * @param[out] cfg Output configuration pointer. Must not be NULL.
 */
extern void esy_hal_spi_default_cfg(esy_spi_config_t *cfg);

/**
 * @brief Initialize an SPI instance.
 *
 * This function wires the platform backend into @p spi and configures the
 * underlying hardware.
 *
 * @param[in,out] spi SPI handle (caller-provided storage).
 * @param[in]     cfg SPI configuration.
 * @retval ESY_OK Success.
 * @retval <0     Failure (e.g., ESY_ERR_INVALID_ARG / ESY_ERR_BUSY /
 * ESY_ERR_FAIL).
 */
extern esy_err_t esy_hal_spi_init(esy_spi_t *spi,
                                       const esy_spi_config_t *cfg);

/**
 * @brief Deinitialize an SPI instance.
 *
 * Retire native state, reset owned pins, then release the physical controller.
 * A registered SPI device adapter must be unregistered first. This task-only
 * call is serialized with I/O, poll and PM. Pending DMA (including completed
 * hardware awaiting poll/callback retirement) returns BUSY and retains the
 * handle, buffers and callbacks. Poll or abort successfully, then retry.
 * SDK cleanup errors retain the handle and claim; retry deinit without clearing
 * storage. A zero-state handle returns OK. XIO handles use their own deinit API.
 *
 * @param[in,out] spi SPI handle.
 * @retval ESY_OK Fully released; the handle is zero and can be initialized again.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_STATE Registered, copied or non-hardware handle.
 * @retval ESY_ERR_BUSY Pending native/DMA work; no resources released.
 * @retval <0 Native cleanup failure; resources remain owned for retry.
 */
extern esy_err_t esy_hal_spi_deinit(esy_spi_t *spi);

/* ========================= Blocking I/O (8-bit) ========================= */

/**
 * @brief Blocking write (8-bit frames).
 *
 * @param[in] spi  SPI handle.
 * @param[in] data Input buffer.
 * @param[in] len  Length in bytes.
 * @return >=0 Number of bytes written; <0 error code.
 */
extern int32_t esy_hal_spi_write_blocking(esy_spi_t *spi,
                                               const uint8_t *data, size_t len);

/**
 * @brief Blocking read (8-bit frames).
 *
 * @param[in]  spi  SPI handle.
 * @param[out] data Output buffer.
 * @param[in]  len  Length in bytes.
 * @return >=0 Number of bytes read; <0 error code.
 */
extern int32_t esy_hal_spi_read_blocking(esy_spi_t *spi, uint8_t *data,
                                              size_t len);

/**
 * @brief Blocking full-duplex transfer (8-bit frames).
 *
 * @param[in]  spi    SPI handle.
 * @param[in]  tx_buf TX buffer.
 * @param[out] rx_buf RX buffer.
 * @param[in]  len    Length in bytes.
 * @return >=0 Number of bytes transferred; <0 error code.
 */
extern int32_t esy_hal_spi_transfer_blocking(esy_spi_t *spi,
                                                  const uint8_t *tx_buf,
                                                  uint8_t *rx_buf, size_t len);

/**
 * @brief Blocking half-duplex write followed immediately by read.
 *
 * The transaction remains active across both phases. On a shared data line,
 * the backend releases the output direction between the final transmit
 * sampling edge and the following clock edge.
 *
 * @param[in] spi       SPI handle.
 * @param[in] tx_buf    Transmit buffer.
 * @param[in] tx_len    Transmit length in bytes.
 * @param[out] rx_buf   Receive buffer.
 * @param[in] rx_len    Receive length in bytes.
 * @retval ESY_OK Success.
 * @retval ESY_ERR_NOT_IMPLEMENTED Backend does not implement the operation.
 * @retval <0 Other failure.
 */
extern esy_err_t esy_hal_spi_write_then_read_blocking(
    esy_spi_t *spi, const uint8_t *tx_buf, size_t tx_len, uint8_t *rx_buf,
    size_t rx_len);

/* ========================= Blocking I/O (16-bit) ========================= */

/**
 * @brief Blocking write (16-bit frames).
 *
 * @param[in] spi  SPI handle.
 * @param[in] data Input buffer in halfwords.
 * @param[in] len  Length in halfwords.
 * @return >=0 Number of halfwords written; <0 error code.
 */
extern int32_t esy_hal_spi_write16_blocking(esy_spi_t *spi,
                                                 const uint16_t *data,
                                                 size_t len);

/**
 * @brief Blocking read (16-bit frames).
 *
 * @param[in]  spi  SPI handle.
 * @param[out] data Output buffer in halfwords.
 * @param[in]  len  Length in halfwords.
 * @return >=0 Number of halfwords read; <0 error code.
 */
extern int32_t esy_hal_spi_read16_blocking(esy_spi_t *spi, uint16_t *data,
                                                size_t len);

/**
 * @brief Blocking full-duplex transfer (16-bit frames).
 *
 * @param[in]  spi    SPI handle.
 * @param[in]  tx_buf TX buffer in halfwords.
 * @param[out] rx_buf RX buffer in halfwords.
 * @param[in]  len    Length in halfwords.
 * @return >=0 Number of halfwords transferred; <0 error code.
 */
extern int32_t esy_hal_spi_transfer16_blocking(esy_spi_t *spi,
                                                    const uint16_t *tx_buf,
                                                    uint16_t *rx_buf,
                                                    size_t len);

/* ========================= DMA (optional) ========================= */

/**
 * @brief DMA write (optional).
 *
 * Submits a DMA-named TX transaction using the provided DMA configuration.
 * Completion may be inline. The descriptor rules of
 * @ref esy_hal_spi_transfer_dma apply; the missing RX endpoint is discarded.
 *
 * @param[in] spi     SPI handle.
 * @param[in] dma_cfg DMA configuration.
 * @retval ESY_OK Success.
 * @retval <0     Failure (returns ESY_ERR_NOT_SUPPORTED if the platform does
 * not support DMA for SPI).
 */
extern esy_err_t esy_hal_spi_write_dma(esy_spi_t *spi,
                                            esy_dma_config_t *dma_cfg);

/**
 * @brief DMA read (optional).
 *
 * Submits a DMA-named RX transaction using the provided DMA configuration.
 * Completion may be inline. The descriptor rules of
 * @ref esy_hal_spi_transfer_dma apply; the missing TX endpoint sends dummy data.
 *
 * @param[in] spi     SPI handle.
 * @param[in] dma_cfg DMA configuration.
 * @retval ESY_OK Success.
 * @retval <0     Failure (returns ESY_ERR_NOT_SUPPORTED if the platform does
 * not support DMA for SPI).
 */
extern esy_err_t esy_hal_spi_read_dma(esy_spi_t *spi,
                                           esy_dma_config_t *dma_cfg);

/**
 * @brief DMA full-duplex transfer (optional).
 *
 * Submits a full-duplex SPI transfer through the backend's DMA-named API.
 * Completion may be inline; native DMA channels are backend-specific.
 *
 * @param[in] spi        SPI handle.
 * @param[in] tx_dma_cfg TX DMA configuration, or NULL for receive-only.
 * @param[in] rx_dma_cfg RX DMA configuration, or NULL for transmit-only.
 * @param[in] complete_cb Terminal transfer-result callback; may be NULL.
 * @param[in] user_data Opaque callback context.
 *
 * A started transfer reports exactly one terminal result through
 * @p complete_cb when provided. A successful callback result guarantees that
 * both DMA directions transferred their requested lengths. Built-in Pico/STM32
 * callers must drive @ref esy_hal_spi_poll from the serialized owner task;
 * terminal callbacks run there after DMA, pending IRQ access and wire activity
 * retire. Other backends may complete inline or in ISR context. All buffers,
 * handles and callback arguments remain borrowed through terminal return.
 * On failure, query esy_hal_spi_is_busy(): an idle rejection retains no buffers
 * and produces no later callback. If cleanup failed and the handle remains
 * busy, buffers, handle and callback context remain borrowed. Continue owner
 * polling for the terminal error, or successfully abort before reusing them.
 * Successful abort cancels the retained transfer without a terminal callback.
 * At least one configuration is required. A provided TX/RX descriptor requires
 * a non-NULL src/dst respectively, a nonzero byte length and an aligned buffer.
 * Lengths must contain complete 8- or 16-bit elements and must fit a bit count
 * in size_t. Two provided descriptors require equal byte lengths and element
 * widths. Circular mode and 32-bit elements are unsupported. Validation failures
 * reject before board/backend dispatch, callback retention or buffer access.
 * Pico XIO requires exactly 8-bit frames with 8-bit elements or 16-bit frames
 * with 16-bit elements, in either bit order. Each memory element contains one
 * complete frame value. Other XIO frame/element combinations reject with
 * NOT_SUPPORTED before DMA claims, FIFO changes or CS activity.
 * Built-in STM32F4/ESP32 require 8-bit elements for frames up to 8 bits and
 * 16-bit elements otherwise. ESP32 applies the file-level RX/transaction bounds
 * and completes synchronously in the caller; absent directions report zero
 * transferred bytes.
 * Single-direction helpers pass a NULL memory endpoint internally: Pico/STM32
 * use a fixed dummy/discard word, while ESP32 uses the SDK's missing-buffer mode.
 * STM32F4 also rejects routes sharing a physical stream.
 * @retval ESY_OK Success.
 * @retval <0     Failure (returns ESY_ERR_NOT_SUPPORTED if the platform does
 * not support DMA for SPI).
 */
extern esy_err_t esy_hal_spi_transfer_dma(
    esy_spi_t *spi, esy_dma_config_t *tx_dma_cfg, esy_dma_config_t *rx_dma_cfg,
    esy_spi_dma_complete_cb_t complete_cb, void *user_data);

/**
 * @brief Query current SPI DMA transfer remaining counts (optional).
 *
 * Remaining counts are reported in DMA transfer units (not bytes). For
 * single-direction DMA, the unused direction output may be NULL.
 *
 * @param[in]  spi                SPI handle.
 * @param[out] out_tx_remaining   Remaining TX transfer count (units), may be
 * NULL.
 * @param[out] out_rx_remaining   Remaining RX transfer count (units), may be
 * NULL.
 * @retval ESY_OK Success.
 * @retval <0     Failure (returns ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_spi_get_dma_trans_count_remaining(
    const esy_spi_t *spi, size_t *out_tx_remaining, size_t *out_rx_remaining);

/* ========================= Runtime controls (optional)
 * ========================= */

/**
 * @brief Query whether the SPI instance is busy (optional).
 *
 * @param[in] spi SPI handle.
 * @retval true  A transaction is in progress.
 * @retval false The instance is idle.
 */
extern bool esy_hal_spi_is_busy(esy_spi_t *spi);

/** @brief Advance DMA completion/failed-transfer cleanup from the serialized owner task.
 * Built-in Pico/STM32 backends require periodic calls after DMA acceptance;
 * callbacks execute here after both directions and physical SPI have stopped.
 * Every initialized backend must supply the poll hook. Synchronous backends
 * may return OK immediately when no retirement work is pending.
 * Never call concurrently with SPI I/O/lifecycle or from an ISR.
 * @return OK, INVALID_ARG for NULL, STATE for a missing backend hook, or BUSY
 * while failed cleanup retains storage.
 */
extern esy_err_t esy_hal_spi_poll(esy_spi_t *spi);

/**
 * @brief Set SPI clock speed at runtime (optional).
 *
 * @param[in] spi      SPI handle.
 * @param[in] speed_hz Target clock frequency in Hz.
 * @retval ESY_OK Success.
 * @retval <0     Failure (returns ESY_ERR_NOT_SUPPORTED if runtime speed change
 * is not supported).
 */
extern esy_err_t esy_hal_spi_set_speed(esy_spi_t *spi, uint32_t speed_hz);

/**
 * @brief Abort the current transaction (optional).
 *
 * Call from the serialized owner task. On STM32F4, success retires both DMA
 * directions, callbacks and wire activity, releases CS and cancels without a
 * terminal callback. Failure retains borrowed storage; continue polling or
 * retry abort before reuse or deinit. Unsupported ports return NOT_SUPPORTED.
 *
 * @param[in] spi SPI handle.
 * @retval ESY_OK Success.
 * @retval <0     Failure (returns ESY_ERR_NOT_SUPPORTED if abort is not
 * supported).
 */
extern esy_err_t esy_hal_spi_abort(esy_spi_t *spi);

/* ========================= Introspection (optional) =========================
 */

/**
 * @brief Query public SPI capabilities for an initialized handle.
 *
 * Uses @ref esy_spi_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  spi      SPI handle.
 * @param[out] out_caps Capability bitmask.
 * @retval ESY_OK Success.
 * @retval <0     Failure (returns ESY_ERR_NOT_SUPPORTED if not supported).
 */
extern esy_err_t esy_hal_spi_get_caps(const esy_spi_t *spi,
                                           uint32_t *out_caps);

/**
 * @brief Query the physical controller index (optional).
 *
 * @param[in]  spi       SPI handle.
 * @param[out] out_index Physical controller index.
 * @retval ESY_OK Success.
 * @retval <0     Failure (returns ESY_ERR_NOT_SUPPORTED if not supported).
 */
extern esy_err_t esy_hal_spi_get_phys_index(const esy_spi_t *spi,
                                                 uint8_t *out_index);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_SPI_H */
