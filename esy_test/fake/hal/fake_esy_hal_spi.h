/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_hal_spi.h
 * @brief Fake HAL SPI contract for unit tests.
 *
 * DMA entries enforce the public HAL descriptor, endpoint, length, width and
 * alignment checks before applying injected provider results. Represent an
 * unused direction with a NULL descriptor, not a descriptor with NULL storage.
 */

#ifndef FAKE_ESY_HAL_SPI_H
#define FAKE_ESY_HAL_SPI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_spi_device.h"
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset all fake SPI instances.
 *
 * Clears recorded writes and preset read data.
 */
void fake_esy_hal_spi_reset(void);

/**
 * @brief Set the result returned by esy_hal_spi_init().
 */
void fake_esy_hal_spi_set_init_result(esy_err_t ret);

/**
 * @brief Preset 8-bit RX data for a SPI handle.
 *
 * Later read_blocking or transfer_blocking calls copy bytes from this buffer.
 *
 * @param spi   SPI handle.
 * @param data  Data buffer.
 * @param len   Data length in bytes.
 */
void fake_esy_hal_spi_set_rx_data(esy_spi_t *spi,
                                       const uint8_t *data,
                                       size_t len);

/**
 * @brief Get the length of the last 8-bit write for a SPI handle.
 */
size_t fake_esy_hal_spi_get_last_tx_len(esy_spi_t *spi);

/**
 * @brief Copy the last 8-bit write for a SPI handle.
 *
 * @param spi      SPI handle.
 * @param out_buf  Output buffer.
 * @param max_len  Maximum writable length of out_buf.
 * @return Number of bytes copied.
 */
size_t fake_esy_hal_spi_copy_last_tx(esy_spi_t *spi,
                                          uint8_t *out_buf,
                                          size_t max_len);

/**
 * @brief Get the number of blocking writes for a SPI handle.
 */
size_t fake_esy_hal_spi_get_tx_count(esy_spi_t *spi);

/**
 * @brief Copy the data from the indexed blocking write.
 *
 * @param spi      SPI handle.
 * @param index    Zero-based write index.
 * @param out_buf  Output buffer.
 * @param max_len  Maximum writable length of out_buf.
 * @return Number of bytes copied, or 0 for an invalid index.
 */
size_t fake_esy_hal_spi_copy_tx_at(esy_spi_t *spi,
                                        size_t index,
                                        uint8_t *out_buf,
                                        size_t max_len);

/**
 * @brief Set the blocking write result.
 *
 * The default returns the requested write length; negative values simulate a
 * SPI write failure.
 */
void fake_esy_hal_spi_set_write_blocking_result(int32_t ret);

/**
 * @brief Query whether a SPI handle has been initialized.
 */
bool fake_esy_hal_spi_is_initialized(esy_spi_t *spi);

/**
 * @brief Set remaining TX/RX DMA counts in transfer units, not bytes.
 * Changing remaining counts does not change the requested byte lengths reported
 * by a successful terminal transfer callback.
 */
void fake_esy_hal_spi_set_dma_remaining(esy_spi_t *spi,
                                             size_t tx_remaining,
                                             size_t rx_remaining);

/**
 * @brief Set the DMA write result.
 */
void fake_esy_hal_spi_set_write_dma_result(esy_err_t ret);

/**
 * @brief Set the DMA read result.
 */
void fake_esy_hal_spi_set_read_dma_result(esy_err_t ret);

/**
 * @brief Set the full-duplex DMA transfer result.
 */
void fake_esy_hal_spi_set_transfer_dma_result(esy_err_t ret);

/**
 * @brief Retain DMA buffers/callbacks when the configured submission fails.
 * Default false models an idle rejection. True models partial-start cleanup
 * failure: the handle stays busy until successful poll or abort. Existing
 * retained transfers reject further submissions without replacing their borrow.
 */
void fake_esy_hal_spi_set_failed_submit_retains_borrow(bool retain);

