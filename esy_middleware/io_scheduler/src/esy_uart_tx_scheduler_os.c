/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler_os.h"

#include <stdatomic.h>
#include <string.h>

#include "esy_hal/esy_hal_system.h"
#include "esy_io_scheduler_task_resource_internal.h"

#define UART_OS_CORE_REQUEST_COUNT 2u
#define UART_OS_ACTIVE_WAIT_MS 1u

enum { UART_OS_OPEN = 0u, UART_OS_DRAIN, UART_OS_CANCEL_QUEUED, UART_OS_ABORT };

typedef struct {
  esy_uart_tx_scheduler_t core;
  esy_uart_tx_scheduler_tx_request_t core_requests[UART_OS_CORE_REQUEST_COUNT];
  esy_os_queue_t *queue;
  esy_os_task_t *task;
  atomic_uint stop_mode;
  atomic_bool ready;
  bool initialized;
} esy_uart_tx_scheduler_os_ctx_t;

_Static_assert(sizeof(esy_uart_tx_scheduler_os_ctx_t) <=
    ESY_UART_TX_SCHEDULER_OS_STORAGE_BYTES, "UART OS adapter storage too small");
_Static_assert(alignof(esy_uart_tx_scheduler_os_ctx_t) <= alignof(max_align_t),
    "UART OS adapter alignment is insufficient");

static esy_uart_tx_scheduler_os_ctx_t *uart_os_ctx(esy_uart_tx_scheduler_os_t *adapter) {
  return adapter != NULL ? (esy_uart_tx_scheduler_os_ctx_t *)(void *)adapter->bytes : NULL;
}

/* Exactly one worker touches core state. Producers only use the MPSC OS queue;
 * the lifecycle owner publishes monotonic shutdown escalation atomically. */
static bool uart_os_step(esy_uart_tx_scheduler_os_ctx_t *ctx, uint32_t *wait_ms) {
  *wait_ms = UINT32_MAX;
  if (!atomic_load_explicit(&ctx->ready, memory_order_acquire)) return false;
  unsigned mode = atomic_load_explicit(&ctx->stop_mode, memory_order_acquire);
  if (mode == UART_OS_ABORT && esy_uart_tx_scheduler_is_busy(&ctx->core))
    (void)esy_uart_tx_scheduler_request_abort(&ctx->core);
  if (!esy_uart_tx_scheduler_is_busy(&ctx->core)) {
    esy_uart_tx_scheduler_tx_request_t request;
    esy_err_t receive = esy_os_queue_receive(ctx->queue, &request, 0u);
    if (receive == ESY_OK) {
      if (mode >= UART_OS_CANCEL_QUEUED) {
        request.on_complete(request.data, request.length,
            ESY_UART_TX_SCHEDULER_TX_CANCELLED, ESY_OK, request.user_data);
      } else {
        esy_err_t result = esy_uart_tx_scheduler_submit(&ctx->core, &request);
        if (result != ESY_OK)
          request.on_complete(request.data, request.length,
              ESY_UART_TX_SCHEDULER_TX_ERROR, result, request.user_data);
      }
      *wait_ms = 0u;
    } else if (mode != UART_OS_OPEN && receive == ESY_ERR_TIMEOUT) {
      if (esy_uart_tx_scheduler_deinit(&ctx->core) == ESY_OK) return true;
      *wait_ms = UART_OS_ACTIVE_WAIT_MS;
    } else if (receive != ESY_ERR_TIMEOUT) {
      /* A receive failure does not prove the queue empty or release requests. */
      *wait_ms = UART_OS_ACTIVE_WAIT_MS;
    }
  }
  if (esy_uart_tx_scheduler_is_busy(&ctx->core)) {
    (void)esy_uart_tx_scheduler_process(&ctx->core);
    *wait_ms = esy_uart_tx_scheduler_next_wait_ms(&ctx->core);
    /* Revisit the queue after a completion; no producer wake is required for
     * descriptors accepted before the worker started its current request. */
    if (*wait_ms == UINT32_MAX) *wait_ms = 0u;
  }
  return false;
}

static void uart_os_worker(void *arg) {
  esy_uart_tx_scheduler_os_ctx_t *ctx = arg;
  for (;;) {
    uint32_t wait_ms;
    if (uart_os_step(ctx, &wait_ms)) {
      /* After parking the owner may reclaim ctx; never access it on resume. */
      for (;;) (void)esy_os_task_park();
    }
    (void)esy_os_task_notify_take(true, wait_ms);
  }
}

