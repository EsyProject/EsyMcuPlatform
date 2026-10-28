/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hal_uart.h
 * @brief HAL UART interface (handle-based).
 *
 * Design notes:
 * - The caller provides storage for the UART handle; the HAL
 *   assembles backend context and function pointers and performs dispatch.
 * - No controller index or instance ID is exposed to users; all operations
 *   are performed via @ref esy_uart_t* handles.
 * - Blocking APIs return the number of bytes transferred (>= 0);
 *   other APIs return @ref esy_err_t.
 */

#ifndef ESY_HAL_UART_H
#define ESY_HAL_UART_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

/**
 * @note DMA descriptors are copied and never rewritten in caller storage.
 * Circular completion is a cycle notification, not terminal release; the
 * device reference remains held until successful abort or terminal error.
 * Buffers and callback arguments must remain alive throughout that period.
 * Both registered and unregistered handles track DMA until terminal callback
 * return or successful abort. Failed submission/abort may retain borrowed
 * storage: query is_busy and continue poll/abort before reuse. Callback reentry
 * into UART APIs is forbidden. Built-in finite TX/RX requires owner-driven poll
 * for native retirement and completion, except ESP32 TX may complete inline.
 * Circular ordinary DMA is rejected before starting; use dedicated continuous
 * RX where supported. ESP32 finite RX advances only in the serialized owner
 * poll, with at most 1024 bytes and one terminal callback per call. No platform
 * task or SDK event queue drives RX. Poll at a rate that prevents SDK RX-ring
 * overflow, or apply sender backpressure. Callbacks must be bounded and cannot
 * reenter UART I/O or lifecycle.
 * @note Handle lifecycle: initialize caller-owned handle storage with {0} before
 * its first init. Serialize lifecycle and data operations on one owner context.
 * Repeated init, including init while registered, returns ESY_ERR_STATE without
 * changing the live handle. Stop asynchronous work before unregister/deinit.
 * A competing physical-controller owner returns ESY_ERR_BUSY before board or
 * native state changes. Failed init permits retry only after successful cleanup;
 * cleanup failure preserves the handle for deinit retry, with I/O unavailable.
 * Registered handles cannot be deinitialized directly. Successful deinit returns
 * an unregistered, quiescent handle to the zero state, ready for another init.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @note This initializer replaces every field. Apply caller overrides afterward.
 * @brief Fill a UART configuration structure with default values.
 *
 * Writes platform-defined default settings into @p cfg.
 *
 * @param[out] cfg Configuration structure to initialize; must not be NULL.
 */
extern void esy_hal_uart_default_cfg(esy_uart_config_t *cfg);

/**
 * @brief Initialize a UART instance.
 *
 * data_bits is the payload width, excluding parity. STM32F4 byte-buffer APIs
 * support 7E/7O and 8N/8E/8O with one or two stop bits. Other valid framing,
 * including 9-bit payloads, returns NOT_SUPPORTED before board or native setup.
 * Unsupported framing never borrows storage or callback arguments.
 *
 * @param[in,out] uart UART handle provided by the caller.
 * @param[in]     cfg  UART configuration (includes phys_index, baudrate, etc.).
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_uart_init(esy_uart_t *uart,
                                        const esy_uart_config_t *cfg);

/**
 * @brief Deinitialize a UART instance.
 *
 * Applies to hardware UART handles initialized by esy_hal_uart_init; XIO handles
 * use esy_hal_xio_uart_deinit. Resources stay claimed through native cleanup and
 * board pin reset. Cleanup failure preserves the handle for retry; do not clear
 * or reuse it. Registered handles must first be unregistered. Active DMA, stream
 * callbacks, or physical transmission return BUSY. ESP32 finite RX advances
 * only from owner polling; no platform-owned worker survives successful cleanup.
 * Zero-state deinit is harmless.
 *
 * @param[in,out] uart UART handle.
 * @return ESY_OK on completed cleanup, INVALID_ARG for NULL, STATE for registered
 * or foreign-provider handles, BUSY for live work, or a native cleanup error.
 */
