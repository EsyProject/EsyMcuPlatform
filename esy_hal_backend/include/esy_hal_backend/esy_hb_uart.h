/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_uart.h
 * @brief UART HAL backend interface (handle-based, no internal allocation).
 *
 * Design rules:
 * - The caller provides storage for the UART handle; the HAL backend must not
 *   perform any heap allocation.
 * - The physical controller index is provided in @ref esy_uart_config_t
 *   (typically via the phys_index field).
 * - Unsupported capabilities must return @ref ESY_ERR_NOT_SUPPORTED
 *   (or an equivalent error code).
 */

#ifndef ESY_HB_UART_H
#define ESY_HB_UART_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name HAL backend top-level API (handle-based, explicit extern) */
/** @{ */

/**
 * @brief Validate UART framing before claiming resources or configuring pins.
 * @param[in] config Requested controller configuration; not retained.
 * @return ESY_OK, ESY_ERR_INVALID_ARG, or ESY_ERR_NOT_SUPPORTED.
 * This synchronous query changes no handle, board, IRQ or native hardware state.
 * data_bits counts payload bits, excluding parity. STM32F4 supports byte-based
 * 7E/7O and 8N/8E/8O; 9-bit payloads and 7N are unsupported.
 */
extern esy_err_t esy_hb_uart_validate_config(const esy_uart_config_t *config);

/**
 * @brief Reserve a physical controller before changing board pins or SDK state.
 *
 * Caller-owned handle storage must start at {0}. A reserved handle may claim
 * again before native initialization has started; a competing handle receives
 * BUSY without side effects.
 * Serialize lifecycle on one owner context. A suspended controller stays claimed.
 * A live handle must not be copied, moved, or have its private pointers changed.
 * @param[in,out] uart Caller-owned handle.
 * @param[in] phys_index Physical controller index.
 * @return ESY_OK, INVALID_ARG, STATE for a live/foreign handle, or BUSY.
 */
esy_err_t esy_hb_uart_claim(esy_uart_t *uart, uint8_t phys_index);

/**
 * @brief Release a reservation after native cleanup and board pin reset.
 *
 * Native deinit must have succeeded first. The handle remains reserved on
 * failure. This is not a substitute for retiring asynchronous work.
 * @param[in,out] uart Reserved handle.
 * @return ESY_OK, INVALID_ARG, STATE for a foreign handle, or BUSY if active.
 */
esy_err_t esy_hb_uart_release(esy_uart_t *uart);

/**
 * @brief Initialize a UART controller.
 *
 * The backend also checks/claims its controller when called directly. On failure,
 * retire partial native state with deinit, reset only this owner's board pins,
 * and release the reservation; failure to clean up must be retried first.
 * @param[in,out] uart   UART handle storage provided by the caller
 * @param[in]     config UART configuration (phys_index, baudrate,
 *                       data bits, parity, stop bits, etc.)
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_uart_init(esy_uart_t *uart,
                                     const esy_uart_config_t *config);

/**
 * @brief Retire native UART resources while retaining the physical reservation.
 *
 * Serialize from the owner after asynchronous callbacks and buffers retire.
 * Failure preserves the handle and reservation for retry. Success is idempotent
 * and retains ctx until board pins are reset and esy_hb_uart_release succeeds.
 * @param[in,out] uart Reserved UART handle.
 * @return ESY_OK, INVALID_ARG, STATE for a foreign handle, BUSY for live work,
 * or a native cleanup error. ESP32 has no platform-owned RX worker: the owner
 * must complete/abort finite RX before cleanup, and callbacks run from poll.
 */
extern esy_err_t esy_hb_uart_deinit(esy_uart_t *uart);

/**
 * @brief Send data in blocking mode.
 *
 * @param[in] uart UART handle
 * @param[in] data Pointer to transmit buffer
 * @param[in] len  Number of bytes to send
 *
 * @return >=0 Number of bytes actually sent
 * @return <0  Error code
 */