esy_err_t esy_uart_tx_scheduler_os_create(esy_uart_tx_scheduler_os_t *adapter,
    esy_uart_t *uart, const esy_uart_tx_scheduler_os_config_t *config) {
  if (adapter == NULL || uart == NULL || config == NULL || esy_hal_system_is_in_isr())
    return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_os_ctx_t *ctx = uart_os_ctx(adapter);
  if (ctx->initialized) return ESY_ERR_STATE;
  const esy_uart_tx_scheduler_os_static_resources_t *resources = &config->static_resources;
  if (resources->requests == NULL || resources->request_count == 0u ||
      SIZE_MAX / resources->request_count < sizeof(*resources->requests) ||
      (uintptr_t)resources->requests % alignof(esy_uart_tx_scheduler_tx_request_t) != 0u ||
      resources->queue_control_storage == NULL || resources->task_control_storage == NULL ||
      !esy_io_scheduler_task_stack_resource_is_valid(resources->task_stack_buffer,
          resources->task_stack_buffer_size, config->task_stack_size))
    return ESY_ERR_INVALID_ARG;
  if (!esy_os_task_park_supported()) return ESY_ERR_NOT_SUPPORTED;
  memset(ctx, 0, sizeof(*ctx));
  atomic_init(&ctx->stop_mode, UART_OS_OPEN);
  atomic_init(&ctx->ready, false);
  esy_uart_tx_scheduler_static_resources_t core_resources = {
      .requests = ctx->core_requests, .request_count = UART_OS_CORE_REQUEST_COUNT};
  esy_err_t result = esy_uart_tx_scheduler_init(&ctx->core, uart, &core_resources);
  if (result != ESY_OK) return result;
  result = esy_os_queue_create_static(sizeof(*resources->requests),
      resources->request_count, resources->requests, resources->queue_control_storage,
      &ctx->queue);
  if (result != ESY_OK) {
    (void)esy_uart_tx_scheduler_deinit(&ctx->core);
    return result;
  }
  ctx->initialized = true;
  result = esy_os_task_create_static(config->task_name, uart_os_worker, ctx,
      config->task_stack_size, config->task_priority, resources->task_stack_buffer,
      resources->task_control_storage, &ctx->task);
  if (result != ESY_OK) {
    esy_os_queue_delete(ctx->queue);
    (void)esy_uart_tx_scheduler_deinit(&ctx->core);
    memset(ctx, 0, sizeof(*ctx));
    return result;
  }
  esy_os_task_set_core_affinity(ctx->task, config->task_core_affinity);
  atomic_store_explicit(&ctx->ready, true, memory_order_release);
  (void)esy_os_task_notify_give(ctx->task);
  return ESY_OK;
}

esy_err_t esy_uart_tx_scheduler_os_submit(esy_uart_tx_scheduler_os_t *adapter,
    const esy_uart_tx_scheduler_tx_request_t *request) {
  if (adapter == NULL || request == NULL || request->data == NULL ||
      request->length == 0u || request->length > INT32_MAX ||
      request->timeout_ms > INT32_MAX || request->on_complete == NULL ||
      esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_os_ctx_t *ctx = uart_os_ctx(adapter);
  if (!ctx->initialized || atomic_load_explicit(&ctx->stop_mode, memory_order_acquire)
      != UART_OS_OPEN) return ESY_ERR_STATE;
  esy_err_t result = esy_os_queue_send(ctx->queue, request, 0u);
  if (result != ESY_OK) return result == ESY_ERR_TIMEOUT ? ESY_ERR_NO_MEM : result;
  (void)esy_os_task_notify_give(ctx->task);
  return ESY_OK;
}

static esy_err_t uart_os_stop(esy_uart_tx_scheduler_os_t *adapter, unsigned mode) {
  if (adapter == NULL || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_os_ctx_t *ctx = uart_os_ctx(adapter);
  if (!ctx->initialized || esy_os_task_is_current(ctx->task)) return ESY_ERR_STATE;
  unsigned observed = atomic_load_explicit(&ctx->stop_mode, memory_order_acquire);
  while (observed < mode && !atomic_compare_exchange_weak_explicit(&ctx->stop_mode,
      &observed, mode, memory_order_acq_rel, memory_order_acquire)) {}
  (void)esy_os_task_notify_give(ctx->task);
  return ESY_OK;
}

esy_err_t esy_uart_tx_scheduler_os_request_stop(esy_uart_tx_scheduler_os_t *adapter,
    bool cancel_queued) {
  return uart_os_stop(adapter, cancel_queued ? UART_OS_CANCEL_QUEUED : UART_OS_DRAIN);
}

esy_err_t esy_uart_tx_scheduler_os_request_abort(esy_uart_tx_scheduler_os_t *adapter) {
  return uart_os_stop(adapter, UART_OS_ABORT);
}

esy_err_t esy_uart_tx_scheduler_os_destroy(esy_uart_tx_scheduler_os_t *adapter) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_os_ctx_t *ctx = uart_os_ctx(adapter);
  if (ctx == NULL || !ctx->initialized) return ESY_OK;
  if (esy_os_task_is_current(ctx->task) ||
      atomic_load_explicit(&ctx->stop_mode, memory_order_acquire) == UART_OS_OPEN)
    return ESY_ERR_STATE;
  esy_err_t result = esy_io_scheduler_delete_parked(ctx->task);
  if (result != ESY_OK) return result;
  esy_os_queue_delete(ctx->queue);
  memset(ctx, 0, sizeof(*ctx));
  return ESY_OK;
}

#ifdef TEST
/* Deterministic test scheduling executes the same bounded worker slice. */
void esy_uart_tx_scheduler_os_test_iteration(void *arg) {
  uint32_t wait_ms;
  if (arg != NULL && uart_os_step(arg, &wait_ms)) (void)esy_os_task_park();
}
#endif