extern esy_err_t esy_hal_uart_deinit(esy_uart_t *uart);

/**
 * @brief Blocking UART transmit.
 *
 * @param[in] uart UART handle.
 * @param[in] data Pointer to transmit buffer.
 * @param[in] len  Number of bytes to transmit.
 *
 * @return
 *  - >=0 Number of bytes actually transmitted.
 *  - <0  Error code.
 */
extern int32_t esy_hal_uart_send_blocking(esy_uart_t *uart,
                                               const uint8_t *data, size_t len);

/**
 * @brief Blocking UART receive.
 *
 * Returns ESY_ERR_BUSY while ordinary RX DMA or an RX stream owns reception,
 * including a stream whose start or stop failed and still needs cleanup.
 *
 * @param[in]  uart UART handle.
 * @param[out] data Receive buffer.
 * @param[in]  len  Maximum number of bytes to receive.
 *
 * @return
 *  - >=0 Number of bytes actually received.
 *  - <0  Error code.
 */
extern int32_t esy_hal_uart_receive_blocking(esy_uart_t *uart,
                                                  uint8_t *data, size_t len);

/**
 * @brief DMA-based UART transmit (optional).
 *
 * @param[in] uart    UART handle.
 * @param[in] dma_cfg DMA configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_uart_send_dma(esy_uart_t *uart,
                                            esy_dma_config_t *dma_cfg);

/**
 * @brief DMA-based UART receive (optional).
 *
 * @param[in] uart    UART handle.
 * @param[in] dma_cfg DMA configuration.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_uart_receive_dma(esy_uart_t *uart,
                                               esy_dma_config_t *dma_cfg);

/**
 * @brief Start a continuous asynchronous UART RX byte stream.
 *
 * A stream exclusively owns RX until successful stop. Blocking RX and ordinary
 * RX DMA return ESY_ERR_BUSY during that borrow; independent TX remains usable.
 * Repeated start returns ESY_ERR_STATE without changing the current borrow.
 *
 * The caller owns @p storage and must keep it valid until
 * @ref esy_hal_uart_rx_stream_stop returns ESY_OK. Failed stop retains
 * storage and callback ownership; retry without clearing either. Serialize
 * start/peek/consume/stop and physical lifecycle from the owner task.
 * Start requires both provider start and stop operations. INVALID_ARG,
 * NOT_SUPPORTED and NOT_IMPLEMENTED reject without borrowing storage or callback
 * context. Other start failures may retain them: keep both alive and call stop
 * until it succeeds, including on unregistered handles. A registered handle
 * first attempts stop after such a failure. Successful cleanup releases its
 * temporary graph reference and preserves the start error; failed cleanup
 * returns the cleanup error and retains the graph reference until stop succeeds.
 * Callbacks must be bounded, nonblocking and must not reenter stream lifecycle
 * or DMA configuration/abort. The callback may execute in
 * interrupt context and only indicates stream activity or an error; it does
 * not mark a protocol frame boundary.
 * On Pico, the first successful stream start on each core reserves its UART RX
 * IRQ priority until reset, including after stop/deinit. Every stream started
 * on that core must use the same board-provided priority; a mismatch returns
 * BUSY without borrowing new storage or callback arguments. Do not change
 * those native IRQ priorities. Different cores may select different priorities.
 *
 * @param[in,out] uart UART handle.
 * @param[in,out] storage Caller-owned receive storage.
 * @param[in] storage_size Storage size in bytes; must be at least two. Pico and
 * STM32F4 backends additionally require a power-of-two size and reject other
 * sizes with ESY_ERR_INVALID_ARG.
 * @param[in] callback Required stream event callback.
 * @param[in] user_data Callback argument.
 *
 * @retval ESY_OK Stream started.
 * @retval ESY_ERR_NOT_SUPPORTED Required provider operations are unavailable.
 * @retval <0 Start or cleanup failure; stop may be required before storage reuse.
 */
