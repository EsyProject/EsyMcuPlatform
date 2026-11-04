/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_dma_scheduler_core.h"

#include <string.h>
#include <stdatomic.h>

#define DMA_SCHEDULER_MAGIC UINT32_C(0x44534D41)
#define DMA_SCHEDULER_RETRY_MS 1u

typedef struct {
    uint32_t magic;
    esy_block_ring_t queue;
    esy_dma_t *dma;
    esy_dma_scheduler_request_t *current;
    esy_err_t result;
    bool processing;
    bool stopping;
    bool cancel_queued;
    bool abort_requested;
    bool cleanup;
    bool terminal;
    atomic_uint events;
} dma_scheduler_ctx_t;

_Static_assert(sizeof(dma_scheduler_ctx_t) <= ESY_DMA_SCHEDULER_CORE_STORAGE_BYTES,
               "DMA scheduler storage is too small");
_Static_assert(_Alignof(esy_dma_scheduler_core_t) >= _Alignof(dma_scheduler_ctx_t),
               "DMA scheduler storage alignment is insufficient");

static dma_scheduler_ctx_t *get_context(const esy_dma_scheduler_core_t *core) {
    if (core == NULL) return NULL;
    uint32_t magic;
    memcpy(&magic, core->bytes, sizeof(magic));
    return magic == DMA_SCHEDULER_MAGIC
        ? (dma_scheduler_ctx_t *)(void *)core->bytes : NULL;
}

static bool is_reentrant(const dma_scheduler_ctx_t *ctx) {
    return ctx->processing;
}

enum { DMA_DONE = 1u, DMA_HALF = 2u, DMA_ERROR = 4u };

/* HAL IRQ callbacks publish only. The serialized scheduler owner delivers
 * every application callback and owns the queued request's lifetime. */
static void on_complete(void *arg) {
    dma_scheduler_ctx_t *ctx = arg;
    atomic_fetch_or_explicit(&ctx->events, DMA_DONE, memory_order_release);
}
static void on_half(void *arg) {
    dma_scheduler_ctx_t *ctx = arg;
    atomic_fetch_or_explicit(&ctx->events, DMA_HALF, memory_order_release);
}
static void on_error(void *arg) {
    dma_scheduler_ctx_t *ctx = arg;
    atomic_fetch_or_explicit(&ctx->events, DMA_ERROR, memory_order_release);
}

static void deliver_events(dma_scheduler_ctx_t *ctx) {
    unsigned events = atomic_exchange_explicit(&ctx->events, 0u, memory_order_acq_rel);
    const esy_dma_config_t *transfer = &ctx->current->transfer;
    bool terminal = (events & DMA_ERROR) ||
        (!transfer->circular && (events & DMA_DONE));
    if (terminal && !esy_hal_dma_is_idle(ctx->dma)) {
        /* A peer owner can observe publication before the HAL IRQ callback
         * drops its lease. Keep the event until that final borrow retires. */
        atomic_fetch_or_explicit(&ctx->events, events, memory_order_release);
        return;
    }
    if (events & DMA_ERROR) {
        ctx->terminal = true;
        ctx->result = ESY_ERR_IO;
        if (transfer->error_callback) transfer->error_callback(transfer->error_callback_arg);
        return;
    }
    if ((events & DMA_HALF) && transfer->half_callback)
        transfer->half_callback(transfer->half_callback_arg);
    if (events & DMA_DONE) {
        if (!transfer->circular) {
            ctx->terminal = true;
            ctx->result = ESY_OK;
        }
        if (transfer->callback) transfer->callback(transfer->callback_arg);
    }
}

/* Routes are public immutable controller configuration, not runtime state.
 * A request selects exactly one configured pair; no batch policy is inferred. */
static uint32_t route_mask(const dma_scheduler_ctx_t *ctx,
                            const esy_dma_config_t *transfer) {
    for (uint32_t i = 0u; i < ctx->dma->channel_count; ++i) {
        const esy_dma_inner_config_t *route = &ctx->dma->channels[i];
        if (route->dreq == transfer->inner.dreq &&
            route->channel == transfer->inner.channel) return UINT32_C(1) << i;
    }
    return 0u;
}

static void finish_current(dma_scheduler_ctx_t *ctx, esy_err_t result,
                            bool cancelled) {
    if (ctx->current->done != NULL)
        ctx->current->done(result, cancelled, ctx->current->done_arg);
    ctx->current = NULL;
    ctx->cleanup = false;
    ctx->terminal = false;
    esy_block_ring_commit_read(&ctx->queue);
}

esy_err_t esy_dma_scheduler_core_init(esy_dma_scheduler_core_t *core,
    esy_dma_t *dma, const esy_dma_scheduler_core_config_t *config) {
    if (core == NULL || dma == NULL || config == NULL) return ESY_ERR_INVALID_ARG;
    if (get_context(core) != NULL) return ESY_ERR_STATE;
    const esy_dma_scheduler_core_resources_t *resources = &config->static_resources;
    const uint32_t count = resources->request_storage_count;
    if (resources->request_storage == NULL || count < 2u ||
        (count & (count - 1u)) != 0u ||
        count > UINT32_MAX / sizeof(esy_dma_scheduler_request_t) ||
        (uintptr_t)resources->request_storage % _Alignof(esy_dma_scheduler_request_t) != 0u)
        return ESY_ERR_INVALID_ARG;
    if (!esy_hal_dma_is_idle(dma)) return ESY_ERR_BUSY;
    memset(core->bytes, 0, sizeof(core->bytes));
    dma_scheduler_ctx_t *ctx = (dma_scheduler_ctx_t *)(void *)core->bytes;
    esy_err_t result = esy_block_ring_init(&ctx->queue,
        (uint8_t *)resources->request_storage, sizeof(esy_dma_scheduler_request_t), count);
    if (result != ESY_OK) return result;
    atomic_init(&ctx->events, 0u);
    ctx->dma = dma;
    ctx->magic = DMA_SCHEDULER_MAGIC;
    return ESY_OK;
}

