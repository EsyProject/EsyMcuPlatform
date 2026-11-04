/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_pwm_scheduler_core.h"
#include "esy_common/ds/esy_block_ring.h"
#include <stdatomic.h>
#include <string.h>

#define PWM_SCHEDULER_RETRY_MS 1u

enum { PWM_DONE = 1u, PWM_HALF = 2u, PWM_ERROR = 4u };

typedef struct {
  esy_block_ring_t queue;
  esy_pwm_t *pwm;
  atomic_uint events;
  bool initialized;
  bool processing;
  bool active;
  bool stopping;
  bool cancel_queued;
  bool abort_requested;
  bool abort_succeeded;
  bool failed_submission;
  esy_err_t submission_error;
} esy_pwm_scheduler_ctx_t;

_Static_assert(sizeof(esy_pwm_scheduler_ctx_t) + alignof(esy_pwm_scheduler_ctx_t) - 1u
    <= ESY_PWM_SCHEDULER_CORE_STORAGE_BYTES, "PWM scheduler storage too small");

static esy_pwm_scheduler_ctx_t *pwm_ctx(const esy_pwm_scheduler_core_t *core) {
  if (!core) return NULL;
  uintptr_t ptr = (uintptr_t)core->bytes;
  const uintptr_t alignment = alignof(esy_pwm_scheduler_ctx_t);
  ptr = (ptr + alignment - 1u) & ~(alignment - 1u);
  return (esy_pwm_scheduler_ctx_t *)ptr;
}

static void pwm_done(void *arg) {
  esy_pwm_scheduler_ctx_t *ctx = arg;
  atomic_fetch_or_explicit(&ctx->events, PWM_DONE, memory_order_release);
}
static void pwm_half(void *arg) {
  esy_pwm_scheduler_ctx_t *ctx = arg;
  atomic_fetch_or_explicit(&ctx->events, PWM_HALF, memory_order_release);
}
static void pwm_error(void *arg) {
  esy_pwm_scheduler_ctx_t *ctx = arg;
  atomic_fetch_or_explicit(&ctx->events, PWM_ERROR, memory_order_release);
}

esy_err_t esy_pwm_scheduler_core_init(esy_pwm_scheduler_core_t *core, esy_pwm_t *pwm,
    const esy_pwm_scheduler_core_config_t *config) {
  if (!core || !pwm || !config)
    return ESY_ERR_INVALID_ARG;
  const esy_pwm_scheduler_core_static_resources_t *r = &config->static_resources;
  if (!r->requests || (uintptr_t)r->requests % alignof(esy_pwm_scheduler_request_t) ||
      r->request_count < 2u || (r->request_count & (r->request_count - 1u)) ||
      SIZE_MAX / r->request_count < sizeof(*r->requests) ||
      UINT32_MAX / r->request_count < sizeof(*r->requests)) return ESY_ERR_INVALID_ARG;
  esy_pwm_scheduler_ctx_t *ctx = pwm_ctx(core);
  if (ctx->initialized) return ESY_ERR_STATE;
  if (!pwm->ops) return ESY_ERR_STATE;
  if (!esy_hal_pwm_is_idle(pwm)) return ESY_ERR_BUSY;
  memset(ctx, 0, sizeof(*ctx));
  esy_err_t result = esy_block_ring_init(&ctx->queue, (uint8_t *)r->requests,
      sizeof(*r->requests), r->request_count);
  if (result != ESY_OK) return result;
  atomic_init(&ctx->events, 0u);
  ctx->pwm = pwm;
  ctx->initialized = true;
  return ESY_OK;
}

