/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_UART_TX_SCHEDULER_OS_H
#define ESY_UART_TX_SCHEDULER_OS_H

#include <stdalign.h>
#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler.h"
#include "esy_middleware/os/esy_os_queue.h"
#include "esy_middleware/os/esy_os_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque adapter capacity, including its single-worker core. */
#define ESY_UART_TX_SCHEDULER_OS_STORAGE_BYTES 1024u
/** @brief Zero-initialize before first create; retain until destroy succeeds. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[ESY_UART_TX_SCHEDULER_OS_STORAGE_BYTES];
} esy_uart_tx_scheduler_os_t;

/** @brief Aggregate caller-owned resources; none may be reused before destroy. */
typedef struct {
  esy_uart_tx_scheduler_tx_request_t *requests; /**< OS queue backing array. */
  uint32_t request_count; /**< Queue capacity >=1, plus one in-flight request. */
  esy_os_queue_static_storage_t *queue_control_storage; /**< Queue control block. */
  void *task_stack_buffer; /**< Aligned worker stack. */
  size_t task_stack_buffer_size; /**< Stack allocation size in bytes. */
  esy_os_task_static_storage_t *task_control_storage; /**< Worker control block. */
} esy_uart_tx_scheduler_os_static_resources_t;

/** @brief Explicit OS-worker configuration; HAL initialization remains external. */
typedef struct {
  const char *task_name; /**< Optional native task name. */
  uint32_t task_stack_size; /**< Required stack size in bytes. */
  uint32_t task_priority; /**< OS priority. */
  uint8_t task_core_affinity; /**< OS core-affinity policy. */
  esy_uart_tx_scheduler_os_static_resources_t static_resources; /**< Borrowed resources. */
} esy_uart_tx_scheduler_os_config_t;

/** @brief Create a static worker that exclusively owns the existing TX core.
 * Link esy_uart_tx_scheduler_os with ESY_RTOS=FreeRTOS, ENABLE_ESY_SYSTEM and
 * the UART TX scheduler feature enabled. The UART must already be initialized and TX idle. No UART
 * initialization, backend task or dynamic allocation is performed. Independent
 * RX remains available. Completion callbacks run in this worker, never an ISR.
 * The core selects native DMA or bounded synchronous TX automatically; requests
 * and immutable data stay borrowed through exactly one callback's return.
 * First handle storage must be {0}; repeated create returns STATE. Failure
 * releases created OS resources and permits retry. Core resources are internal;
 * the OS queue and task resources remain caller-owned until destroy succeeds.
 * @return OK; INVALID_ARG for invalid storage/ISR; STATE if already live;
 * NOT_SUPPORTED if cooperative parking is unavailable; or provider failure.
 */
esy_err_t esy_uart_tx_scheduler_os_create(esy_uart_tx_scheduler_os_t *adapter,
    esy_uart_t *uart, const esy_uart_tx_scheduler_os_config_t *config);

/** @brief Submit by value from any producer task without starting I/O inline.
 * MPSC admission uses the bounded OS queue with zero wait. Success borrows data
 * and user_data through callback return; failure borrows nothing. Callback may
 * submit, but must not invoke lifecycle/control operations on this adapter.
 * No business process/poll call is required. Lifecycle must not run concurrently
 * with producer calls: stop and join all external producers before stopping.
 * @return OK; NO_MEM if full; STATE if stopped/inactive; INVALID_ARG for ISR
 * or an invalid request (including timeout >INT32_MAX); or queue failure.
 */
esy_err_t esy_uart_tx_scheduler_os_submit(esy_uart_tx_scheduler_os_t *adapter,
    const esy_uart_tx_scheduler_tx_request_t *request);

/** @brief Close admission and either drain or cancel queued work.
 * Stop/join external producers first. Active TX drains; cancellation affects
 * unsent work only. Repeated requests can escalate cancellation, never reopen.
 * Lifecycle/control is serialized by one external owner, never the worker/ISR.
 * A synchronous HAL call already executing runs until its finite request budget;
 * this marking operation does not interrupt it. Native DMA faults may retain
 * resources until explicit abort recovery. No overall stop deadline is implied.
 * @return OK; INVALID_ARG for NULL/ISR; STATE for inactive/worker reentry.
 */
esy_err_t esy_uart_tx_scheduler_os_request_stop(esy_uart_tx_scheduler_os_t *adapter,
    bool cancel_queued);

/** @brief Close admission and request queued plus active cancellation.
 * The worker alone calls core abort and retries retained native cleanup. A
 * synchronous call already executing cannot be preempted by this request;
 * prefix bytes may reach the wire. Exactly one terminal callback still applies.
 * Stop/join producers first; repeated calls are idempotent.
 * @return OK; INVALID_ARG for NULL/ISR; STATE for inactive/worker reentry.
 */
esy_err_t esy_uart_tx_scheduler_os_request_abort(esy_uart_tx_scheduler_os_t *adapter);

/** @brief Join a stopped, cooperatively parked worker and release OS resources.
 * Request stop/abort first, after producers are joined. The worker drains all
 * callbacks and physical TX before parking. Deletion uses the existing bounded
 * cooperative join (up to 100 one-millisecond OS waits, subject to scheduling).
 * BUSY retains the handle, queue, task and buffers for retry; never clear them.
 * Success returns storage to the zero state and permits create again. NULL or
 * an already destroyed handle is harmless. No UART deinitialization occurs.
 * @return OK; BUSY while worker/native work remains; STATE if not stopped or
 * called by the worker; INVALID_ARG from ISR; or an OS deletion failure.
 */
esy_err_t esy_uart_tx_scheduler_os_destroy(esy_uart_tx_scheduler_os_t *adapter);

#ifdef __cplusplus
}
#endif
#endif /* ESY_UART_TX_SCHEDULER_OS_H */
