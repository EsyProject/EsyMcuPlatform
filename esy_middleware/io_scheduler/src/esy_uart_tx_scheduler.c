/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler.h"
#include <stdatomic.h>
#include <string.h>
#include "esy_common/ds/esy_block_ring.h"
#include "esy_hal/esy_hal_uart.h"

typedef struct {
  esy_block_ring_t queue;
  esy_uart_t *uart;
  atomic_int terminal;
  bool initialized;
  bool active;
  bool processing;
  bool stopping;
  bool cancel_queued;
  bool abort_current;
  bool abort_succeeded;
  bool failed_submission;
  esy_err_t submission_error;
} esy_uart_tx_scheduler_ctx_t;

#define UART_TX_RETRY_MS 1u

enum { TX_PENDING = 0, TX_SUCCESS, TX_FAILURE };
_Static_assert(sizeof(esy_uart_tx_scheduler_ctx_t) + alignof(esy_uart_tx_scheduler_ctx_t) - 1u
    <= ESY_UART_TX_SCHEDULER_STORAGE_BYTES, "UART scheduler storage too small");

/* Keep the public handle's ordinary alignment while satisfying the ring's
 * internal cache-line alignment, including when embedded in another handle. */
static esy_uart_tx_scheduler_ctx_t *uart_ctx(const esy_uart_tx_scheduler_t *scheduler) {
  if (!scheduler) return NULL;
  uintptr_t address = (uintptr_t)scheduler->bytes;
  const uintptr_t alignment = alignof(esy_uart_tx_scheduler_ctx_t);
  address = (address + alignment - 1u) & ~(alignment - 1u);
  return (esy_uart_tx_scheduler_ctx_t *)address;
}

static void uart_tx_done(void *arg) {
  esy_uart_tx_scheduler_ctx_t *ctx = arg;
  atomic_store_explicit(&ctx->terminal, TX_SUCCESS, memory_order_release);
}
static void uart_tx_error(void *arg) {
  esy_uart_tx_scheduler_ctx_t *ctx = arg;
  atomic_store_explicit(&ctx->terminal, TX_FAILURE, memory_order_release);
}

esy_err_t esy_uart_tx_scheduler_init(esy_uart_tx_scheduler_t *scheduler, esy_uart_t *uart,
    const esy_uart_tx_scheduler_static_resources_t *resources) {
  if (!scheduler || !uart || !resources || !resources->requests ||
      (uintptr_t)resources->requests % alignof(esy_uart_tx_scheduler_tx_request_t) ||
      resources->request_count < 2u ||
      (resources->request_count & (resources->request_count - 1u)) ||
      SIZE_MAX / resources->request_count < sizeof(*resources->requests))
    return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_ctx_t *ctx = uart_ctx(scheduler);
  if (ctx->initialized) return ESY_ERR_STATE;
  if (esy_hal_uart_is_busy(uart, true)) return ESY_ERR_BUSY;
  memset(ctx, 0, sizeof(*ctx));
  esy_err_t err = esy_block_ring_init(&ctx->queue, (uint8_t *)resources->requests,
      sizeof(*resources->requests), resources->request_count);
  if (err != ESY_OK) return err;
  atomic_init(&ctx->terminal, TX_PENDING);
  ctx->uart = uart;
  ctx->initialized = true;
  return ESY_OK;
}

esy_err_t esy_uart_tx_scheduler_submit(esy_uart_tx_scheduler_t *scheduler,
    const esy_uart_tx_scheduler_tx_request_t *request) {
  if (!scheduler || !request || !request->data || !request->length ||
      request->length > INT32_MAX || request->timeout_ms > INT32_MAX || !request->on_complete) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_ctx_t *ctx = uart_ctx(scheduler);
  if (!ctx->initialized || ctx->stopping) return ESY_ERR_STATE;
  esy_uart_tx_scheduler_tx_request_t *slot =
      (esy_uart_tx_scheduler_tx_request_t *)esy_block_ring_get_write_block(&ctx->queue);
  if (!slot) return ESY_ERR_NO_MEM;
  *slot = *request;
  esy_block_ring_commit_write(&ctx->queue);
  return ESY_OK;
}