extern int32_t esy_hb_uart_send_blocking(esy_uart_t *uart,
                                             const uint8_t *data, size_t len);

/**
 * @brief Receive data in blocking mode.
 *
 * This call blocks until the requested number of bytes is received
 * or an error occurs. Pico and STM32F4 reject active ordinary RX DMA or a
 * continuous RX stream with ESY_ERR_BUSY before reading the peripheral.
 *
 * @param[in]  uart UART handle
 * @param[out] data Receive buffer
 * @param[in]  len  Number of bytes requested
 *
 * @return >=0 Number of bytes actually received
 * @return <0  Error code
 */
extern int32_t esy_hb_uart_receive_blocking(esy_uart_t *uart, uint8_t *data,
                                                size_t len);

/**
 * @brief Transmit through the port's optional DMA-style interface.
 *
 * Pico and STM32F4 start an asynchronous transfer. ESP32 uses a synchronous
 * emulation: this call waits for physical TX completion and invokes the terminal
 * callback inline before returning ESY_OK. It has no bounded completion timeout.
 * Call from the serialized task owner, and include the selected port's blocking
 * behavior and inline callback in the caller's execution and lifetime contract.
 *
 * @param[in] uart       UART handle
 * @param[in] dma_config DMA transmit configuration
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_uart_send_dma(esy_uart_t *uart,
                                         esy_dma_config_t *dma_config);

/**
 * @brief Start a non-blocking DMA-based receive (optional).
 *
 * @param[in] uart       UART handle
 * @param[in] dma_config DMA receive configuration
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_uart_receive_dma(esy_uart_t *uart,
                                            esy_dma_config_t *dma_config);

/** @brief Retire one DMA direction from its serialized task owner.
 * Success disables its UART requests and retires DMA, queued callbacks and
 * buffer access without invoking a terminal callback. Failure preserves the
 * request for poll/abort retry. No ISR or callback reentry is permitted.
 */
esy_err_t esy_hb_uart_abort_dma(esy_uart_t *uart, bool is_tx);

/** @brief Advance bounded task-owned DMA retirement for at most two directions.
 * Pico/STM32 IRQs publish events only. SDK abort time contributes to this call;
 * failed retirement retains buffers and callbacks for a later poll/abort.
 * ESP32 advances at most 1024 RX bytes and one terminal callback per poll;
 * the caller must poll often enough for its configured line rate and buffering.
 */
esy_err_t esy_hb_uart_poll(esy_uart_t *uart);

/** @brief Native direction/callback access remains active until false. */
bool esy_hb_uart_is_dma_busy(const esy_uart_t *uart, bool is_tx);

/** @brief Borrow caller RX storage and publish stream events from the IRQ.
 * Pico's first successful stream start on each core fixes that core's UART RX
 * IRQ priority until reset, because static dispatchers stay installed after
 * stop. Every stream started there must request the same board-provided
 * priority; a conflict returns BUSY before borrowing storage/callbacks or
 * changing hardware. Native users must not change the owned IRQ priorities.
 * Stream lifecycle is serialized by the task owner; callbacks never reenter it.
 */
extern esy_err_t esy_hb_uart_rx_stream_start(
    esy_uart_t *uart, uint8_t *storage, size_t storage_size,
    esy_uart_rx_stream_callback_t callback, void *user_data);

/** @brief Peek an owned stream; STATE certifies no storage/callback is borrowed.
 * Active and retained failed-start streams must return another status. */
extern esy_err_t esy_hb_uart_rx_stream_peek(esy_uart_t *uart,
                                                const uint8_t **data,
                                                size_t *length);

extern esy_err_t esy_hb_uart_rx_stream_consume(esy_uart_t *uart,
                                                   size_t length);

extern esy_err_t esy_hb_uart_rx_stream_stop(esy_uart_t *uart);