/**
 * @brief Set owner poll cleanup result for retained failed submissions.
 * ESY_OK retires the borrow and delivers one DMA_ERROR callback; an error
 * retains it. Default ESY_OK. An installed fake poll hook takes precedence.
 */
void fake_esy_hal_spi_set_poll_result(esy_err_t ret);

/**
 * @brief Set task abort result; ESY_OK cancels silently, errors retain the borrow.
 * Default ESY_OK. All controls and observations are test-thread serialized.
 */
void fake_esy_hal_spi_set_abort_result(esy_err_t ret);

/** @brief Count poll calls for the handle since reset; zero for unknown handles. */
size_t fake_esy_hal_spi_get_poll_count(esy_spi_t *spi);
/** @brief Count abort calls for the handle since reset; zero for unknown handles. */
size_t fake_esy_hal_spi_get_abort_count(esy_spi_t *spi);
/** @brief Observe borrowed TX storage until terminal retirement; NULL if none. */
const void *fake_esy_hal_spi_get_borrowed_tx(esy_spi_t *spi);
/** @brief Observe borrowed RX storage until terminal retirement; NULL if none. */
void *fake_esy_hal_spi_get_borrowed_rx(esy_spi_t *spi);

/**
 * @brief Configure whether transfer_dma completes asynchronously.
 *
 * In async mode, transfer_dma records the callback and remains busy until the
 * test calls fake_esy_hal_spi_complete_last_dma().
 */
void fake_esy_hal_spi_set_transfer_dma_async(bool async);

/**
 * @brief Complete the pending DMA with its requested byte lengths.
 * Successful completion does not derive transferred bytes from the remaining
 * counters. Retained submission failures report DMA_ERROR with unknown counts.
 */
void fake_esy_hal_spi_complete_last_dma(esy_spi_t *spi);

/**
 * @brief Retire pending DMA with an explicit status and completed byte counts.
 * Use this helper for partial/error terminal results instead of changing the
 * remaining transfer-unit counters. Releases the borrow before one callback.
 * @param spi Pending fake handle; an unknown/idle handle is ignored.
 * @param result Terminal result copied for this call; NULL is ignored.
 */
void fake_esy_hal_spi_complete_last_dma_with_result(
    esy_spi_t *spi, const esy_spi_dma_transfer_result_t *result);

/**
 * @brief Set the result returned by the DMA remaining-count query.
 */
void fake_esy_hal_spi_set_get_dma_remaining_result(esy_err_t ret);

/**
 * @brief Inject one write result after the specified number of calls.
 * @param successful_calls Calls to leave unchanged before injecting once.
 * @param result Injected error or byte count (SPI permits short transfers).
 * Reset cancels pending injections. Test-thread only; externally serialize.
 */
void fake_esy_hal_spi_fail_write_after(size_t successful_calls, int32_t result);

/**
 * @brief Inject one read result after the specified number of calls.
 * @param successful_calls Calls to leave unchanged before injecting once.
 * @param result Injected error or byte count (SPI permits short transfers).
 * Reset cancels pending injections. Test-thread only; externally serialize.
 */
void fake_esy_hal_spi_fail_read_after(size_t successful_calls, int32_t result);

/** @brief Supply a borrowed graph association in the fake provider; NULL clears it.
 * Installs the fake's immutable operation table without touching HAL private state. */
void fake_esy_hal_spi_set_device(esy_spi_t *spi, const esy_device_t *device);
/** @brief Replace owner poll behavior for this fake instance; NULL restores default. */
void fake_esy_hal_spi_set_poll_hook(esy_spi_t *spi, esy_err_t (*hook)(esy_spi_t *));
/** @brief Replace PM write behavior after public transition checks; NULL records writes. */
void fake_esy_hal_spi_set_pm_write_hook(esy_spi_t *spi,
    int32_t (*hook)(esy_spi_t *, const uint8_t *, size_t));

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_HAL_SPI_H */
