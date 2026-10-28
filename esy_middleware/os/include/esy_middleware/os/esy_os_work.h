/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_os_work.h
 * @brief Static work queues with task/ISR admission and explicit cleanup.
 *
 * Submissions are MPSC (tasks and API-safe ISRs); one worker runs handlers in
 * FIFO order. Per-object state transitions share the OS task/ISR critical lock.
 * A work object can have only one pending/running submission. Handlers must
 * return in bounded time. Work callbacks execute in worker task context.
 *
 * Create requires unused storage, following esy_os_storage.h. Queue lifecycle,
 * affinity changes and each delayed object's control calls have one external
 * owner. Only submit_from_isr is ISR-callable; the caller aggregates need_yield
 * and yields once at ISR exit. Do not reclaim a queue while any producer can
 * still call into it, even though destroy rejects submissions during shutdown.
 *
 * Synchronous waits require a running scheduler, task context, no critical
 * section, and must not run in a timer callback. They wait for bounded handlers
 * and may block without a timeout. Waiting from the target worker is rejected.
 * During a work wait, new submissions of that object are rejected. Successful
 * cancel_sync/flush allows reclaiming work only after its producers are stopped.
 *
 * Delayed-work control is single-owner and task-only. A bound queue is retained
 * until cancel returns ESY_OK, including after expiry or start failure. Destroy
 * returns BUSY while bindings exist. Reschedule retires the old timer before
 * rebinding or immediate submission; BUSY retains resources and requires retry.
 * A running/pending base work makes reschedule return STATE. Timer expiry makes
 * one non-blocking enqueue attempt; inspect last_result to detect a full queue.
 *
 * Before an OTA/Flash transition requiring worker silence, the consumer must
 * stop producers, cancel delayed work to completion, then destroy/drain queues.
 * Continued operation is consumer policy; this library assumes no Flash/RAM,
 * ISR, watchdog or output-safety policy. Failed cleanup must retain resources.
 */
#ifndef ESY_OS_WORK_H
#define ESY_OS_WORK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/os/esy_os_queue.h"
#include "esy_middleware/os/esy_os_task.h"
#include "esy_middleware/os/esy_os_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque queue-state budget, excluding native control blocks. */
#define ESY_OS_WORK_QUEUE_INTERNAL_STORAGE_BYTES 256u
/** @brief Packed queue and worker task control-block budget. */
#define ESY_OS_WORK_QUEUE_STATIC_STORAGE_BYTES \
  (ESY_OS_WORK_QUEUE_INTERNAL_STORAGE_BYTES + ESY_OS_TASK_STATIC_STORAGE_BYTES + \
   ESY_OS_QUEUE_STATIC_STORAGE_BYTES)
/** @brief Alignment of queue control storage. */
#define ESY_OS_WORK_QUEUE_STATIC_STORAGE_ALIGN 8u
/** @brief Work object state budget. */
#define ESY_OS_WORK_INTERNAL_STORAGE_BYTES 128u
/** @brief Work storage budget. */
#define ESY_OS_WORK_STATIC_STORAGE_BYTES ESY_OS_WORK_INTERNAL_STORAGE_BYTES
/** @brief Work storage alignment. */
#define ESY_OS_WORK_STATIC_STORAGE_ALIGN 8u
/** @brief Delayed state budget, excluding base work and native timer. */
#define ESY_OS_DELAYED_WORK_INTERNAL_STORAGE_BYTES 192u
/** @brief Delayed work control storage budget. */
#define ESY_OS_DELAYED_WORK_STATIC_STORAGE_BYTES \
  (ESY_OS_DELAYED_WORK_INTERNAL_STORAGE_BYTES + ESY_OS_WORK_STATIC_STORAGE_BYTES + \
   ESY_OS_TIMER_STATIC_STORAGE_BYTES)
/** @brief Delayed work alignment. */
#define ESY_OS_DELAYED_WORK_STATIC_STORAGE_ALIGN 8u
/**
 * @brief Exact queue data bytes for a nonzero depth (pointer entries).
 * Depth must not exceed SIZE_MAX / sizeof(void *); create validates this bound.
 */