/**
 * @brief Dispatch a physical UART interrupt to the bound HAL backend instance.
 *
 * Platforms with application-owned vector tables must call this entry from
 * the corresponding UART ISR. Platforms that register handlers internally may
 * leave it unused.
 */
extern void esy_hb_uart_handle_irq(uint8_t phys_index);

/**
 * @brief Query current UART DMA transfer remaining counts.
 *
 * Remaining counts are reported in DMA transfer units (not bytes). For
 * single-direction DMA usage, either output pointer may be NULL.
 *
 * @param[in]  uart               UART handle
 * @param[out] out_tx_remaining   Remaining TX transfer count (units), may be
 * NULL
 * @param[out] out_rx_remaining   Remaining RX transfer count (units), may be
 * NULL
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_uart_get_dma_trans_count_remaining(
    const esy_uart_t *uart, size_t *out_tx_remaining, size_t *out_rx_remaining);

/**
 * @brief Query whether the UART is still physically transmitting.
 *
 * The result remains true while data is queued in the transmit FIFO or the
 * shift register is sending the final byte and stop bits. DMA ownership is
 * tracked separately by the HAL wrapper.
 *
 * @param[in] uart Initialized UART handle.
 * @return true while the physical transmitter is active; otherwise false.
 */
extern bool esy_hb_uart_is_transmitting(const esy_uart_t *uart);

/**
 * @brief Suspend the UART peripheral (optional).
 *
 * Typically used for low-power modes.
 * Serialize lifecycle and I/O on the owner task after asynchronous work and
 * callbacks retire. ESP32 returns BUSY for an active RX callback/transfer or
 * undrained physical TX. It gates RX data interrupts while retaining the native
 * SDK driver and RX/TX ring buffers until final deinit. This retains SDK
 * buffers and clocks; it is not a claim of deep-sleep or clock-gating support.
 * Repeated ESP32 suspend is harmless. Data operations reject suspended handles.
 * STM32F4 native deinit failure returns ESY_ERR_STATE because hardware may be
 * partially changed; the PM graph must fault rather than remain ACTIVE. The
 * controller reservation and native cleanup state survive. A faulted registered
 * graph requires system recovery/reset; only an unregistered handle can retry
 * final deinit through the ordinary lifecycle APIs.
 *
 * @param[in] uart UART handle
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_uart_suspend(esy_uart_t *uart);

/**
 * @brief Resume the UART peripheral from a suspended state (optional).
 *
 * ESP32 re-enables RX data interrupts on the existing SDK driver and RX
 * ring buffer. Failure retains the suspended state and resources for retry; repeated
 * resume is harmless. Serialize with lifecycle and data operations.
 * @param[in] uart UART handle
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_uart_resume(esy_uart_t *uart);

/**
 * @brief Check whether the UART has readable data.
 *
 * Typically indicates whether the RX FIFO is non-empty.
 *
 * @param[in] uart UART handle
 *
 * @retval true  Data is available
 * @retval false No data available
 */
extern bool esy_hb_uart_is_readable(const esy_uart_t *uart);

/**
 * @brief Set the UART baudrate at runtime (optional).
 *
 * @param[in] uart     UART handle
 * @param[in] baudrate Target baudrate
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_uart_set_baudrate(esy_uart_t *uart,
                                             uint32_t baudrate);

/**
 * @brief Query public UART capabilities for an initialized handle.
 *
 * Uses @ref esy_uart_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  uart     UART handle
 * @param[out] out_caps Public capability bitmask
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_uart_get_caps(const esy_uart_t *uart,
                                         uint32_t *out_caps);

/**
 * @brief Query the physical controller index (optional).
 *
 * @param[in]  uart      UART handle
 * @param[out] out_index Physical controller index
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (@ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_uart_get_phys_index(const esy_uart_t *uart,
                                               uint8_t *out_index);

/** @} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_UART_H */
