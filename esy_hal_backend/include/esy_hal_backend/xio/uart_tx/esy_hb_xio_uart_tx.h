/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_XIO_UART_TX_H
#define ESY_HB_XIO_UART_TX_H

/**
 * @file esy_hb_xio_uart_tx.h
 * @brief XIO UART TX (8N1) HAL backend provider with no dynamic allocation.
 *
 * Notes:
 * - TX-only implementation. Frame format is fixed to **8N1**
 *   (8 data bits, no parity, 1 stop bit).
 * - No heap allocation is allowed during initialization or runtime.
 *   Backend state is stored in the caller-provided @ref esy_uart_t handle.
 * - RX-only or full-duplex UART functionality is provided by other modules.
 *
 * Conventions:
 * - All function parameters are annotated using
 *   `@param[in] / @param[out] / @param[in,out]`.
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
 * @brief Initialize an XIO-backed UART TX.
 *
 * Binds the specified XIO instance and state machine (SM), configures
 * the TX pin and baudrate, and initializes backend resources.
 * No dynamic memory allocation is performed.
 * Initialize the complete handle with {0} before its first direction init.
 * TX and RX may then own separate slots of the same handle. One task/core owner
 * serializes init/deinit, I/O and polling for both directions. Never copy or
 * clear live storage or callbacks. A repeated TX init returns STATE unchanged.
 * A failed first TX init retains no program/SM and may be retried. The pin must
 * exist on the selected chip and lie in GPIO 0..31.
 * Pico requires baudrate in 1..UINT32_MAX/8 and the exact ratio
 * clk_sys_hz/(8*baudrate) in [1, 65536]. The divider rounds down to 1/256;
 * unrepresentable rates return INVALID_ARG without retaining a program/SM.
 * An unavailable system clock returns STATE before resource allocation.
 *
 * @param[in,out] uart  UART facade/handle (caller-provided storage;
 *                      private backend context is written by init)
 * @param[in]     cfg   TX configuration (must explicitly specify
 *                      instance/sm_index, tx_pin, and baudrate)
 * @retval ESY_OK  Success
 * @retval ESY_ERR_STATE TX is already initialized; resources are unchanged.
 * @retval <0      Error code (e.g. ESY_ERR_INVALID_ARG / ESY_ERR_BUSY /
 *                 ESY_ERR_NO_MEM / ESY_ERR_STATE /
 *                 ESY_ERR_NOT_SUPPORTED)
 */
esy_err_t esy_hb_xio_uart_tx_init(esy_uart_t *uart,
                                     const esy_xio_uart_config_t *cfg);

/**
 * @brief Blocking transmit of a byte stream.
 *
 * Waits for FIFO space and then for physical drain, including the last stop
 * bit. A success permits releasing the source and suspending this direction.
 * One task/core serializes this call with DMA, poll, busy checks and lifecycle;
 * it is not ISR-safe and has no hard execution-time bound. Keep the PIO clock
 * and state machine enabled. An already disabled SM returns STATE before send.
 *
 * @param[in] uart  UART handle
 * @param[in] data  Transmit buffer
 * @param[in] len   Number of bytes to send
 * @return >=0 Number of bytes actually transmitted;
 *         <0  Error code (negative @ref esy_err_t)
 */
int32_t esy_hb_xio_uart_tx_write_blocking(esy_uart_t *uart,
                                             const uint8_t *data,
                                             size_t len);

/**
 * @brief Check retained DMA or physical TX work from the serialized owner.
 *
 * A false result proves that accepted TX has passed its final stop-bit delay.
 * The Pico provider first discards stale TXSTALL after DMA retirement and FIFO
 * empty, then requires a fresh stall at the program's blocking PULL. A first
 * check may conservatively return true. The check is bounded and nonblocking;
 * IRQ callbacks must not invoke it or change the SM/FDEBUG state. Never disable
 * the SM or change its clock while work is pending. Missing TX storage is idle.
 *
 * @param[in,out] uart TX handle; NULL or an unconfigured direction is idle.
 * @return true while DMA/callback or physical TX is pending, false after drain.
 */
bool esy_hb_xio_uart_tx_is_busy(esy_uart_t *uart);

/**
 * @brief Start a DMA-based transmit (non-blocking start).
 *
 * The DMA configuration (channel, DREQ, source/destination, callbacks, etc.)
 * must be provided by the caller. This function does not allocate memory.
 *
 * @param[in] uart     UART handle
 * @param[in] dma_cfg  DMA configuration
 * @retval ESY_OK  Successfully started
 * @retval <0      Error code
 */
esy_err_t esy_hb_xio_uart_tx_write_dma(esy_uart_t *uart,
                                          esy_dma_config_t *dma_cfg);

/**
 * @brief Get the currently configured TX pin number.
 *
 * @param[in] uart  UART handle
 * @return Pin number (platform-defined). If unavailable or not configured,
 *         may return @ref ESY_PIN_UNUSED or another implementation-defined value.
 */
uint8_t esy_hb_xio_uart_tx_get_pin(esy_uart_t *uart);

/**
 * @brief Deinitialize UART TX and release backend resources.
 *
 * The storage for @p uart itself is not freed.
 * Stop producers and finish callback retirement before this task-owner call.
 * Active DMA or undrained physical TX leaves the direction unchanged: complete
 * owner polling or abort, then keep checking is_busy until false before retrying.
 * Abort alone does not discard queued bytes. Otherwise cleanup clears the TX slot,
 * preserves RX, and permits TX reinitialization. An unused TX slot is a no-op.
 *
 * @param[in,out] uart  UART handle (private backend context is cleared/reset)
 */
void esy_hb_xio_uart_tx_deinit(esy_uart_t *uart);

/** @brief Task-owner cancellation; success retires native DMA and callbacks.
 * Failure retains caller storage for retry. Queued PIO bytes may still transmit.
 */
esy_err_t esy_hb_xio_uart_tx_abort_dma(esy_uart_t *uart);

/** @brief Retire published terminal IRQ events before notifying the owner.
 * One direction is checked per call; SDK abort and callback time contribute.
 * Never call from ISR or callbacks. Unconfigured directions are harmless.
 */
esy_err_t esy_hb_xio_uart_tx_poll(esy_uart_t *uart);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_XIO_UART_TX_H */
