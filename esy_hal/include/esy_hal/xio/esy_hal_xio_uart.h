/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_XIO_UART_H
#define ESY_HAL_XIO_UART_H

#include <stdint.h>
#include <stddef.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"
#include "esy_common/defs/peripheral/xio/esy_xio_uart_defs.h"  /* esy_xio_uart_config_t */
#include "esy_common/defs/peripheral/esy_uart_defs.h"          /* esy_uart_t */

/**
 * @note DMA is finite-only on the built-in Pico XIO backend. IRQs publish
 * completion/error events; the serialized owner must keep calling
 * esy_hal_uart_poll() until terminal retirement or successful abort. Native
 * pending/peer callbacks retire before notifications and borrowed storage are
 * released. Failed cleanup keeps the handle/buffers borrowed for retry.
 * XIO terminal callbacks run in the polling task and must not reenter UART APIs.
 * Completion retires DMA source access; queued PIO bytes may still transmit.
 * TX busy remains true until physical drain, including the last stop bit.
 * Blocking TX waits for that drain before returning. Check busy repeatedly
 * after DMA completion/abort before deinit; a first wire check may conservatively
 * report busy while the provider discards an old FIFO-starvation indication.
 * Runtime PM skips undrained TX; system suspend returns BUSY. Keep the PIO clock
 * fixed and the state machine enabled until idle. All busy checks use the same
 * serialized task/core owner as I/O, poll, lifecycle and PM; they are not ISR APIs.
 * Circular ordinary DMA is rejected before starting. Half notifications are
 * not provided by the Pico generic DMA backend.
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
 * @file esy_hal_xio_uart.h
 * @brief HAL assembly API for UART over XIO (8N1, no dynamic memory).
 *
 * Design goals:
 * - Expose only minimal assembly APIs: default configuration and init/deinit.
 * - All backend-specific wiring (PIO/FlexIO/RMT/Stub) is implemented in the .c file
 *   and selected by the build system (CMake source selection).
 * - Dynamic memory allocation is strictly forbidden.
 *
 * Architecture notes:
 * - TX and RX are treated as independent XIO programs and may be enabled separately.
 * - Backend runtime context is constructed inside @c uart->backend (private storage).
 * - The public @ref esy_uart_t interface is wired via vtable during initialization.
 * - Data transfers use the generic UART API, including @ref esy_hal_uart_send_dma
 *   and @ref esy_hal_uart_receive_dma declared in esy_hal/esy_hal_uart.h.
 *
 * Protocol assumptions:
 * - Fixed UART framing: 8 data bits, no parity, 1 stop bit (8N1).
 * - Flow control is not handled at this layer.
 *
 * Concurrency:
 * - This assembly layer does not provide locking.
 * - Upper layers must serialize access if TX/RX are shared across contexts.
 */

/**
 * @brief Fill default configuration for UART over XIO.
 *
 * Default policy:
 * - pin                  = 0xFF (unset, must be explicitly configured)
 * - baudrate             = 115200
 * - framing              = fixed 8N1 (not configurable)
 * - instance             = 0xFF (XIO instance must be specified by caller)
 * - sm_index             = 0xFF (state machine index must be specified by caller)
 *
 * @param[out] cfg Output configuration pointer; must not be NULL.
 */
void esy_hal_xio_uart_default_cfg(esy_xio_uart_config_t *cfg);

/**
 * @brief Initialize and assemble UART (TX/RX) to the XIO backend.
 *
 * This function wires the generic @ref esy_uart_t handle to the XIO backend
 * implementation. Backend-specific runtime context is constructed in
 * @c uart->backend, and TX/RX paths are enabled according to @p tx_cfg
 * and @p rx_cfg.
 *
 * @param[in,out] uart   UART handle initialized with {0} before first use.
 * Repeated init returns ESY_ERR_STATE. Failure releases partial TX/RX resources;
 * retry is permitted. Deinit requires quiescent, unregistered storage and clears
 * it for reuse. Serialize operations, lifecycle and PM on one owner context.
 *                        The implementation initializes @c uart->backend
 *                        and installs the UART vtable.
 * @param[in]     tx_cfg TX configuration; may be NULL to disable TX.
 * @param[in]     rx_cfg RX configuration; may be NULL to disable RX.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid arguments (e.g., missing pins, instance/SM not specified).
 * @retval ESY_ERR_NO_MEM      Insufficient backend storage (uart->backend too small).
 * @retval ESY_ERR_BUSY        Hardware/XIO resources already in use.
 * @retval ESY_ERR_FAIL        Other platform-specific failures.
 *
 * @note
 * - TX-only or RX-only UART instances are allowed.
 * - If both TX and RX are enabled, they may share or use separate XIO state machines,
 *   depending on backend implementation.
 * - Baudrate accuracy and timing behavior depend on the selected XIO backend.
 * - Pico requires baudrate in 1..UINT32_MAX/8 and the exact divider
 *   clk_sys_hz/(8*baudrate) in [1, 65536]. Out-of-range rates return INVALID_ARG
 *   and release partial resources; no clamping to the system clock occurs.
 *   The valid divider rounds down to 1/256 resolution. An unavailable system
 *   clock returns STATE before allocating that direction's resources.
 */
esy_err_t esy_hal_xio_uart_init(esy_uart_t *uart,
                                    esy_xio_uart_config_t *tx_cfg,
                                    esy_xio_uart_config_t *rx_cfg);

/**
 * @brief Query retained DMA work or physical TX from the serialized owner.
 *
 * @param[in] uart UART handle.
 * @param[in] is_tx true for TX including FIFO/shift/stop-bit drain; false for RX DMA.
 *
 * @retval true The selected direction still owns DMA or physical TX work.
 * @retval false The selected direction is idle; accepted TX is physically drained.
 */
bool esy_hal_xio_uart_is_busy(esy_uart_t *uart, bool is_tx);

/**
 * @brief Deinitialize and release UART over XIO resources (idempotent).
 *
 * This function stops TX/RX programs (if active), releases XIO resources,
 * and clears backend state. Calling it multiple times is safe.
 * Registered handles, tracked DMA, callbacks or undrained physical TX leave the
 * complete handle unchanged. Poll/abort DMA and check TX busy until false, then
 * retry from the serialized owner. Aborting DMA does not discard queued bytes.
 *
 * @param[in,out] uart UART handle.
 */
void esy_hal_xio_uart_deinit(esy_uart_t *uart);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HAL_XIO_UART_H */
