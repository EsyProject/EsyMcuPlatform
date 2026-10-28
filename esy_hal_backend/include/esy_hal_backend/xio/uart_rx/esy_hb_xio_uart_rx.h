/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_XIO_UART_RX_H
#define ESY_HB_XIO_UART_RX_H

/**
 * @file esy_hb_xio_uart_rx.h
 * @brief XIO UART RX (8N1) HAL backend provider with no dynamic allocation.
 *
 * Notes:
 * - RX-only implementation. Frame format is fixed to **8N1** (8 data bits, no parity, 1 stop bit).
 * - No heap allocation; the caller provides @ref esy_uart_t storage and its backend context space.
 * - XIO instance/SM, RX pin, and baudrate are provided via @ref esy_xio_uart_config_t.
 *
 * Conventions:
 * - This project uses `@param[in] / @param[out] / @param[in,out]` annotations consistently.
 */

#include <stdint.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"          /* esy_dma_config_t */
#include "esy_common/defs/peripheral/xio/esy_xio_uart_defs.h" /* esy_xio_uart_config_t */
#include "esy_common/defs/peripheral/esy_uart_defs.h"         /* esy_uart_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize an XIO-backed UART RX.
 *
 * Binds the selected XIO instance and state machine (SM), configures the RX pin and baudrate,
 * and brings up the backend resources. No dynamic memory allocation is performed.
 * Initialize the complete handle with {0} before its first direction init.
 * RX and TX may then own separate slots of the same handle. One task/core owner
 * serializes init/deinit, I/O and polling for both directions. Never copy or
 * clear live storage or callbacks. A repeated RX init returns STATE unchanged.
 * A failed first RX init retains no program/SM and may be retried. The pin must
 * exist on the selected chip and lie in GPIO 0..31.
 * Pico requires baudrate in 1..UINT32_MAX/8 and the exact ratio
 * clk_sys_hz/(8*baudrate) in [1, 65536]. The divider rounds down to 1/256;
 * unrepresentable rates return INVALID_ARG without retaining a program/SM.
 * An unavailable system clock returns STATE before resource allocation.
 *
 * @param[in,out] uart  UART handle (caller-provided storage; private backend context is written by init)
 * @param[in]     cfg   RX configuration (must specify instance/sm_index, pin, baudrate)
 * @retval ESY_OK  Success
 * @retval ESY_ERR_STATE RX is already initialized; resources are unchanged.
 * @retval <0      Error code (e.g. ESY_ERR_INVALID_ARG / ESY_ERR_BUSY / ESY_ERR_STATE /
 *                 ESY_ERR_NOT_SUPPORTED / ESY_ERR_NOT_IMPLEMENTED)
 */
esy_err_t esy_hb_xio_uart_rx_init(esy_uart_t *uart,
                                     const esy_xio_uart_config_t *cfg);

/**
 * @brief Blocking receive of a byte stream.
 *
 * Blocks while the internal RX FIFO has no data, until @p len bytes are received or an error occurs.
 *
 * @param[in]  uart  UART handle
 * @param[out] buf   Receive buffer
 * @param[in]  len   Number of bytes requested
 * @return >=0 Number of bytes actually received; <0 error code (negative @ref esy_err_t)
 */
int32_t esy_hb_xio_uart_rx_read_blocking(esy_uart_t *uart,
                                            uint8_t *buf,
                                            size_t len);

/**
 * @brief Start a DMA-based receive (non-blocking start).
 *
 * The caller provides the DMA configuration (channel, src/dst, length, callbacks, etc.).
 * This function does not allocate memory.
 *
 * @param[in] uart     UART handle
 * @param[in] dma_cfg  DMA configuration (board mapping typically fills dreq/channel/irq/priority, etc.)
 * @retval ESY_OK  Successfully started
 * @retval <0      Error code
 */
esy_err_t esy_hb_xio_uart_rx_read_dma(esy_uart_t *uart,
                                         esy_dma_config_t *dma_cfg);

/**
 * @brief Check whether RX data is available (RX FIFO not empty).
 *
 * @param[in] uart  UART handle
 * @retval true   Data is readable
 * @retval false  No data available
 */
bool esy_hb_xio_uart_rx_is_readable(const esy_uart_t *uart);

/**
 * @brief Get the currently configured RX pin number.
 *
 * @param[in] uart  UART handle
 * @return Pin number (platform-defined). If unavailable/unconfigured, may return
 *         @ref ESY_PIN_UNUSED or another implementation-defined sentinel.
 */
uint8_t esy_hb_xio_uart_rx_get_pin(esy_uart_t *uart);

/**
 * @brief Deinitialize UART RX and release backend resources (does not free @p uart storage).
 *
 * @param[in,out] uart  UART handle (private backend context is cleared/reset)
 * @note Stop producers and finish callback retirement before this task-owner
 * call. Active DMA leaves the direction unchanged: complete owner polling or
 * abort successfully before retrying. Otherwise cleanup clears only the RX
 * slot, preserves TX, and permits RX reinitialization. An unused RX is a no-op.
 */
void esy_hb_xio_uart_rx_deinit(esy_uart_t *uart);

/** @brief Task-owner cancellation; success retires native DMA and callbacks.
 * Failure retains caller storage for retry. Received PIO bytes remain buffered.
 */
esy_err_t esy_hb_xio_uart_rx_abort_dma(esy_uart_t *uart);

/** @brief Retire published terminal IRQ events before notifying the owner.
 * One direction is checked per call; SDK abort and callback time contribute.
 * Never call from ISR or callbacks. Unconfigured directions are harmless.
 */
esy_err_t esy_hb_xio_uart_rx_poll(esy_uart_t *uart);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_XIO_UART_RX_H */