esy_err_t esy_dma_scheduler_core_submit(esy_dma_scheduler_core_t *core,
    const esy_dma_scheduler_request_t *request) {
    if (core == NULL || request == NULL) return ESY_ERR_INVALID_ARG;
    dma_scheduler_ctx_t *ctx = get_context(core);
    if (ctx == NULL || is_reentrant(ctx) || ctx->stopping) return ESY_ERR_STATE;
    if (request->transfer.length == 0u || route_mask(ctx, &request->transfer) == 0u)
        return ESY_ERR_INVALID_ARG;
    uint8_t *slot = esy_block_ring_get_write_block(&ctx->queue);
    if (slot == NULL) return ESY_ERR_BUSY;
    memcpy(slot, request, sizeof(*request));
    esy_block_ring_commit_write(&ctx->queue);
    return ESY_OK;
}

static esy_err_t start_current(dma_scheduler_ctx_t *ctx) {
    atomic_store_explicit(&ctx->events, 0u, memory_order_release);
    esy_dma_config_t transfer = ctx->current->transfer;
    transfer.callback = on_complete;
    transfer.callback_arg = ctx;
    transfer.half_callback = ctx->current->transfer.half_callback != NULL ? on_half : NULL;
    transfer.half_callback_arg = transfer.half_callback != NULL ? ctx : NULL;
    transfer.error_callback = on_error;
    transfer.error_callback_arg = ctx;
    esy_err_t result = esy_hal_dma_prepare(ctx->dma, &transfer);
    if (result != ESY_OK) {
        finish_current(ctx, result, false);
        return result;
    }
    result = esy_hal_dma_start(ctx->dma, route_mask(ctx, &transfer));
    if (result != ESY_OK) {
        /* is_busy deliberately excludes prepared routes. A failed start still
         * borrows hardware/storage, so always abort on a later owner turn. */
        ctx->result = result;
        ctx->cleanup = true;
    }
    return result;
}

esy_err_t esy_dma_scheduler_core_process(esy_dma_scheduler_core_t *core) {
    if (core == NULL) return ESY_ERR_INVALID_ARG;
    dma_scheduler_ctx_t *ctx = get_context(core);
    if (ctx == NULL || is_reentrant(ctx)) return ESY_ERR_STATE;
    ctx->processing = true;
    esy_err_t result = ESY_OK;
    if (ctx->current != NULL) {
        if (ctx->abort_requested || ctx->cleanup) {
            result = esy_hal_dma_abort(ctx->dma, ctx->current->transfer.inner.dreq,
                                       ctx->current->transfer.inner.channel);
            if (result == ESY_OK)
                finish_current(ctx, ctx->cleanup ? ctx->result : ESY_OK,
                                !ctx->cleanup);
        } else {
            deliver_events(ctx);
            if (ctx->terminal) finish_current(ctx, ctx->result, false);
        }
    } else {
        ctx->current = (esy_dma_scheduler_request_t *)(void *)
            esy_block_ring_get_read_block(&ctx->queue);
        if (ctx->current != NULL) {
            if (ctx->cancel_queued) finish_current(ctx, ESY_OK, true);
            else result = start_current(ctx);
        }
    }
    ctx->processing = false;
    return result;
}

esy_err_t esy_dma_scheduler_core_request_stop(esy_dma_scheduler_core_t *core,
                                             bool cancel_queued) {
    if (core == NULL) return ESY_ERR_INVALID_ARG;
    dma_scheduler_ctx_t *ctx = get_context(core);
    if (ctx == NULL || is_reentrant(ctx)) return ESY_ERR_STATE;
    ctx->stopping = true;
    ctx->cancel_queued |= cancel_queued;
    return ESY_OK;
}

esy_err_t esy_dma_scheduler_core_request_abort(esy_dma_scheduler_core_t *core) {
    esy_err_t result = esy_dma_scheduler_core_request_stop(core, true);
    if (result == ESY_OK) get_context(core)->abort_requested = true;
    return result;
}

bool esy_dma_scheduler_core_is_idle(const esy_dma_scheduler_core_t *core) {
    dma_scheduler_ctx_t *ctx = get_context(core);
    return ctx != NULL && !is_reentrant(ctx) && ctx->current == NULL &&
        esy_block_ring_empty(&ctx->queue) && esy_hal_dma_is_idle(ctx->dma);
}

uint32_t esy_dma_scheduler_core_next_wait_ms(const esy_dma_scheduler_core_t *core) {
    dma_scheduler_ctx_t *ctx = get_context(core);
    if (ctx == NULL || is_reentrant(ctx)) return UINT32_MAX;
    if (ctx->current == NULL)
        return esy_block_ring_empty(&ctx->queue) ? UINT32_MAX : 0u;
    return DMA_SCHEDULER_RETRY_MS;
}

esy_err_t esy_dma_scheduler_core_deinit(esy_dma_scheduler_core_t *core) {
    if (core == NULL) return ESY_OK;
    dma_scheduler_ctx_t *ctx = get_context(core);
    if (ctx == NULL) return ESY_OK;
    if (!esy_dma_scheduler_core_is_idle(core)) return ESY_ERR_BUSY;
    memset(core->bytes, 0, sizeof(core->bytes));
    return ESY_OK;
}
