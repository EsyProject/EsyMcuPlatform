/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_XIO_SPI_H
#define ESY_HB_XIO_SPI_H

/**
 * @file esy_hb_xio_spi.h
 * @brief XIO SPI HAL backend provider (PIO-based SPI implementation, compatible with
 * @ref esy_spi_vtable_t).
 *
 * Notes:
 * - No dynamic memory allocation; the caller allocates the handle and any
 * context storage.
 * - Function prototypes match @ref esy_spi_vtable_t exactly, so they can be
 * plugged into the generic SPI facade without adapters.
 * - Initialization uses @ref esy_xio_spi_config_t (XIO-specific:
 * instance/SM/pins/speed/etc.).
 * - Initialize the complete caller-owned handle with {0} before first use.
 *   One task/core owner serializes lifecycle, I/O and polling. Never copy or
 *   clear live storage, including while DMA or callbacks retain it.
 */

#include <stdint.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h" /* esy_spi_t / esy_spi_config_t / mode enums */
#include "esy_common/defs/peripheral/esy_dma_defs.h"         /* esy_dma_config_t */
#include "esy_common/defs/peripheral/xio/esy_xio_spi_defs.h" /* esy_xio_spi_config_t */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Vtable-compatible API (must be implemented) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize a PIO/XIO-backed SPI instance.
 *
 * Uses the XIO binding (instance/SM), pin mapping, and SPI parameters
 * (mode/bitrate/bit order/word size) from @p cfg to set up the backend. No heap
 * allocation is performed. Matching valid pin_mosi and pin_miso values select
 * three-wire master mode on the shared SDIO pin. Write-only blocking transfers
 * remain on the PIO state machine; atomic write-then-read transfers use SIO for
 * the direction turnaround. The Pico blocking APIs support 2-bit to 16-bit
 * frames. Frame values occupy the least-significant data_bits of each byte or
 * half-word buffer element. A trigger pin cannot be combined with three-wire
 * mode because its half-duplex turnaround path is not trigger-aware.
 * Pico pin assignments must name GPIOs present on the chip in the range 0..31,
 * or ESY_PIN_UNUSED where the signal is optional. Slave mode requires a
 * physical CS input; missing CS returns NOT_SUPPORTED before program/SM or
 * GPIO changes.
 * Slave MISO also requires a distinct slave_cs_sm_index in 0..3, in the same
 * instance; invalid/missing selection returns INVALID_ARG before native changes.
 * A separate two-instruction CS monitor controls only MISO output enable at
 * clk_sys while the data SM controls only its value. CS uses the input
 * synchronizer: allow six clk_sys cycles plus pad propagation for deselection
 * to high impedance and selection to enable; verify the timing on hardware.
 * The monitor owns its explicitly selected SM until failed-init cleanup,
 * suspend or deinit. Master and MISO-less slave modes claim no extra SM.
 * Repeated init returns STATE without changing live resources. A failed first
 * init releases its temporary program/SM and leaves the handle unused for retry.
 *
 * @param[in,out] spi  SPI handle (caller-provided storage)
 * @param[in]     cfg  XIO SPI configuration (@ref esy_xio_spi_config_t)
 * @retval ESY_OK  Success
 * @retval ESY_ERR_STATE The handle already owns a backend; unchanged.
 * @retval ESY_ERR_INVALID_ARG Invalid pointers, pins or configuration.
 * @retval ESY_ERR_NOT_SUPPORTED Trigger pin combined with three-wire mode, or
 * slave mode without a physical CS input.
 * @retval <0      Error code (e.g. ESY_ERR_INVALID_ARG / ESY_ERR_BUSY /
 * ESY_ERR_STATE / ESY_ERR_NOT_SUPPORTED / ESY_ERR_NOT_IMPLEMENTED)
 */
esy_err_t esy_hb_xio_spi_init(esy_spi_t *spi,
                                  const esy_xio_spi_config_t *cfg);