extern esy_err_t esy_hal_uart_rx_stream_start(
    esy_uart_t *uart, uint8_t *storage, size_t storage_size,
    esy_uart_rx_stream_callback_t callback, void *user_data);

/**
 * @brief Peek the next contiguous readable span without copying.
 *
 * @p data remains valid until the matching consume or stream stop call.
 * A successful call may return a zero length when no bytes are available.
 *
 * @param[in,out] uart UART handle.
 * @param[out] data Start of the contiguous readable span.
 * @param[out] length Number of readable bytes in the span.
 *
 * @retval ESY_OK Span returned.
 * @retval ESY_ERR_STATE No stream owns receive storage or a callback. Providers
 * must not return STATE for an active or failed-start retained stream. Callers
 * may use this result before start to exclude an existing stream owner.
 * Other errors do not establish that storage is unborrowed.
 * @retval <0 Error code.
 */
extern esy_err_t esy_hal_uart_rx_stream_peek(esy_uart_t *uart,
                                                  const uint8_t **data,
                                                  size_t *length);

/**
 * @brief Consume bytes from the most recently peeked span.
 *
 * @param[in,out] uart UART handle.
 * @param[in] length Number of bytes to consume.
 *
 * @retval ESY_OK Bytes consumed.
 * @retval <0 Error code.
 */
extern esy_err_t esy_hal_uart_rx_stream_consume(esy_uart_t *uart,
                                                     size_t length);

/**
 * @brief Stop continuous RX and detach storage only on success.
 *
 * ESY_OK guarantees no later RX callback or producer storage access. On failure,
 * keep the handle/storage/context alive and retry; do not restart or deinitialize.
 *
 * @param[in,out] uart UART handle.
 *
 * @retval ESY_OK Stream stopped or already stopped.
 * @retval <0 Error code.
 */
extern esy_err_t esy_hal_uart_rx_stream_stop(esy_uart_t *uart);

/**
 * @brief Abort an active UART DMA transfer and release its device reference.
 *
 * @param[in,out] uart UART handle.
 * @param[in] is_tx true for TX DMA, false for RX DMA.
 * @retval ESY_OK Transfer was aborted.
 * @retval ESY_ERR_STATE No matching tracked DMA transfer is active.
 * @retval ESY_ERR_BUSY A callback/native receive turn is still retiring.
 *
 * Registration is optional. Success cancels without a terminal callback and
 * releases borrowed storage; failure retains it for a later poll/abort retry.
 * Bytes already queued in the UART transmitter may still leave the wire.
 * @retval <0 Platform DMA abort failure.
 */
extern esy_err_t esy_hal_uart_abort_dma(esy_uart_t *uart, bool is_tx);

/** @brief Retire DMA events from the serialized owner task.
 * All callers must continue polling while DMA is busy. Each call visits at
 * most TX and RX; native abort and user callback execution determine its time.
 * ESP32 finite RX consumes at most 1024 bytes and emits at most one terminal
 * callback per poll. Choose the owner polling period or sender backpressure
 * to prevent the configured SDK RX ring from overflowing.
 * Hardware UART TX completion waits through repeated polls for physical drain;
 * XIO completion retires DMA source access while queued PIO bytes may transmit.
 * Callbacks run here on Pico/STM32/XIO and for ESP32 RX; ESP32 TX may complete
 * inline in send_dma. Never call from ISR, callbacks, or concurrently with UART
 * operations.
 * Every initialized backend must supply the poll hook.
 * @retval ESY_OK No retirement failure (pending work may remain).
 * @retval ESY_ERR_STATE The backend poll hook is missing.
 * @retval <0 Retirement failed; keep storage alive and retry poll/abort.
 */
esy_err_t esy_hal_uart_poll(esy_uart_t *uart);