#define ESY_OS_WORK_QUEUE_BUFFER_BYTES(depth) ((size_t)(depth) * sizeof(void *))

/** @brief Opaque worker queue. */
typedef struct esy_os_work_queue_s esy_os_work_queue_t;
/** @brief Opaque work item. */
typedef struct esy_os_work_s esy_os_work_t;
/** @brief Opaque delayed work item. */
typedef struct esy_os_delayed_work_s esy_os_delayed_work_t;

/** @brief Caller-owned aligned work queue control storage. */
typedef struct esy_os_work_queue_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_WORK_QUEUE_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_WORK_QUEUE_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_WORK_QUEUE_STATIC_STORAGE_BYTES];
} esy_os_work_queue_static_storage_t;

/** @brief Caller-owned aligned work control storage. */
typedef struct esy_os_work_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_WORK_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_WORK_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_WORK_STATIC_STORAGE_BYTES];
} esy_os_work_static_storage_t;

/** @brief Caller-owned aligned delayed work control storage. */
typedef struct esy_os_delayed_work_static_storage_s {
#ifdef __cplusplus
  alignas(ESY_OS_DELAYED_WORK_STATIC_STORAGE_ALIGN)
#else
  _Alignas(ESY_OS_DELAYED_WORK_STATIC_STORAGE_ALIGN)
#endif
  uint8_t bytes[ESY_OS_DELAYED_WORK_STATIC_STORAGE_BYTES];
} esy_os_delayed_work_static_storage_t;

/** @brief All caller-owned worker resources, retained until destroy completes. */
typedef struct {
  void *task_stack_buffer; /**< Stack aligned to ESY_OS_TASK_STACK_ALIGN. */
  size_t task_stack_buffer_size; /**< Stack bytes, nonzero and at most UINT32_MAX. */
  void *queue_buffer; /**< Pointer-entry queue buffer. */
  size_t queue_buffer_size; /**< At least ESY_OS_WORK_QUEUE_BUFFER_BYTES(depth). */
  esy_os_work_queue_static_storage_t *storage; /**< Packed control storage. */
} esy_os_work_queue_resources_t;

/** @brief Worker callback; ctx remains borrowed until work is quiescent. */
typedef void (*esy_os_work_handler_t)(esy_os_work_t *work, void *ctx);

/**
 * @brief Create/start a queue using unused resources; no dynamic allocation.
 * @param name Optional worker name. @param prio Native task priority.
 * @param queue_depth Nonzero number of pointer entries.
 * @param resources Complete storage description, borrowed through destruction.
 * @param out_wq Set to NULL on failure or the created queue on success.
 * @return ESY_OK, INVALID_ARG for storage errors, or a native creation error.
 * Task-creation failure deletes the already-created queue; resources can retry.
 */
esy_err_t esy_os_work_queue_create_static(const char *name, uint32_t prio,
    uint32_t queue_depth, const esy_os_work_queue_resources_t *resources,
    esy_os_work_queue_t **out_wq);
/**
 * @brief Close admission, drain accepted work, park/delete the worker and queue.
 * @return ESY_OK once complete (including repeated destroy); BUSY if delayed work
 * remains bound; STATE on the worker itself; INVALID_ARG for NULL.
 * One task lifecycle owner; all handlers must terminate. The owner must stop
 * external producers before reclaiming memory. A BUSY result leaves admission open.
 */
esy_err_t esy_os_work_queue_destroy(esy_os_work_queue_t *wq);
/** @brief Set affinity under exclusive lifecycle ownership; NULL returns INVALID_ARG. */
esy_err_t esy_os_work_queue_set_core_affinity(esy_os_work_queue_t *wq, uint8_t affinity);
/**
 * @brief Initialize unused work storage and borrow handler/ctx; no zeroing needed.
 * @return ESY_OK or INVALID_ARG; a valid out_work is cleared on failure.
 * Recreate only after producers stop and cancel_sync/flush confirms quiescence.
 */