/**
 * @brief Deinitialize SPI and release backend resources (idempotent).
 *
 * @param[in,out] spi SPI handle
 * @note Stop producers and retire callbacks before this task-owner operation.
 * Active DMA leaves the handle and resources unchanged; finish poll or abort
 * successfully before retrying. Completed cleanup clears ctx and permits init
 * again without resetting the handle. No other backend may use it concurrently.
 */
void esy_hb_xio_spi_deinit(esy_spi_t *spi);

/**
 * @brief Blocking transmit using byte-backed frames.
 *
 * @param[in] spi     SPI handle
 * @param[in] tx_buf  Transmit buffer; each element contains one 2-bit to 8-bit
 *                    frame in its least-significant bits
 * @param[in] len     Number of frames to send
 * @return >=0 Number of frames sent; <0 error code
 * @note Three-wire master writes remain on the configured PIO state machine
 *       between calls and drive the shared SDIO pin as an output.
 */
int32_t esy_hb_xio_spi_write_blocking(esy_spi_t *spi, const uint8_t *tx_buf,
                                          size_t len);

/**
 * @brief Blocking receive using byte-backed frames.
 *
 * A fill frame is transmitted internally to generate SCK.
 *
 * @param[in]  spi    SPI handle
 * @param[out] rx_buf Receive buffer; each element receives one 2-bit to 8-bit
 *                    frame in its least-significant bits
 * @param[in]  len    Number of frames to receive
 * @return >=0 Number of frames received; <0 error code
 */
int32_t esy_hb_xio_spi_read_blocking(esy_spi_t *spi, uint8_t *rx_buf,
                                         size_t len);

/**
 * @brief Blocking full-duplex or single-direction byte-backed frame transfer.
 *
 * @param[in]  spi    SPI handle
 * @param[in]  tx_buf Transmit buffer (may be NULL for read-only / half-duplex
 * semantics)
 * @param[out] rx_buf Receive buffer (may be NULL for write-only / half-duplex
 * semantics)
 * @param[in]  len    Transfer length in 2-bit to 8-bit frames
 * @return >=0 Number of frames transferred; <0 error code
 * @note Three-wire mode supports only one non-NULL data buffer per call.
 */
int32_t esy_hb_xio_spi_transfer_blocking(esy_spi_t *spi,
                                             const uint8_t *tx_buf,
                                             uint8_t *rx_buf, size_t len);

/**
 * @brief Blocking three-wire write followed immediately by read.
 *
 * The shared data pin is released after the final transmit sampling edge and
 * before the following clock edge. This operation supports 2-bit to 8-bit
 * frames in Mode 0. Each buffer element contains one frame in its
 * least-significant bits.
 *
 * @param[in] spi       SPI handle
 * @param[in] tx_buf    Transmit buffer
 * @param[in] tx_len    Number of frames to transmit
 * @param[out] rx_buf   Receive buffer
 * @param[in] rx_len    Number of frames to receive
 * @retval ESY_OK Success
 * @retval ESY_ERR_NOT_SUPPORTED Unsupported role, wiring, frame width, or mode
 * @retval <0 Error code
 */
esy_err_t esy_hb_xio_spi_write_then_read_blocking(esy_spi_t *spi,
                                                      const uint8_t *tx_buf,
                                                      size_t tx_len,
                                                      uint8_t *rx_buf,
                                                      size_t rx_len);

/**
 * @brief Blocking transmit using half-word-backed frames.
 *
 * @param[in] spi     SPI handle
 * @param[in] tx_buf  Transmit buffer; each element contains one 9-bit to 16-bit
 *                    frame in its least-significant bits
 * @param[in] len     Number of frames to send
 * @return >=0 Number of frames sent; <0 error code
 */
int32_t esy_hb_xio_spi_write16_blocking(esy_spi_t *spi,
                                            const uint16_t *tx_buf, size_t len);

/**
 * @brief Blocking receive using half-word-backed frames.
 *
 * @param[in]  spi    SPI handle
 * @param[out] rx_buf Receive buffer; each element receives one 9-bit to 16-bit
 *                    frame in its least-significant bits
 * @param[in]  len    Number of frames to receive
 * @return >=0 Number of frames received; <0 error code
 */
int32_t esy_hb_xio_spi_read16_blocking(esy_spi_t *spi, uint16_t *rx_buf,
                                           size_t len);

