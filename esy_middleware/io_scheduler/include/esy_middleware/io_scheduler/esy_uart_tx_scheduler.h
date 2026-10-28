/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_uart_tx_scheduler.h
 * @brief Owner-driven UART TX queue; RX stays in the HAL byte stream.
 *
 * Zero-initialize the handle before first init. All public calls and completion
 * callbacks run on one externally serialized owner context, never an ISR.
 * DMA callbacks only publish atomic completion. No task or memory is allocated.
 * The caller exclusively owns UART TX while initialized; independent HAL
 * RX stream use is allowed. The backend must report physical wire and retained
 * DMA ownership through its busy query. A failed submit may retain a transfer;
 * process keeps its queue slot/data until terminal poll or successful abort.
 * Stop producers before shutdown. Call process periodically.
 * Completion may submit, but must not recursively process, stop, abort, init or deinit.
 * Successful submission copies the descriptor and borrows immutable data through
 * its exactly-once completion callback. Failure borrows nothing and gives no callback.
 */
#ifndef ESY_UART_TX_SCHEDULER_H
#define ESY_UART_TX_SCHEDULER_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Opaque handle capacity, including internal alignment slack. */
#define ESY_UART_TX_SCHEDULER_STORAGE_BYTES 384u
/** @brief Caller-owned handle; must outlive successful deinit. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_UART_TX_SCHEDULER_STORAGE_BYTES];
} esy_uart_tx_scheduler_t;
/** @brief Terminal request outcome. */
typedef enum {
  ESY_UART_TX_SCHEDULER_TX_OK = 0, /**< DMA completed and wire idle. */
  ESY_UART_TX_SCHEDULER_TX_ERROR, /**< Backend rejected or failed the transfer. */
  ESY_UART_TX_SCHEDULER_TX_CANCELLED /**< Unsent request or safely aborted active TX. */
} esy_uart_tx_scheduler_tx_status_t;
/** @brief Owner-context completion; length is requested length, not bytes sent.
 * Error is the backend status on ERROR, otherwise ESY_OK. Partial progress on
 * error is unknown. Data and user_data are borrowed until this call returns. */
typedef void (*esy_uart_tx_scheduler_tx_callback_t)(const uint8_t *data, size_t length,
    esy_uart_tx_scheduler_tx_status_t status, esy_err_t error, void *user_data);
/** @brief One TX descriptor. Only byte transfers are supported. */
typedef struct {
  const uint8_t *data; /**< Immutable borrowed bytes. */
  size_t length; /**< 1..INT32_MAX, additionally limited by the backend. */
  esy_uart_tx_scheduler_tx_callback_t on_complete; /**< Required terminal callback. */
  void *user_data; /**< Borrowed callback context. */
} esy_uart_tx_scheduler_tx_request_t;
/** @brief Caller-owned descriptor storage, valid until deinit succeeds. */
typedef struct {
  esy_uart_tx_scheduler_tx_request_t *requests; /**< Aligned request array. */
  uint32_t request_count; /**< Power of two >=2; capacity is count minus one. */
} esy_uart_tx_scheduler_static_resources_t;
/** @brief Initialize TX only. No UART initialization or RX operations.
 * @return OK; INVALID_ARG for invalid storage; STATE if already initialized;
 * BUSY if physical TX is active. Failure retains no resources. */
esy_err_t esy_uart_tx_scheduler_init(esy_uart_tx_scheduler_t *scheduler, esy_uart_t *uart,
    const esy_uart_tx_scheduler_static_resources_t *resources);
/** @brief Enqueue without starting DMA or invoking callbacks.
 * @return OK on acceptance; NO_MEM if full; STATE if inactive/stopping;
 * INVALID_ARG for invalid request. */
esy_err_t esy_uart_tx_scheduler_submit(esy_uart_tx_scheduler_t *scheduler,
    const esy_uart_tx_scheduler_tx_request_t *request);
/** @brief Advance at most one terminal request and start at most one transfer.
 * Success waits for both terminal DMA notification and physical wire idle.
 * Polls UART retirement once. A failed submission that remains busy gets at
 * most one abort attempt per process call; its original error is completed only
 * after retirement. Cleanup failure retains the request for retry without resend.
 * request_abort also retires ordinary accepted TX through at most one abort
 * attempt. A successful abort waits for physical wire idle before CANCELLED;
 * repeated processing does not abort the same successfully retired DMA again.
 * An already observed DMA terminal result with idle wire wins before an abort
 * succeeds. Once abort succeeds, CANCELLED wins later terminal observations.
 * Failed retained submissions always report their original ERROR, even during
 * explicit abort. Explicit cleanup may complete despite a failed owner poll;
 * otherwise that poll error propagates with resources retained.
 * @return OK, INVALID_ARG for NULL, STATE if inactive/recursive, or the UART
 * retirement/abort error. Keep processing retained requests after cleanup failure. */
esy_err_t esy_uart_tx_scheduler_process(esy_uart_tx_scheduler_t *scheduler);
/** @brief Stop accepting TX. If cancel_queued, cancel unsent requests in process;
 * otherwise drain them. Accepted in-flight DMA drains; a failed submission may
 * be aborted to retire its retained borrow before reporting the original error.
 * Repeated calls may escalate drain to cancellation, never reverse shutdown.
 * Calling after request_abort cannot downgrade active cancellation to draining.
 * @return OK, INVALID_ARG for NULL, or STATE if inactive/in a callback. */
esy_err_t esy_uart_tx_scheduler_request_stop(esy_uart_tx_scheduler_t *scheduler, bool cancel_queued);
/** @brief Close admission and request cancellation of all queued and active TX.
 * Call only from the externally serialized non-ISR owner. This call only marks
 * state: it performs no I/O, waits or callbacks. process continues retirement,
 * retries failed aborts and retains every borrowed buffer until exactly one
 * terminal callback. CANCELLED does not imply that no prefix reached the wire.
 * Repeated abort requests are idempotent; request_stop cannot reverse them.
 * RX is never stopped or modified. Only successful deinit followed by init
 * reopens admission. The owner supplies any overall shutdown timeout/recovery.
 * @return OK; INVALID_ARG for NULL; STATE if inactive or during process/callback.
 */
esy_err_t esy_uart_tx_scheduler_request_abort(esy_uart_tx_scheduler_t *scheduler);
/** @brief Owner-only query: any queued or active request, including pending callbacks. */
bool esy_uart_tx_scheduler_is_busy(const esy_uart_tx_scheduler_t *scheduler);
/** @brief Release an idle handle; does not stop or alter RX.
 * @return OK (also NULL/already inactive), BUSY while any request/wire remains,
 * or STATE during process/callback. Failure retains every owned resource. */
esy_err_t esy_uart_tx_scheduler_deinit(esy_uart_tx_scheduler_t *scheduler);
#ifdef __cplusplus
}
#endif
#endif