/**
 * @brief Query current UART DMA transfer remaining counts (optional).
 *
 * Remaining counts are reported in DMA transfer units (not bytes). For
 * single-direction DMA usage, either output pointer may be NULL.
 *
 * @param[in]  uart               UART handle.
 * @param[out] out_tx_remaining   Remaining TX transfer count (units), may be
 * NULL.
 * @param[out] out_rx_remaining   Remaining RX transfer count (units), may be
 * NULL.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (returns ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_uart_get_dma_trans_count_remaining(
    const esy_uart_t *uart, size_t *out_tx_remaining, size_t *out_rx_remaining);

/**
 * @brief Query UART busy status (optional).
 *
 * Busy belongs to this handle: unrelated users of an overlapping DMA route
 * do not make this UART busy. TX includes queued FIFO/shift-register activity.
 *
 * @param[in] uart  UART handle.
 * @param[in] is_tx true to query TX channel; false to query RX channel.
 *
 * For TX, a true result covers both DMA activity and bytes still queued in the
 * UART FIFO or shift register. For RX, it covers ordinary DMA and RX stream
 * ownership, including a failed start/stop whose cleanup has not succeeded.
 *
 * @retval true  Transfer or physical transmission in progress.
 * @retval false Idle.
 */
extern bool esy_hal_uart_is_busy(esy_uart_t *uart, bool is_tx);

/**
 * @brief Suspend UART operation (optional).
 *
 * Typically used for low-power transitions. A destructive or uncertain native
 * suspend failure returns ESY_ERR_STATE. The PM adapter faults its registered
 * graph, which retains the controller and requires system recovery/reset.
 * An unregistered handle retains ownership for final deinit retry. Other errors
 * preserve the previous active state.
 * Stop asynchronous work first and serialize this task-context operation with
 * I/O, callbacks and lifecycle. ESP32 returns BUSY while finite RX is pending,
 * its owner-poll callback is executing, or physical TX is not idle. It retains
 * the SDK driver and RX ring, gating only RX data interrupts until resume; no
 * platform task/event queue participates. SDK clocks/buffers remain allocated.
 * Suspended handles reject data operations.
 *
 * @param[in] uart UART handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_uart_suspend(esy_uart_t *uart);

/**
 * @brief Resume UART operation (optional).
 *
 * Restores UART state after suspension.
 *
 * @param[in] uart UART handle.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_uart_resume(esy_uart_t *uart);

/**
 * @brief Check whether UART RX data is available.
 *
 * Indicates whether the RX FIFO or receive buffer is non-empty.
 *
 * @param[in] uart UART handle.
 *
 * @retval true  Data available for reading.
 * @retval false No data available.
 */
extern bool esy_hal_uart_is_readable(const esy_uart_t *uart);

/**
 * @brief Set UART baud rate at runtime (optional).
 *
 * @param[in] uart     UART handle.
 * @param[in] baudrate Target baud rate.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code (ESY_ERR_NOT_SUPPORTED if unsupported).
 */
extern esy_err_t esy_hal_uart_set_baudrate(esy_uart_t *uart,
                                                uint32_t baudrate);

/**
 * @brief Query public UART capabilities for an initialized handle.
 *
 * Uses @ref esy_uart_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  uart     UART handle.
 * @param[out] out_caps Capability bitmask.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_uart_get_caps(const esy_uart_t *uart,
                                            uint32_t *out_caps);

/**
 * @brief Query physical controller index (optional).
 *
 * @param[in]  uart      UART handle.
 * @param[out] out_index Physical controller index.
 *
 * @retval ESY_OK Success.
 * @retval <0     Error code.
 */
extern esy_err_t esy_hal_uart_get_phys_index(const esy_uart_t *uart,
                                                  uint8_t *out_index);

/**
 * @brief UART interrupt handler entry for application-owned vector tables.
 */
extern void esy_hal_uart_handle_irq(uint8_t phys_index);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_UART_H */