/**
 * @brief Blocking full-duplex or single-direction half-word-backed transfer.
 *
 * @param[in]  spi    SPI handle
 * @param[in]  tx_buf Transmit buffer (may be NULL for read-only); each element
 *                    contains one 9-bit to 16-bit frame in its
 * least-significant bits
 * @param[out] rx_buf Receive buffer (may be NULL for write-only); each element
 *                    receives one frame in its least-significant bits
 * @param[in]  len    Transfer length in frames
 * @return >=0 Number of frames transferred; <0 error code
 * @note Three-wire mode supports only one non-NULL data buffer per call.
 */
int32_t esy_hb_xio_spi_transfer16_blocking(esy_spi_t *spi,
                                               const uint16_t *tx_buf,
                                               uint16_t *rx_buf, size_t len);

/**
 * @brief Start a DMA-based full-duplex or half-duplex transfer.
 *
 * @param[in] spi            SPI handle
 * @param[in] tx_dma_config  TX DMA config (may be NULL for RX-only)
 * @param[in] rx_dma_config  RX DMA config (may be NULL for TX-only)
 * @retval ESY_OK  Successfully started
 * @retval <0      Error code (e.g. ESY_ERR_INVALID_ARG / ESY_ERR_BUSY /
 *                 ESY_ERR_NOT_SUPPORTED / ESY_ERR_NOT_IMPLEMENTED)
 *
 * @note Whether one-direction DMA is supported is implementation-defined. If
 * not supported, the caller must provide dummy/constant buffers as
 * placeholders.
 * @note Three-wire DMA is not supported. Pico accepts only 8-bit frames with
 * 8-bit DMA elements and 16-bit frames with 16-bit DMA elements, for either bit
 * order. Each memory element contains one numeric frame value. Other frame/width
 * combinations return NOT_SUPPORTED before DMA claims, FIFO drain or CS changes.
 */
esy_err_t esy_hb_xio_spi_transfer_dma(esy_spi_t *spi,
                                          esy_dma_config_t *tx_dma_config,
                                          esy_dma_config_t *rx_dma_config,
                                          esy_spi_dma_complete_cb_t complete_cb,
                                          void *user_data);

/* -------------------------------------------------------------------------- */
/* Convenience queries (optional; used by HAL for pinmux/diagnostics) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get the actual TX pin number (e.g. MOSI).
 *
 * @param[in] spi SPI handle
 * @return Pin number (platform-defined). If unavailable/unconfigured, may
 * return
 *         @ref ESY_PIN_UNUSED or another implementation-defined sentinel.
 */
uint8_t esy_hb_xio_spi_get_tx_pin(esy_spi_t *spi);

/**
 * @brief Get the actual RX pin number (e.g. MISO).
 *
 * @param[in] spi SPI handle
 * @return Pin number (platform-defined). If unavailable/unconfigured, may
 * return
 *         @ref ESY_PIN_UNUSED or another implementation-defined sentinel.
 */
uint8_t esy_hb_xio_spi_get_rx_pin(esy_spi_t *spi);

/**
 * @brief Get the actual SCK pin number.
 *
 * @param[in] spi SPI handle
 * @return Pin number (platform-defined). If unavailable/unconfigured, may
 * return
 *         @ref ESY_PIN_UNUSED or another implementation-defined sentinel.
 */
uint8_t esy_hb_xio_spi_get_pin_sck(esy_spi_t *spi);

/** @brief Owner-task poll; returns BUSY until both DMA channels and PIO finish.
 * Terminal callbacks run here after channel retirement, never in the DMA ISR.
 * Serialize with all operations on this handle and its DMA channels.
 */
esy_err_t esy_hb_xio_spi_poll(esy_spi_t *spi);
/** @brief Retire both channels without callback; BUSY/error retains ownership. */
esy_err_t esy_hb_xio_spi_abort(esy_spi_t *spi);
/** @brief Query whether DMA still owns the handle and borrowed buffers. */
bool esy_hb_xio_spi_is_busy(esy_spi_t *spi);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_XIO_SPI_H */