esy_err_t esy_os_work_create_static(esy_os_work_handler_t handler, void *ctx,
    esy_os_work_static_storage_t *storage, esy_os_work_t **out_work);
/** @brief Non-blocking task submit: INVALID_ARG on NULL, STATE if active/closing, BUSY if full, otherwise OK. */
esy_err_t esy_os_work_submit(esy_os_work_queue_t *wq, esy_os_work_t *work);
/**
 * @brief Non-blocking ISR submit; same status contract as task submit.
 * need_yield is optional and is cleared even on failure. This API never yields.
 */
esy_err_t esy_os_work_submit_from_isr(esy_os_work_queue_t *wq,
    esy_os_work_t *work, bool *need_yield);
/** @brief Cancel the pending cycle only; running/idle work is unchanged. NULL is a no-op. */
void esy_os_work_cancel(esy_os_work_t *work);
/** @brief Cancel/wait for the current cycle; INVALID_ARG on NULL, STATE on its worker. */
esy_err_t esy_os_work_cancel_sync(esy_os_work_t *work);
/** @brief Wait for the current cycle; INVALID_ARG on NULL, STATE on its worker. */
esy_err_t esy_os_work_flush(esy_os_work_t *work);
/** @brief Synchronized task-context pending snapshot; false for NULL. */
bool esy_os_work_is_pending(const esy_os_work_t *work);
/** @brief Synchronized task-context running snapshot; false for NULL. */
bool esy_os_work_is_running(const esy_os_work_t *work);
/**
 * @brief Initialize unused delayed-work storage; borrow handler/ctx.
 * @return ESY_OK or INVALID_ARG; a valid out_dwork is cleared on failure.
 * Reuse is legal only after cancel returns ESY_OK and external users are stopped.
 */
esy_err_t esy_os_delayed_work_create_static(esy_os_work_handler_t handler,
    void *ctx, esy_os_delayed_work_static_storage_t *storage,
    esy_os_delayed_work_t **out_dwork);
/**
 * @brief Retire the previous timer then bind/submit; zero delay submits immediately.
 * @return OK if accepted, BUSY while retiring/full, STATE if base work is active or
 * queue is closing, INVALID_ARG on NULL, or a timer creation/start error.
 * On failure resources remain owned; cancel to completion before reclamation.
 */
esy_err_t esy_os_delayed_work_submit(esy_os_work_queue_t *wq,
    esy_os_delayed_work_t *dwork, uint32_t delay_ms);
/** @brief Submit to the retained queue; STATE if unbound; otherwise submit's statuses. */
esy_err_t esy_os_delayed_work_reschedule(esy_os_delayed_work_t *dwork, uint32_t delay_ms);
/**
 * @brief Non-blocking cancel/poll: BUSY retains resources; OK detaches the queue.
 * Call again after BUSY. Pending work is cancelled; running work finishes normally.
 * Callback context and all backing memory must remain alive until OK.
 */
esy_err_t esy_os_delayed_work_cancel(esy_os_delayed_work_t *dwork);
/** @brief Task-only cancel/wait/detach; STATE on its worker, INVALID_ARG on NULL. */
esy_err_t esy_os_delayed_work_cancel_sync(esy_os_delayed_work_t *dwork);
/** @brief Synchronized countdown snapshot; false after expiry or failed start. */
bool esy_os_delayed_work_is_active(const esy_os_delayed_work_t *dwork);
/**
 * @brief Last enqueue/start result: BUSY during countdown, STATE before submission,
 * OK for accepted work, or a dispatch error after expiry. INVALID_ARG for NULL.
 * Use with is_active: an inactive BUSY indicates a failed enqueue/start, not a timer.
 */
esy_err_t esy_os_delayed_work_last_result(const esy_os_delayed_work_t *dwork);
#ifdef __cplusplus
}
#endif
#endif
