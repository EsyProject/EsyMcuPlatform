/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_XIO_SPI_H
#define ESY_HAL_XIO_SPI_H

#include <stdint.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_spi_defs.h" /* esy_xio_spi_config_t */
#include "esy_common/defs/peripheral/esy_spi_defs.h" /* esy_spi_t / esy_spi_config_t */

/**
 * @note Initialize handle storage with {0}; repeated init returns ESY_ERR_STATE.
 * Serialize all operations on one owner task/core. DMA descriptors are copied;
 * buffers remain borrowed until terminal callback from esy_hal_spi_poll()
 * or successful esy_hal_spi_abort(). Poll must be driven by the owner;
 * BUSY retains ownership. Abort suppresses the callback. Deinit refuses a
 * registered or DMA-active handle; unregister/retire before deinitialization.
 * Successful deinit returns zero storage. Failed init permits retry.
 * On Pico, selected state machines are reserved in the SDK claim table. The
 * built-in Wi-Fi provider shares a nonblocking assembly guard with XIO; claim
 * contention returns BUSY or is retried by that provider. Direct external SDK
 * allocators must be serialized with XIO init/deinit and PM resource reassembly
 * because they do not participate in that guard. The SDK fixed-SM claim has no
 * recoverable atomic try-claim operation.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_hal_xio_spi.h
 * @brief HAL assembly API for SPI over XIO/PIO (no dynamic memory, vtable
 * compatible).
 *
 * Design goals:
 * - Provide only "default config" and "init/deinit" as the public assembly API.
 * - Backend selection (PIO/FlexIO/RMT/Stub) is decided by build system (CMake
 * source selection).
 * - No dynamic memory allocation is allowed.
 * - SPI is a single peripheral (not split TX/RX). A single @ref
 * esy_xio_spi_config_t describes SCK/MOSI/MISO/CS and protocol parameters.
 *
 * Ownership & lifetime:
 * - The caller owns @p spi storage.
 * - The implementation constructs backend context inside @c spi->backend
 * (private storage) and wires the vtable accordingly.
 *
 * Threading:
 * - Unless the backend explicitly states otherwise, this assembly layer does
 * not provide synchronization. Upper layers must ensure serialized access if
 * required.
 */

/**
 * @brief Fill default configuration for SPI over XIO.
 *
 * Default policy:
 * - pin_sck/pin_mosi/pin_miso/pin_cs = 0xFF (unset)
 * - role = ESY_SPI_ROLE_MASTER, mode = ESY_SPI_MODE0
 * - is_msb_first = true, data_bits = 8, speed_hz = 1 MHz
 * - instance / sm_index = 0xFF (caller must explicitly select)
 * - slave_cs_sm_index = 0xFF (select a second SM for slave MISO)
 * - cs_active_low = true; all other CS timing (ns) set to 0
 *
 * @param[out] cfg Output configuration pointer; must not be NULL.
 */
void esy_hal_xio_spi_default_cfg(esy_xio_spi_config_t *cfg);

/**
 * @brief Initialize and assemble SPI to the XIO backend (no dynamic memory).
 *
 * This function wires @p spi to the selected XIO backend implementation, builds
 * the backend context in @c spi->backend, and fills the SPI vtable so that
 * generic SPI APIs operate on XIO.
 *
 * @param[in,out] spi SPI handle (caller-provided storage). The implementation
 * constructs backend context in @c spi->backend and updates the vtable.
 * @param[in]     cfg XIO SPI configuration (pins/instance/SM/mode/bit
 * order/data bits/speed, etc.).
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments (e.g., required pins not set,
 * instance/SM not specified).
 * @retval ESY_ERR_NOT_SUPPORTED Slave mode without a physical CS input.
 * @retval ESY_ERR_NO_MEM      Insufficient PIO instruction memory.
 * @retval ESY_ERR_BUSY        Hardware resource is busy/unavailable.
 * @retval ESY_ERR_FAIL        Other failures.
 *
 * @note
 * - CPHA is typically implemented by selecting different PIO programs.
 * - CPOL can be implemented by inverting SCK via GPIO output override (platform
 * dependent).
 * - 8/16-bit frame sizes are supported by default; other widths can be extended
 * in the backend.
 * - Matching valid pin_mosi and pin_miso values select three-wire master mode
 * on the shared SDIO pin. Blocking read and write operations switch SDIO
 * direction; full-duplex and DMA transfers are unsupported.
 * - Slave mode requires a physical CS input; pin_cs=ESY_PIN_UNUSED returns
 * NOT_SUPPORTED before board pin configuration or program/SM allocation. Master
 * mode may use external CS ownership.
 * - Pico slave MISO requires slave_cs_sm_index in 0..3, distinct from sm_index
 * in the same instance. An invalid selection returns INVALID_ARG before board
 * configuration. The monitor consumes two instruction words and runs at
 * clk_sys, independently of data FIFO/SCK stalls. With CS synchronized, allow
 * six clk_sys cycles plus pad propagation for MISO high impedance after CS
 * deasserts and for activation before the first clock; qualify this on hardware.
 * Master and MISO-less slave configurations do not claim a monitor.
 * - Slave blocking transfers retire one RX FIFO word per transmitted frame,
 * including TX-only calls. Completion survives owner preemption followed by
 * CS deassertion; the master must still clock every requested frame.
 * Finite slave DMA uses its completed RX count, including dummy RX for TX-only,
 * without waiting for another CS/TXSTALL. Master DMA still waits for wire drain.
 */
esy_err_t esy_hal_xio_spi_init(esy_spi_t *spi, esy_xio_spi_config_t *cfg);

/**
 * @brief Deinitialize and release XIO SPI resources (idempotent).
 *
 * This function releases backend-specific hardware resources and clears the
 * assembly state. Calling it multiple times is safe.
 *
 * @param[in,out] spi SPI handle.
 */
void esy_hal_xio_spi_deinit(esy_spi_t *spi);

/**
 * @brief Start an XIO SPI DMA transfer after completing board DMA metadata.
 *
 * This convenience API mirrors the generic SPI DMA entry while keeping the
 * XIO-specific board metadata completion inside the HAL. Pico XIO accepts only
 * 8-bit frame/8-bit DMA and 16-bit frame/16-bit DMA combinations, in either bit
 * order. Other frame/element combinations return NOT_SUPPORTED before DMA
 * claims, FIFO changes or CS activity. The generic SPI descriptor validation,
 * buffer lifetime and poll/abort completion contract applies.
 *
 * @param[in,out] spi SPI handle initialized by @ref esy_hal_xio_spi_init.
 * @param[in,out] tx_dma_cfg TX DMA config; may be NULL for RX-only transfer.
 * @param[in,out] rx_dma_cfg RX DMA config; may be NULL for TX-only transfer.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid handle or missing both DMA configs.
 * @retval ESY_ERR_NOT_SUPPORTED DMA operation is not bound.
 * @retval <0 HAL backend error.
 */
esy_err_t esy_hal_xio_spi_transfer_dma(
    esy_spi_t *spi, esy_dma_config_t *tx_dma_cfg, esy_dma_config_t *rx_dma_cfg,
    esy_spi_dma_complete_cb_t complete_cb, void *user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HAL_XIO_SPI_H */