static void uart_finish(esy_uart_tx_scheduler_ctx_t *ctx,
    esy_uart_tx_scheduler_tx_status_t status, esy_err_t error) {
  const esy_uart_tx_scheduler_tx_request_t request =
      *(esy_uart_tx_scheduler_tx_request_t *)esy_block_ring_get_read_block(&ctx->queue);
  ctx->active = false;
  ctx->failed_submission = false;
  ctx->abort_succeeded = false;
  esy_block_ring_commit_read(&ctx->queue);
  request.on_complete(request.data, request.length, status, error, request.user_data);
}

esy_err_t esy_uart_tx_scheduler_process(esy_uart_tx_scheduler_t *scheduler) {
  if (!scheduler) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_ctx_t *ctx = uart_ctx(scheduler);
  if (!ctx->initialized || ctx->processing) return ESY_ERR_STATE;
  ctx->processing = true;

  if (ctx->active && ctx->failed_submission) {
    if (!ctx->abort_succeeded && esy_hal_uart_is_busy(ctx->uart, true)) {
      esy_err_t result = esy_hal_uart_abort_dma(ctx->uart, true);
      /* A retired DMA can leave UART bytes on the wire; STATE keeps waiting. */
      if (result != ESY_OK && result != ESY_ERR_STATE) {
        ctx->processing = false;
        return result;
      }
      if (result == ESY_OK) ctx->abort_succeeded = true;
    }
    if (!esy_hal_uart_is_busy(ctx->uart, true)) {
      uart_finish(ctx, ESY_UART_TX_SCHEDULER_TX_ERROR, ctx->submission_error);
      ctx->processing = false;
      return ESY_OK;
    }
    ctx->processing = false;
    return ESY_OK;
  }
  if (ctx->active && ctx->abort_current) {
    bool busy = esy_hal_uart_is_busy(ctx->uart, true);
    int terminal = atomic_load_explicit(&ctx->terminal, memory_order_acquire);
    /* A terminal result observed with idle wire wins before abort commits.
     * After successful abort, cancellation wins all later observations. */
    if (!ctx->abort_succeeded && terminal != TX_PENDING && !busy) {
      uart_finish(ctx, terminal == TX_SUCCESS ? ESY_UART_TX_SCHEDULER_TX_OK :
          ESY_UART_TX_SCHEDULER_TX_ERROR, terminal == TX_SUCCESS ? ESY_OK : ESY_ERR_IO);
      ctx->processing = false;
      return ESY_OK;
    }
    if (!ctx->abort_succeeded) {
      esy_err_t result = esy_hal_uart_abort_dma(ctx->uart, true);
      if (result != ESY_OK && result != ESY_ERR_STATE) {
        ctx->processing = false;
        return result;
      }
      if (result == ESY_OK) ctx->abort_succeeded = true;
    }
    if (ctx->abort_succeeded && !esy_hal_uart_is_busy(ctx->uart, true)) {
      uart_finish(ctx, ESY_UART_TX_SCHEDULER_TX_CANCELLED, ESY_OK);
      ctx->processing = false;
      return ESY_OK;
    }
    ctx->processing = false;
    return ESY_OK;
  }
  if (ctx->active) {
    int terminal = atomic_load_explicit(&ctx->terminal, memory_order_acquire);
    if (terminal != TX_PENDING && !esy_hal_uart_is_busy(ctx->uart, true))
      uart_finish(ctx, terminal == TX_SUCCESS ? ESY_UART_TX_SCHEDULER_TX_OK :
          ESY_UART_TX_SCHEDULER_TX_ERROR, terminal == TX_SUCCESS ? ESY_OK : ESY_ERR_IO);
    /* A callback may enqueue; leave its transfer to the next bounded slice. */
  } else {
    esy_uart_tx_scheduler_tx_request_t *request =
        (esy_uart_tx_scheduler_tx_request_t *)esy_block_ring_get_read_block(&ctx->queue);
    if (request) {
      if (ctx->cancel_queued) {
        uart_finish(ctx, ESY_UART_TX_SCHEDULER_TX_CANCELLED, ESY_OK);
      } else if (!esy_hal_uart_is_busy(ctx->uart, true)) {
        esy_dma_config_t dma = {
            .src = request->data,
            .length = request->length,
            .transfer_size = ESY_DMA_TRANSFER_SIZE_8,
            .callback = uart_tx_done,
            .callback_arg = ctx,
            .error_callback = uart_tx_error,
            .error_callback_arg = ctx,
            .inner = {.direction = ESY_DMA_MEM_TO_MEM},
        };
        atomic_store_explicit(&ctx->terminal, TX_PENDING, memory_order_relaxed);
        ctx->abort_succeeded = false;
        ctx->active = true;
        esy_err_t err = esy_hal_uart_send_dma(ctx->uart, &dma);
        if ((err == ESY_ERR_NOT_SUPPORTED || err == ESY_ERR_NOT_IMPLEMENTED) &&
            !esy_hal_uart_is_busy(ctx->uart, true)) {
          uint32_t timeout_ms = request->timeout_ms != 0u ? request->timeout_ms
              : ESY_UART_TX_SCHEDULER_DEFAULT_TIMEOUT_MS;
          int32_t count = esy_hal_uart_send_timeout(ctx->uart, request->data,
                                                   request->length, timeout_ms);
          if (count == (int32_t)request->length)
            atomic_store_explicit(&ctx->terminal, TX_SUCCESS, memory_order_release);
          else uart_finish(ctx, ESY_UART_TX_SCHEDULER_TX_ERROR,
                           count < 0 ? (esy_err_t)count : ESY_ERR_IO);
        } else if (err != ESY_OK) {
          if (esy_hal_uart_is_busy(ctx->uart, true)) {
            ctx->failed_submission = true;
            ctx->submission_error = err;
          } else {
            uart_finish(ctx, ESY_UART_TX_SCHEDULER_TX_ERROR, err);
          }
        }
      }
    }
  }
  ctx->processing = false;
  return ESY_OK;
}