esy_err_t esy_pwm_scheduler_core_submit(esy_pwm_scheduler_core_t *core,
    const esy_pwm_scheduler_request_t *request) {
  if (!core || !request || !request->on_complete || !request->transfer.src ||
      !request->transfer.length || request->transfer.callback ||
      request->transfer.callback_arg || request->transfer.half_callback ||
      request->transfer.half_callback_arg || request->transfer.error_callback ||
      request->transfer.error_callback_arg) return ESY_ERR_INVALID_ARG;
  size_t width;
  switch (request->transfer.transfer_size) {
    case ESY_DMA_TRANSFER_SIZE_8: width = 1u; break;
    case ESY_DMA_TRANSFER_SIZE_16: width = 2u; break;
    case ESY_DMA_TRANSFER_SIZE_32: width = 4u; break;
    default: return ESY_ERR_INVALID_ARG;
  }
  if (request->transfer.length % width || (uintptr_t)request->transfer.src % width)
    return ESY_ERR_INVALID_ARG;
  esy_pwm_scheduler_ctx_t *ctx = pwm_ctx(core);
  if (!ctx->initialized || ctx->processing || ctx->stopping) return ESY_ERR_STATE;
  esy_pwm_scheduler_request_t *slot = (esy_pwm_scheduler_request_t *)esy_block_ring_get_write_block(&ctx->queue);
  if (!slot) return ESY_ERR_NO_MEM;
  *slot = *request;
  esy_block_ring_commit_write(&ctx->queue);
  return ESY_OK;
}

static void pwm_finish(esy_pwm_scheduler_ctx_t *ctx,
    esy_pwm_scheduler_status_t status, esy_err_t error) {
  const esy_pwm_scheduler_request_t request =
      *(esy_pwm_scheduler_request_t *)esy_block_ring_get_read_block(&ctx->queue);
  request.on_complete(&request, status, error, request.user_data);
  esy_block_ring_commit_read(&ctx->queue);
  ctx->active = false;
  ctx->failed_submission = false;
  ctx->abort_succeeded = false;
  atomic_store_explicit(&ctx->events, 0u, memory_order_release);
}

static esy_err_t pwm_retire(esy_pwm_scheduler_ctx_t *ctx,
    esy_pwm_scheduler_status_t status, esy_err_t error, bool abort) {
  if (abort && !ctx->abort_succeeded) {
    esy_err_t result = esy_hal_pwm_abort_dma(ctx->pwm);
    if (result != ESY_OK) return result;
    ctx->abort_succeeded = true;
    /* One cleanup operation per turn: a custom provider can retain output
     * after retiring DMA. Disable it on the next turn, never release early. */
    if (!esy_hal_pwm_is_idle(ctx->pwm)) return ESY_ERR_BUSY;
  } else if (!esy_hal_pwm_is_idle(ctx->pwm)) {
    esy_err_t result = esy_hal_pwm_enable(ctx->pwm, false);
    if (result != ESY_OK) return result;
    if (!esy_hal_pwm_is_idle(ctx->pwm)) return ESY_ERR_BUSY;
  }
  pwm_finish(ctx, status, error);
  return ESY_OK;
}

