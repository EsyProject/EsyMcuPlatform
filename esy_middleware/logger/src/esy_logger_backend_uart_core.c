/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/logger/esy_logger_backend_uart_core.h"
#include "esy_logger_backend_core_internal.h"
#include "esy_middleware/logger/esy_logger.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esy_logger_backend_uart_internal.h"
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/ds/esy_block_pool.h"
#include "esy_common/ds/esy_block_ring.h"
#include "esy_common/utils/base/esy_bits.h"
#include "esy_hal/esy_hal_uart.h"
#include "esy_hal/esy_hal_dma.h"
#include "esy_hal/esy_hal_system.h"

#define ESY_LOGGER_UART_MAX_BLOCK_SIZE 128u
#define ESY_LOGGER_UART_MIN_POOL_BLOCK_COUNT 2u
#define ESY_LOGGER_UART_MAX_POOL_BLOCK_COUNT 32u
#define ESY_LOGGER_UART_RETRY_MS 10u

/* ============ Send descriptor stored in the shared ready ring ============ */
typedef struct {
  uint32_t pool_index; /* Block index in the shared pool. */
  uint16_t len;        /* Valid payload length. */
} esy_log_send_desc_t;

_Static_assert(sizeof(esy_log_send_desc_t) <=
                   sizeof(esy_logger_backend_uart_ready_slot_t),
               "UART logger ready slot is too small");
_Static_assert(_Alignof(esy_log_send_desc_t) <=
                   _Alignof(esy_logger_backend_uart_ready_slot_t),
               "UART logger ready slot alignment is too small");

/* ============ Context ============ */
typedef struct {
  /* UART */
  esy_uart_t* uart;

  /* Shared memory pool used by all writers. */
  esy_block_pool_t pool;
  uint8_t* pool_buffers;
  uint32_t* pool_index_stacks;

  /* Shared ready ring storing esy_log_send_desc_t entries. */
  esy_block_ring_t ready_ring;
  esy_logger_backend_uart_ready_slot_t* ready_ring_slots;

  /* Sender task and DMA state shared by task/ISR, protected by crit_auto. */
  void *task;
  esy_logger_backend_runtime_t runtime;
  uint32_t last_retry_ms;
  bool retry_waiting;
  bool dma_busy;
  bool dma_cleanup_pending;
  bool io_busy;
  esy_log_send_desc_t dma_desc;

  /* Cached configuration */
  uint32_t block_size;
  uint32_t pool_block_count;
  uint32_t dma_short_threshold;
  bool initialized;
  bool deinit_requested;
  bool quiesced;
  bool worker_active;
  bool retry_pending;
  esy_err_t last_poll_result;
  uint32_t borrowed_mask;
  uint32_t active_calls;
  esy_logger_backend_t *owner;
} esy_logger_backend_uart_ctx_t;

_Static_assert(sizeof(esy_logger_backend_uart_ctx_t) <=
                   ESY_LOGGER_BACKEND_UART_CONTEXT_BYTES,
               "UART logger context storage is too small");
_Static_assert(_Alignof(esy_logger_backend_uart_ctx_t) <=
                   _Alignof(esy_logger_backend_uart_context_storage_t),
               "UART logger context storage alignment is too small");

/* Resolve the shared pool block index for a buffer; return false if invalid. */
static bool resolve_pool_index(const esy_logger_backend_uart_ctx_t* ctx,
                               const uint8_t* buf, uint32_t* out_index) {
  uint32_t index = 0u;
  if (ctx == NULL || buf == NULL || out_index == NULL) {
    return false;
  }

  if (esy_block_pool_ptr_to_index(&ctx->pool, buf, &index) != ESY_OK) {
    return false;
  }

  *out_index = index;
  return true;
}

/* ============ Forward declarations ============ */
static esy_err_t acquire_buffer(void* self, uint8_t** buf, size_t* buf_len);
static esy_err_t submit_buffer(void* self, const uint8_t* buf, size_t len);
static void dma_complete(void* arg);