esy_err_t esy_uart_tx_scheduler_request_stop(esy_uart_tx_scheduler_t *scheduler, bool cancel_queued) {
  if (!scheduler) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_ctx_t *ctx = uart_ctx(scheduler);
  if (!ctx->initialized || ctx->processing) return ESY_ERR_STATE;
  ctx->stopping = true;
  ctx->cancel_queued = ctx->cancel_queued || cancel_queued;
  return ESY_OK;
}

esy_err_t esy_uart_tx_scheduler_request_abort(esy_uart_tx_scheduler_t *scheduler) {
  if (!scheduler) return ESY_ERR_INVALID_ARG;
  esy_uart_tx_scheduler_ctx_t *ctx = uart_ctx(scheduler);
  if (!ctx->initialized || ctx->processing) return ESY_ERR_STATE;
  ctx->stopping = true;
  ctx->cancel_queued = true;
  ctx->abort_current = true;
  return ESY_OK;
}

uint32_t esy_uart_tx_scheduler_next_wait_ms(const esy_uart_tx_scheduler_t *scheduler) {
  const esy_uart_tx_scheduler_ctx_t *ctx = uart_ctx(scheduler);
  if (!ctx || !ctx->initialized || esy_block_ring_empty(&ctx->queue)) return UINT32_MAX;
  if (!ctx->active) return 0u;
  return UART_TX_RETRY_MS;
}

bool esy_uart_tx_scheduler_is_busy(const esy_uart_tx_scheduler_t *scheduler) {
  const esy_uart_tx_scheduler_ctx_t *ctx = uart_ctx(scheduler);
  return ctx && ctx->initialized && !esy_block_ring_empty(&ctx->queue);
}

esy_err_t esy_uart_tx_scheduler_deinit(esy_uart_tx_scheduler_t *scheduler) {
  esy_uart_tx_scheduler_ctx_t *ctx = uart_ctx(scheduler);
  if (!ctx || !ctx->initialized) return ESY_OK;
  if (ctx->processing) return ESY_ERR_STATE;
  if (!esy_block_ring_empty(&ctx->queue) || esy_hal_uart_is_busy(ctx->uart, true))
    return ESY_ERR_BUSY;
  memset(ctx, 0, sizeof(*ctx));
  return ESY_OK;
}