esy_err_t esy_pwm_scheduler_core_process(esy_pwm_scheduler_core_t *core) {
  if (!core) return ESY_ERR_INVALID_ARG;
  esy_pwm_scheduler_ctx_t *ctx = pwm_ctx(core);
  if (!ctx->initialized || ctx->processing) return ESY_ERR_STATE;
  ctx->processing = true;
  esy_err_t result = ESY_OK;
  esy_pwm_scheduler_request_t *request = (esy_pwm_scheduler_request_t *)esy_block_ring_get_read_block(&ctx->queue);
  if (ctx->active) {
    result = ESY_OK;
    unsigned events = atomic_load_explicit(&ctx->events, memory_order_acquire);
    bool terminal = (events & PWM_ERROR) || (!request->transfer.circular && (events & PWM_DONE));
    if (ctx->failed_submission || ctx->abort_requested || terminal) {
      bool abort = (ctx->failed_submission || ctx->abort_requested) && !terminal;
      esy_pwm_scheduler_status_t status = ctx->failed_submission || (events & PWM_ERROR)
          ? ESY_PWM_SCHEDULER_ERROR
          : (terminal && !ctx->abort_succeeded ? ESY_PWM_SCHEDULER_COMPLETE
                                               : ESY_PWM_SCHEDULER_CANCELLED);
      esy_err_t error = ctx->failed_submission ? ctx->submission_error
          : ((events & PWM_ERROR) ? ESY_ERR_IO : ESY_OK);
      result = pwm_retire(ctx, status, error, abort);
    } else if (result == ESY_OK) {
      /* IRQ publication can race the terminal snapshot above. Finite DONE and
       * ERROR remain latched until cleanup succeeds; only circular DONE is
       * nonterminal progress that this branch may consume. */
      unsigned progress_mask = PWM_HALF |
          (request->transfer.circular ? PWM_DONE : 0u);
      unsigned progress = atomic_fetch_and_explicit(&ctx->events,
          ~progress_mask, memory_order_acq_rel);
      uint32_t flags = (progress & PWM_HALF) ? ESY_PWM_SCHEDULER_PROGRESS_HALF : 0u;
      if (request->transfer.circular && (progress & PWM_DONE))
        flags |= ESY_PWM_SCHEDULER_PROGRESS_CYCLE;
      if (flags && request->on_progress)
        request->on_progress(request, flags, request->user_data);
    }
  } else if (request) {
    if (ctx->cancel_queued) pwm_finish(ctx, ESY_PWM_SCHEDULER_CANCELLED, ESY_OK);
    else {
      esy_dma_config_t transfer = request->transfer;
      transfer.callback = pwm_done; transfer.callback_arg = ctx;
      transfer.half_callback = request->on_progress != NULL ? pwm_half : NULL;
      transfer.half_callback_arg = transfer.half_callback != NULL ? ctx : NULL;
      transfer.error_callback = pwm_error; transfer.error_callback_arg = ctx;
      atomic_store_explicit(&ctx->events, 0u, memory_order_release);
      ctx->active = true;
      result = esy_hal_pwm_dma_output(ctx->pwm, &transfer, transfer.circular);
      if (result != ESY_OK) {
        ctx->failed_submission = true;
        ctx->submission_error = result;
        if (esy_hal_pwm_is_idle(ctx->pwm)) pwm_finish(ctx, ESY_PWM_SCHEDULER_ERROR, result);
      }
    }
  }
  ctx->processing = false;
  return result;
}

esy_err_t esy_pwm_scheduler_core_request_stop(esy_pwm_scheduler_core_t *core, bool cancel_queued) {
  if (!core) return ESY_ERR_INVALID_ARG;
  esy_pwm_scheduler_ctx_t *ctx = pwm_ctx(core);
  if (!ctx->initialized || ctx->processing) return ESY_ERR_STATE;
  ctx->stopping = true;
  ctx->cancel_queued |= cancel_queued;
  return ESY_OK;
}

esy_err_t esy_pwm_scheduler_core_request_abort(esy_pwm_scheduler_core_t *core) {
  esy_err_t result = esy_pwm_scheduler_core_request_stop(core, true);
  if (result != ESY_OK) return result;
  pwm_ctx(core)->abort_requested = true;
  return ESY_OK;
}

bool esy_pwm_scheduler_core_is_idle(const esy_pwm_scheduler_core_t *core) {
  const esy_pwm_scheduler_ctx_t *ctx = pwm_ctx(core);
  return ctx && ctx->initialized && !ctx->processing &&
      esy_block_ring_empty(&ctx->queue) && esy_hal_pwm_is_idle(ctx->pwm);
}

uint32_t esy_pwm_scheduler_core_next_wait_ms(const esy_pwm_scheduler_core_t *core) {
  const esy_pwm_scheduler_ctx_t *ctx = pwm_ctx(core);
  if (!ctx || !ctx->initialized || ctx->processing || esy_block_ring_empty(&ctx->queue))
    return UINT32_MAX;
  if (!ctx->active) return 0u;
  return PWM_SCHEDULER_RETRY_MS;
}

esy_err_t esy_pwm_scheduler_core_deinit(esy_pwm_scheduler_core_t *core) {
  if (!core) return ESY_ERR_INVALID_ARG;
  esy_pwm_scheduler_ctx_t *ctx = pwm_ctx(core);
  if (!ctx->initialized) return ESY_OK;
  if (ctx->processing) return ESY_ERR_STATE;
  if (!esy_pwm_scheduler_core_is_idle(core)) return ESY_ERR_BUSY;
  memset(ctx, 0, sizeof(*ctx));
  return ESY_OK;
}