/* Callback identity is checked before interpreting opaque state; owner identity
 * also rejects copied handles that refer to another instance's context. */
static esy_logger_backend_uart_ctx_t *backend_context(
    const esy_logger_backend_t *backend) {
  if (backend == NULL || backend->acquire_buffer != acquire_buffer ||
      backend->submit_buffer != submit_buffer || backend->ctx == NULL ||
      (uintptr_t)backend->ctx % _Alignof(esy_logger_backend_uart_ctx_t) != 0u) {
    return NULL;
  }
  esy_logger_backend_uart_ctx_t *ctx = backend->ctx;
  return ctx->owner == backend && ctx->initialized ? ctx : NULL;
}

static esy_err_t validate_static_resources(
    const esy_logger_backend_uart_core_resources_t* resources,
    size_t block_size, uint32_t pool_block_count) {
  if (resources == NULL || resources->context_storage == NULL ||
      (uintptr_t)resources->context_storage %
          _Alignof(esy_logger_backend_uart_context_storage_t) != 0u ||
      resources->pool_buffers == NULL ||
      resources->pool_index_stacks == NULL ||
      resources->ready_ring_slots == NULL ||
      (uintptr_t)resources->pool_index_stacks % _Alignof(uint32_t) != 0u ||
      (uintptr_t)resources->ready_ring_slots %
          _Alignof(esy_logger_backend_uart_ready_slot_t) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  if (resources->pool_buffer_size <
          ((size_t)pool_block_count * block_size) ||
      resources->pool_index_stack_count < (size_t)pool_block_count ||
      resources->ready_ring_slot_count < (size_t)pool_block_count) {
    return ESY_ERR_INVALID_ARG;
  }

  return ESY_OK;
}

/* ============ Public API ============ */

/**
 * @brief Populate the UART backend default configuration.
 *
 * @param[out] cfg Configuration object.
 */
void esy_logger_backend_uart_core_default_cfg(
    esy_logger_backend_uart_core_config_t* cfg) {
  if (cfg == NULL) {
    return;
  }

  cfg->uart = NULL;

  /* Defaults are provided here; init() does not replace zero values. */
  cfg->block_size = 128;        /* Maximum bytes per log block. */
  cfg->pool_block_count = 32;    /* Number of shared pool blocks. */
  cfg->dma_short_threshold = 6;  /* Shorter/equal messages use blocking send. */
  cfg->static_resources = NULL;
}

/**
 * @brief Initialize the UART logger backend.
 *
 * The backend uses one shared memory pool, one shared ready ring, and a sender
 * task.
 *
 * @param[out] logger_backend Backend interface to populate.
 * @param[in]  config         Configuration.
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration, such as zero values or
 *                             non-power-of-two pool depth.
 * @retval <0                  Failure.
 */
esy_err_t esy_logger_backend_uart_init_with_runtime(
    esy_logger_backend_t* logger_backend,
    const esy_logger_backend_uart_core_config_t *config,
    const esy_logger_backend_runtime_t *runtime, const void *worker_config) {
  if (logger_backend == NULL || config == NULL || config->uart == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  if (logger_backend->ctx != NULL || logger_backend->acquire_buffer != NULL ||
      logger_backend->submit_buffer != NULL) return ESY_ERR_STATE;

  /* Validate parameters without replacing zero values with defaults. */
  if (config->block_size == 0u ||
      config->pool_block_count < ESY_LOGGER_UART_MIN_POOL_BLOCK_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  if (config->block_size > ESY_LOGGER_UART_MAX_BLOCK_SIZE ||
      config->pool_block_count > ESY_LOGGER_UART_MAX_POOL_BLOCK_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!esy_is_power_of_two_u32(config->pool_block_count)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (config->dma_short_threshold > config->block_size) {
    /* The short-message threshold must not exceed block size. */
    return ESY_ERR_INVALID_ARG;
  }

  esy_err_t ret = validate_static_resources(config->static_resources,
      config->block_size, config->pool_block_count);
  if (ret != ESY_OK) return ret;
  esy_logger_backend_uart_ctx_t *ctx =
      (esy_logger_backend_uart_ctx_t *)config->static_resources->context_storage->bytes;
  /* The public contract requires zero context storage before first use.
   * Lifecycle owners serialize reuse, including init on a second handle. */
  if (ctx->owner != NULL || ctx->initialized) return ESY_ERR_STATE;
  memset(ctx, 0, sizeof(*ctx));
  ctx->runtime = *runtime;

  ctx->block_size = config->block_size;
  ctx->pool_block_count = config->pool_block_count;
#ifdef ENABLE_ESY_QEMU
  ctx->dma_short_threshold = config->block_size;
#else
  ctx->dma_short_threshold = config->dma_short_threshold;
#endif

  ctx->uart = config->uart;

  ctx->pool_buffers = config->static_resources->pool_buffers;
  ctx->pool_index_stacks = config->static_resources->pool_index_stacks;
  ctx->ready_ring_slots = config->static_resources->ready_ring_slots;

  ret = esy_block_pool_init(&ctx->pool, ctx->pool_buffers, ctx->block_size,
                            ctx->pool_block_count, ctx->pool_index_stacks);
  if (ret != ESY_OK) {
    memset(ctx, 0, sizeof(*ctx));
    return ret;
  }

  ret = esy_block_ring_init(&ctx->ready_ring, (uint8_t*)ctx->ready_ring_slots,
                            sizeof(esy_logger_backend_uart_ready_slot_t),
                            ctx->pool_block_count);
  if (ret != ESY_OK) {
    memset(ctx, 0, sizeof(*ctx));
    return ret;
  }

  ctx->dma_busy = false;

  if (runtime->start) {
    ret = runtime->start(ctx, worker_config, &ctx->task);
    if (ret != ESY_OK) {
      memset(ctx, 0, sizeof(*ctx));
      return ret;
    }
  }

  /* Output interface */
  /* Critical section reason: publish the initialized context atomically to the
   * sender task; it may already run after static task creation. */
  uint32_t key = ctx->runtime.lock();
  logger_backend->acquire_buffer = acquire_buffer;
  logger_backend->submit_buffer = submit_buffer;
  logger_backend->ctx = ctx;
  ctx->owner = logger_backend;
  ctx->initialized = true;
  ctx->runtime.unlock(key);

  return ESY_OK;
}

/* Lifecycle calls have one external owner. The frontend detach excludes an
 * entire log transaction; borrowed blocks and active calls protect raw users. */
esy_err_t esy_logger_backend_uart_deinit(esy_logger_backend_t *backend) {
  if (backend == NULL || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (backend->ctx == NULL && backend->acquire_buffer == NULL &&
      backend->submit_buffer == NULL) return ESY_OK;
  esy_logger_backend_uart_ctx_t *ctx = backend_context(backend);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  esy_err_t ret = esy_logger_detach(backend);
  if (ret != ESY_OK) return ret;
  /* Critical section reason: task/ISR completion and producers share the stop
   * state and resource leases; they must be checked as one lifecycle decision. */
  uint32_t key = ctx->runtime.lock();
  if (backend->ctx == NULL) {
    ctx->runtime.unlock(key);
    return ESY_OK;
  }
  if (backend->ctx != ctx || ctx->owner != backend) {
    ctx->runtime.unlock(key);
    return ESY_ERR_INVALID_ARG;
  }
  ctx->deinit_requested = true;
  if (ctx->worker_active || ctx->dma_busy || ctx->io_busy ||
      ctx->borrowed_mask != 0u || ctx->active_calls != 0u) {
    ctx->runtime.unlock(key);
    return ESY_ERR_BUSY;
  }
  void *task = ctx->task;
  esy_uart_t *uart = ctx->uart;
  ++ctx->active_calls;
  ctx->runtime.unlock(key);
  /* The stopped sender cannot start more I/O. Pin the context while querying
   * physical drain outside the lock: source completion need not empty a FIFO. */
  bool wire_busy = esy_hal_uart_is_busy(uart, true);
  key = ctx->runtime.lock();
  --ctx->active_calls;
  ctx->runtime.unlock(key);
  if (wire_busy) return ESY_ERR_BUSY;
  if (ctx->runtime.stop) {
    ret = ctx->runtime.stop(task);
    if (ret != ESY_OK) return ret;
  }
  memset(ctx, 0, sizeof(*ctx));
  memset(backend, 0, sizeof(*backend));
  return ESY_OK;
}

esy_err_t esy_logger_backend_uart_quiesce(esy_logger_backend_t *backend) {
  if (backend == NULL || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_uart_ctx_t *ctx = backend_context(backend);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  /* Critical section reason: control-plane changes must be serialized with
   * sender selection and task/ISR completion of in-flight I/O. */
  uint32_t key = ctx->runtime.lock();
  esy_err_t ret = ESY_OK;
  if (backend->ctx != ctx || ctx->owner != backend) ret = ESY_ERR_INVALID_ARG;
  else if (ctx->deinit_requested || ctx->quiesced) ret = ESY_ERR_STATE;
  else if (ctx->io_busy || ctx->worker_active || ctx->active_calls != 0u) ret = ESY_ERR_BUSY;
  if (ret != ESY_OK) {
    ctx->runtime.unlock(key);
    return ret;
  }
  /* Reserve quiescence before leaving the lock so the worker cannot start I/O
   * across the physical-busy sample. Producers may still enqueue normally. */
  ctx->quiesced = true;
  ++ctx->active_calls;
  esy_uart_t *uart = ctx->uart;
  void *task = ctx->task;
  ctx->runtime.unlock(key);
  bool wire_busy = esy_hal_uart_is_busy(uart, true);
  key = ctx->runtime.lock();
  if (wire_busy || ctx->active_calls != 1u) {
    ctx->quiesced = false;
    ret = ESY_ERR_BUSY;
  }
  ctx->runtime.unlock(key);
  /* A worker may have consumed a producer notification while the admission
   * gate was closed. Rewake it on rejection, retaining the call lease. */
  if (ret != ESY_OK && ctx->runtime.notify)
    (void)ctx->runtime.notify(task);
  key = ctx->runtime.lock();
  --ctx->active_calls;
  ctx->runtime.unlock(key);
  return ret;
}

esy_err_t esy_logger_backend_uart_resume(esy_logger_backend_t *backend) {
  if (backend == NULL || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_uart_ctx_t *ctx = backend_context(backend);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  /* Critical section reason: resume and sender selection share quiesced. */
  uint32_t key = ctx->runtime.lock();
  if (backend->ctx != ctx || ctx->owner != backend) {
    ctx->runtime.unlock(key);
    return ESY_ERR_INVALID_ARG;
  }
  if (!ctx->quiesced || ctx->deinit_requested) {
    ctx->runtime.unlock(key);
    return ESY_ERR_STATE;
  }
  ctx->quiesced = false;
  ctx->runtime.unlock(key);
  return ctx->runtime.notify ? ctx->runtime.notify(ctx->task) : ESY_OK;
}

static esy_err_t acquire_buffer(void *self, uint8_t **buf, size_t *buf_len) {
  if (self == NULL || buf == NULL || buf_len == NULL) return ESY_ERR_INVALID_ARG;
  *buf = NULL;
  *buf_len = 0u;
  esy_logger_backend_uart_ctx_t *ctx = backend_context(self);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  /* Critical section reason: MPSC producers and the consumer share a non-thread-
   * safe pool and its borrowed-block bitmap; allocation and lease are atomic. */
  uint32_t key = ctx->runtime.lock();
  if (ctx->owner != self || !ctx->initialized || ctx->deinit_requested) {
    ctx->runtime.unlock(key);
    return ESY_ERR_STATE;
  }
  uint8_t *block = esy_block_pool_acquire(&ctx->pool);
  if (block == NULL) {
    ctx->runtime.unlock(key);
    return ESY_ERR_NO_MEM;
  }
  uint32_t index = 0u;
  (void)resolve_pool_index(ctx, block, &index);
  ctx->borrowed_mask |= UINT32_C(1) << index;
  *buf = block;
  *buf_len = ctx->block_size;
  ctx->runtime.unlock(key);
  return ESY_OK;
}

static esy_err_t submit_buffer(void *self, const uint8_t *buf, size_t len) {
  if (self == NULL || buf == NULL) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_uart_ctx_t *ctx = backend_context(self);
  if (ctx == NULL) return ESY_ERR_INVALID_ARG;
  /* Critical section reason: MPSC submit and lifecycle share block ownership,
   * pool and ring updates; an outstanding notification also holds a lease. */
  uint32_t key = ctx->runtime.lock();
  uint32_t index;
  if (ctx->owner != self || !ctx->initialized ||
      !resolve_pool_index(ctx, buf, &index) ||
      (ctx->borrowed_mask & (UINT32_C(1) << index)) == 0u) {
    ctx->runtime.unlock(key);
    return ESY_ERR_INVALID_ARG;
  }
  ctx->borrowed_mask &= ~(UINT32_C(1) << index);
  esy_log_send_desc_t *desc = NULL;
  esy_err_t ret = ESY_OK;
  if (ctx->deinit_requested) ret = ESY_ERR_STATE;
  else if (len > ctx->block_size) ret = ESY_ERR_INVALID_ARG;
  else if (len != 0u) {
    desc = (esy_log_send_desc_t *)esy_block_ring_get_write_block(&ctx->ready_ring);
    if (desc == NULL) ret = ESY_ERR_NO_MEM;
  }
  if (desc == NULL) {
    esy_block_pool_release(&ctx->pool, (uint8_t *)buf);
    ctx->runtime.unlock(key);
    return ret;
  }
  desc->pool_index = index;
  desc->len = (uint16_t)len;
  esy_block_ring_commit_write(&ctx->ready_ring);
  ++ctx->active_calls;
  void *task = ctx->task;
  ctx->runtime.unlock(key);
  if (ctx->runtime.notify) (void)ctx->runtime.notify(task);
  key = ctx->runtime.lock();
  --ctx->active_calls;
  ctx->runtime.unlock(key);
  return ESY_OK;
}

static bool try_kick_one(esy_logger_backend_uart_ctx_t *ctx) {
  /* Critical section reason: the sender, lifecycle owner and completion callback
   * must reserve one descriptor and its I/O lease as an indivisible operation. */
  uint32_t key = ctx->runtime.lock();
  ctx->last_poll_result = ESY_OK;
  if (!ctx->initialized || ctx->worker_active) {
    ctx->runtime.unlock(key);
    return false;
  }
  if (ctx->dma_busy) {
    /* Pin the context while owner-side retirement may invoke dma_complete. */
    bool cleanup_pending = ctx->dma_cleanup_pending;
    ctx->worker_active = true;
    ctx->runtime.unlock(key);
    esy_err_t result = esy_hal_uart_poll(ctx->uart);
    if (cleanup_pending && esy_hal_uart_is_busy(ctx->uart, true)) {
      result = esy_hal_uart_abort_dma(ctx->uart, true);
      /* No DMA borrow remains in STATE, but physical TX may still be draining. */
      if (result == ESY_ERR_STATE) result = ESY_OK;
    }
    if (cleanup_pending && !esy_hal_uart_is_busy(ctx->uart, true)) {
      dma_complete(ctx);
      result = ESY_OK;
    }
    key = ctx->runtime.lock();
    ctx->worker_active = false;
    ctx->last_poll_result = result;
    ctx->runtime.unlock(key);
    /* End this slice after one retirement/abort attempt, even with a large
     * caller budget or an OS worker's drain loop. */
    return false;
  }
  if (!ctx->initialized || ctx->deinit_requested || ctx->quiesced ||
      ctx->worker_active || ctx->dma_busy) {
    ctx->runtime.unlock(key);
    return false;
  }
  ctx->retry_pending = false;
  esy_log_send_desc_t *desc =
      (esy_log_send_desc_t *)esy_block_ring_get_read_block(&ctx->ready_ring);
  if (desc == NULL) {
    ctx->runtime.unlock(key);
    return false;
  }
  uint8_t *payload = NULL;
  if (esy_block_pool_index_to_ptr(&ctx->pool, desc->pool_index, &payload) != ESY_OK) {
    esy_block_ring_commit_read(&ctx->ready_ring);
    ctx->runtime.unlock(key);
    return true;
  }
  ctx->worker_active = true;
  ctx->io_busy = true;
  size_t length = desc->len;
  if (length <= ctx->dma_short_threshold) {
    ctx->runtime.unlock(key);
    int32_t sent = esy_hal_uart_send_blocking(ctx->uart, payload, length);
    bool retry = sent == ESY_ERR_BUSY || sent == ESY_ERR_STATE ||
                 (sent >= 0 && (size_t)sent < length);
    if (sent > 0 && (size_t)sent < length) {
      memmove(payload, payload + sent, length - (size_t)sent);
    }
    key = ctx->runtime.lock();
    if (retry) {
      if (sent > 0) desc->len = (uint16_t)(length - (size_t)sent);
    } else {
      esy_block_ring_commit_read(&ctx->ready_ring);
      esy_block_pool_release(&ctx->pool, payload);
    }
    ctx->retry_pending = retry;
    ctx->io_busy = false;
    ctx->worker_active = false;
    ctx->runtime.unlock(key);
    return !retry;
  }
  ctx->dma_busy = true;
  ctx->dma_desc = *desc;
  ctx->runtime.unlock(key);

  esy_dma_config_t dma;
  esy_hal_dma_default_cfg(&dma);
  dma.src = payload;
  dma.length = length;
  dma.transfer_size = ESY_DMA_TRANSFER_SIZE_8;
  dma.callback = dma_complete;
  dma.callback_arg = ctx;
  dma.error_callback = dma_complete;
  dma.error_callback_arg = ctx;
  esy_err_t ret = esy_hal_uart_send_dma(ctx->uart, &dma);
  bool retained = ret != ESY_OK && esy_hal_uart_is_busy(ctx->uart, true);

  key = ctx->runtime.lock();
  bool retry = ret == ESY_ERR_BUSY || ret == ESY_ERR_STATE;
  if (ret != ESY_OK && ctx->dma_busy) {
    if (retained) {
      ctx->dma_cleanup_pending = true;
      retry = false;
    } else {
      ctx->dma_busy = false;
      ctx->io_busy = false;
      if (!retry) {
        esy_block_ring_commit_read(&ctx->ready_ring);
        esy_block_pool_release(&ctx->pool, payload);
      }
    }
  }
  ctx->retry_pending = retry;
  ctx->worker_active = false;
  ctx->runtime.unlock(key);
  return !retry && !retained;
}

/* Completion and transfer error share ownership cleanup. The HAL may call
 * synchronously in task context or asynchronously from an ISR. */
static void dma_complete(void *arg) {
  esy_logger_backend_uart_ctx_t *ctx = arg;
  /* Critical section reason: completion, sender and lifecycle share descriptor,
   * pool and callback leases. Keep the lease until the notification has returned. */
  uint32_t key = ctx->runtime.lock();
  if (!ctx->dma_busy) {
    ctx->runtime.unlock(key);
    return;
  }
  ++ctx->active_calls;
  uint8_t *payload = NULL;
  (void)esy_block_pool_index_to_ptr(&ctx->pool, ctx->dma_desc.pool_index, &payload);
  esy_block_ring_commit_read(&ctx->ready_ring);
  esy_block_pool_release(&ctx->pool, payload);
  ctx->dma_busy = false;
  ctx->dma_cleanup_pending = false;
  ctx->io_busy = false;
  void *task = ctx->task;
  ctx->runtime.unlock(key);
  if (ctx->runtime.notify) (void)ctx->runtime.notify(task);
  key = ctx->runtime.lock();
  --ctx->active_calls;
  ctx->runtime.unlock(key);
}

ESY_INTERNAL bool esy_logger_backend_uart_internal_try_kick_one(
    const esy_logger_backend_t *backend) {
  esy_logger_backend_uart_ctx_t *ctx = backend_context(backend);
  return ctx != NULL && try_kick_one(ctx);
}

ESY_INTERNAL bool esy_logger_backend_uart_internal_is_initialized(
    const esy_logger_backend_t *backend) {
  return backend_context(backend) != NULL;
}

ESY_INTERNAL bool esy_logger_backend_uart_internal_is_deinit_requested(
    const esy_logger_backend_t *backend) {
  const esy_logger_backend_uart_ctx_t *ctx = backend_context(backend);
  return ctx != NULL && ctx->deinit_requested;
}

ESY_INTERNAL bool esy_logger_backend_uart_internal_is_dma_busy(
    const esy_logger_backend_t *backend) {
  const esy_logger_backend_uart_ctx_t *ctx = backend_context(backend);
  return ctx != NULL && ctx->dma_busy;
}

bool esy_logger_backend_uart_run_one(void *context) {
  return try_kick_one(context);
}

esy_err_t esy_logger_backend_uart_core_init(esy_logger_backend_t *backend,
    const esy_logger_backend_uart_core_config_t *config) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_runtime_t runtime = esy_logger_poll_runtime();
  return esy_logger_backend_uart_init_with_runtime(backend, config, &runtime, NULL);
}

esy_err_t esy_logger_backend_uart_core_process(esy_logger_backend_t *backend, size_t budget) {
  if (!backend || budget == 0u || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_uart_ctx_t *ctx = backend_context(backend);
  if (ctx == NULL || ctx->task || ctx->worker_active) return ESY_ERR_STATE;
  if (ctx->retry_waiting && !ctx->dma_busy &&
      (uint32_t)(esy_hal_system_get_tick() - ctx->last_retry_ms) <
          ESY_LOGGER_UART_RETRY_MS) return ESY_OK;
  for (size_t i = 0u; i < budget && try_kick_one(ctx); ++i) {}
  ctx->retry_waiting = ctx->retry_pending;
  if (ctx->retry_waiting) ctx->last_retry_ms = esy_hal_system_get_tick();
  return ctx->last_poll_result;
}

bool esy_logger_backend_uart_retry_pending(void *context) {
  esy_logger_backend_uart_ctx_t *ctx = context;
  uint32_t key = ctx->runtime.lock();
  bool retry = ctx->dma_busy ||
      (ctx->retry_pending && !ctx->quiesced && !ctx->deinit_requested);
  ctx->runtime.unlock(key);
  return retry;
}

/* Critical section reason: the lifecycle owner publishes stop concurrently
 * with worker wakeups; the worker must acknowledge it before storage reuse. */
bool esy_logger_backend_uart_stop_requested(void *context) {
  esy_logger_backend_uart_ctx_t *ctx = context;
  uint32_t key = ctx->runtime.lock();
  bool stop = ctx->deinit_requested && !ctx->dma_busy && !ctx->worker_active;
  ctx->runtime.unlock(key);
  return stop;
}
