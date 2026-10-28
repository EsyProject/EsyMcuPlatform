/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/logger/esy_logger_backend_printf_core.h"
#include "esy_logger_backend_core_internal.h"
#include "esy_middleware/logger/esy_logger.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/ds/esy_block_pool.h"
#include "esy_common/ds/esy_block_ring.h"
#include "esy_common/utils/base/esy_bits.h"
#include "esy_hal/esy_hal_system.h"

#define ESY_LOGGER_PRINTF_MAX_BLOCK_SIZE 128u
#define ESY_LOGGER_PRINTF_MIN_POOL_BLOCK_COUNT 2u
#define ESY_LOGGER_PRINTF_MAX_POOL_BLOCK_COUNT 8u

/* ============ Send descriptor stored in each per-core ready ring ============ */
typedef struct {
  uint32_t pool_index; /* Block index in the owning core pool. */
  uint16_t len;        /* Valid payload length. */
  uint16_t sequence;   /* Global FIFO order across per-core rings. */
} esy_log_send_desc_t;

_Static_assert(sizeof(esy_log_send_desc_t) <=
                   sizeof(esy_logger_backend_printf_ready_slot_t),
               "Printf logger ready slot is too small");
_Static_assert(_Alignof(esy_log_send_desc_t) <=
                   _Alignof(esy_logger_backend_printf_ready_slot_t),
               "Printf logger ready slot alignment is too small");

/* ============ Context ============ */
typedef struct {
  /* Core topology */
  uint8_t core_count;

  /* Per-core memory pools used by writers running on that core. */
  esy_block_pool_t pools[ESY_MAX_CORE_COUNT];
  uint8_t *pool_buffers;
  uint32_t *pool_index_stacks;

  /* Per-core ready rings storing esy_log_send_desc_t entries. */
  esy_block_ring_t ready_rings[ESY_MAX_CORE_COUNT];
  esy_logger_backend_printf_ready_slot_t *ready_ring_slots;

  /* Print task state */
  void *task;
  esy_logger_backend_runtime_t runtime;
  uint16_t next_sequence;
  uint32_t borrowed_mask[ESY_MAX_CORE_COUNT];
  uint32_t active_calls;
  bool worker_active;
  bool stopping;
  esy_logger_backend_t *owner;

  /* Cached configuration */
  uint32_t block_size;
  uint32_t pool_block_count;
  bool initialized;
} esy_logger_backend_printf_ctx_t;

static esy_logger_backend_printf_ctx_t s_ctx;

/* Resolve the core pool and block index for a buffer; return false if invalid. */
static bool resolve_pool_core_and_index(
    const esy_logger_backend_printf_ctx_t *ctx, const uint8_t *buf,
    uint8_t *out_core, uint32_t *out_index) {
  for (uint8_t c = 0; c < ctx->core_count; ++c) {
    const uint8_t *base =
        &ctx->pool_buffers[(size_t)c * (size_t)ctx->pool_block_count *
                           (size_t)ctx->block_size];
    uint32_t block_size = ctx->block_size;
    uint32_t block_count = ctx->pool_block_count;

    uintptr_t start = (uintptr_t)base;
    uintptr_t end = start + (uintptr_t)block_size * (uintptr_t)block_count;
    uintptr_t p = (uintptr_t)buf;

    if (p >= start && p < end) {
      uintptr_t off = p - start;
      if ((off % block_size) != 0u) {
        return false; /* Unaligned pointer, therefore invalid. */
      }
      *out_core = c;
      *out_index = (uint32_t)(off / block_size);
      return true;
    }
  }
  return false;
}

/* ============ Forward declarations ============ */
static esy_err_t acquire_buffer(void *self, uint8_t **buf, size_t *buf_len);
static esy_err_t submit_buffer(void *self, const uint8_t *buf, size_t len);

static esy_err_t validate_static_resources(
    const esy_logger_backend_printf_core_resources_t *resources,
    uint8_t core_count, size_t block_size, uint32_t pool_block_count) {
  if (resources == NULL || resources->pool_buffers == NULL ||
      resources->pool_index_stacks == NULL ||
      resources->ready_ring_slots == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  size_t total_blocks = (size_t)core_count * (size_t)pool_block_count;
  if (resources->pool_buffer_size < (total_blocks * block_size) ||
      resources->pool_index_stack_count < total_blocks ||
      resources->ready_ring_slot_count < total_blocks) {
    return ESY_ERR_INVALID_ARG;
  }

  return ESY_OK;
}

/* ============ Public API ============ */

void esy_logger_backend_printf_core_default_cfg(
    esy_logger_backend_printf_core_config_t *cfg) {
  if (!cfg) return;

  /* Defaults are provided here; init() does not replace zero values. */
  cfg->block_size = 128;        /* Maximum bytes per log block. */
  cfg->pool_block_count = 8;    /* Number of pool blocks per core. */
  cfg->static_resources = NULL;
}

/**
 * @brief Initialize the printf logger backend.
 *
 * The backend uses per-core memory pools, per-core ready rings, and an
 * aggregation task.
 *
 * @param[out] logger_backend Backend interface to populate.
 * @param[in]  config         Configuration.
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration, such as zero values or
 *                             non-power-of-two pool depth.
 * @retval <0                  Failure.
 */
esy_err_t esy_logger_backend_printf_init_with_runtime(
    esy_logger_backend_t *logger_backend,
    const esy_logger_backend_printf_core_config_t *config,
    const esy_logger_backend_runtime_t *runtime, const void *worker_config) {
  if (!logger_backend || !config) return ESY_ERR_INVALID_ARG;

  if (logger_backend->ctx != NULL || logger_backend->acquire_buffer != NULL ||
      logger_backend->submit_buffer != NULL) return ESY_ERR_STATE;

  /* Validate parameters without replacing zero values with defaults. */
  if (config->block_size == 0u ||
      config->pool_block_count < ESY_LOGGER_PRINTF_MIN_POOL_BLOCK_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  if (config->block_size > ESY_LOGGER_PRINTF_MAX_BLOCK_SIZE ||
      config->pool_block_count > ESY_LOGGER_PRINTF_MAX_POOL_BLOCK_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!esy_is_power_of_two_u32(config->pool_block_count)) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_logger_backend_printf_ctx_t *ctx = &s_ctx;
  if (ctx->initialized) {
    return ESY_ERR_STATE;
  }
  memset(ctx, 0, sizeof(*ctx));
  ctx->runtime = *runtime;

  ctx->block_size = (uint32_t)config->block_size;
  ctx->pool_block_count = config->pool_block_count;

  /* Runtime core count */
  ctx->core_count = runtime->start ? esy_hal_system_get_core_count() : 1u;
  if (ctx->core_count == 0) ctx->core_count = 1;
  if (ctx->core_count > ESY_MAX_CORE_COUNT) {
    memset(ctx, 0, sizeof(*ctx));
    return ESY_ERR_INVALID_ARG;
  }

  esy_err_t ret =
      validate_static_resources(config->static_resources, ctx->core_count,
                                ctx->block_size, ctx->pool_block_count);
  if (ret != ESY_OK) {
    memset(ctx, 0, sizeof(*ctx));
    return ret;
  }

  ctx->pool_buffers = config->static_resources->pool_buffers;
  ctx->pool_index_stacks = config->static_resources->pool_index_stacks;
  ctx->ready_ring_slots = config->static_resources->ready_ring_slots;

  /* Initialize per-core resources. */
  for (uint8_t c = 0; c < ctx->core_count; ++c) {
    uint8_t *pool_buffer =
        &ctx->pool_buffers[(size_t)c * (size_t)ctx->pool_block_count *
                           (size_t)ctx->block_size];
    uint32_t *pool_index_stack =
        &ctx->pool_index_stacks[(size_t)c * (size_t)ctx->pool_block_count];
    esy_logger_backend_printf_ready_slot_t *ready_ring_slots =
        &ctx->ready_ring_slots[(size_t)c * (size_t)ctx->pool_block_count];

    /* Pool init */
    ret = esy_block_pool_init(&ctx->pools[c], pool_buffer, ctx->block_size,
                              ctx->pool_block_count, pool_index_stack);
    if (ret != ESY_OK) {
      memset(ctx, 0, sizeof(*ctx));
      return ret;
    }

    /* Ready ring init */
    ret = esy_block_ring_init(
        &ctx->ready_rings[c], (uint8_t *)ready_ring_slots,
        sizeof(esy_logger_backend_printf_ready_slot_t),
        ctx->pool_block_count);
    if (ret != ESY_OK) {
      memset(ctx, 0, sizeof(*ctx));
      return ret;
    }
  }

  ctx->next_sequence = 0u;

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

esy_err_t esy_logger_backend_printf_deinit(esy_logger_backend_t *backend) {
  if (backend == NULL || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_err_t ret = esy_logger_detach(backend);
  if (ret != ESY_OK) return ret;
  esy_logger_backend_printf_ctx_t *ctx = &s_ctx;
  if (backend->ctx == NULL) return ESY_OK;
  if (backend->ctx != ctx || ctx->owner != backend) return ESY_ERR_INVALID_ARG;
  /* Critical section reason: MPSC producers and output task share the stopping
   * state and leases; clearing storage is allowed only when all users finish. */
  uint32_t key = ctx->runtime.lock();
  if (backend->ctx == NULL) {
    ctx->runtime.unlock(key);
    return ESY_OK;
  }
  if (backend->ctx != ctx || ctx->owner != backend) {
    ctx->runtime.unlock(key);
    return ESY_ERR_INVALID_ARG;
  }
  ctx->stopping = true;
  bool borrowed = false;
  for (uint8_t c = 0u; c < ctx->core_count; ++c) borrowed |= ctx->borrowed_mask[c] != 0u;
  if (ctx->worker_active || ctx->active_calls != 0u || borrowed) {
    ctx->runtime.unlock(key);
    return ESY_ERR_BUSY;
  }
  void *task = ctx->task;
  ctx->runtime.unlock(key);
  if (ctx->runtime.stop) {
    ret = ctx->runtime.stop(task);
    if (ret != ESY_OK) return ret;
  }
  memset(ctx, 0, sizeof(*ctx));
  memset(backend, 0, sizeof(*backend));
  return ESY_OK;
}

static esy_err_t acquire_buffer(void *self, uint8_t **buf, size_t *buf_len) {
  if (self == NULL || buf == NULL || buf_len == NULL) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_printf_ctx_t *ctx = &s_ctx;
  *buf = NULL;
  *buf_len = 0u;
  /* Critical section reason: MPSC allocation and consumer recycling share a
   * non-thread-safe pool and borrowed bitmap; update them as one operation. */
  uint32_t key = ctx->runtime.lock();
  if (ctx->owner != self || !ctx->initialized || ctx->stopping) {
    ctx->runtime.unlock(key);
    return ESY_ERR_STATE;
  }
  uint8_t core = esy_hal_system_get_core_id();
  if (core >= ctx->core_count) core = (uint8_t)(core % ctx->core_count);
  uint8_t *block = esy_block_pool_acquire(&ctx->pools[core]);
  if (block == NULL) {
    ctx->runtime.unlock(key);
    return ESY_ERR_NO_MEM;
  }
  uint32_t index = 0u;
  (void)esy_block_pool_ptr_to_index(&ctx->pools[core], block, &index);
  ctx->borrowed_mask[core] |= UINT32_C(1) << index;
  *buf = block;
  *buf_len = ctx->block_size;
  ctx->runtime.unlock(key);
  return ESY_OK;
}

static esy_err_t submit_buffer(void *self, const uint8_t *buf, size_t len) {
  if (self == NULL || buf == NULL) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_printf_ctx_t *ctx = &s_ctx;
  /* Critical section reason: MPSC producers publish sequence, descriptor and
   * ownership together; the notification lease prevents concurrent deletion. */
  uint32_t key = ctx->runtime.lock();
  uint8_t core;
  uint32_t index;
  if (ctx->owner != self || !ctx->initialized ||
      !resolve_pool_core_and_index(ctx, buf, &core, &index) ||
      (ctx->borrowed_mask[core] & (UINT32_C(1) << index)) == 0u) {
    ctx->runtime.unlock(key);
    return ESY_ERR_INVALID_ARG;
  }
  ctx->borrowed_mask[core] &= ~(UINT32_C(1) << index);
  esy_err_t ret = ESY_OK;
  esy_log_send_desc_t *desc = NULL;
  if (ctx->stopping) ret = ESY_ERR_STATE;
  else if (len > ctx->block_size) ret = ESY_ERR_INVALID_ARG;
  else if (len != 0u) {
    desc = (esy_log_send_desc_t *)esy_block_ring_get_write_block(&ctx->ready_rings[core]);
    if (desc == NULL) ret = ESY_ERR_NO_MEM;
  }
  if (desc == NULL) {
    esy_block_pool_release(&ctx->pools[core], (uint8_t *)buf);
    ctx->runtime.unlock(key);
    return ret;
  }
  desc->pool_index = index;
  desc->len = (uint16_t)len;
  desc->sequence = ctx->next_sequence++;
  esy_block_ring_commit_write(&ctx->ready_rings[core]);
  ++ctx->active_calls;
  void *task = ctx->task;
  ctx->runtime.unlock(key);
  if (ctx->runtime.notify) (void)ctx->runtime.notify(task);
  key = ctx->runtime.lock();
  --ctx->active_calls;
  ctx->runtime.unlock(key);
  return ESY_OK;
}

static bool print_one(esy_logger_backend_printf_ctx_t *ctx) {
  /* Critical section reason: selection of the oldest published descriptor must
   * be atomic with MPSC publication and lifecycle lease acquisition. */
  uint32_t key = ctx->runtime.lock();
  if (!ctx->initialized || ctx->stopping || ctx->worker_active) {
    ctx->runtime.unlock(key);
    return false;
  }
  esy_log_send_desc_t *oldest = NULL;
  uint8_t core = 0u;
  for (uint8_t c = 0u; c < ctx->core_count; ++c) {
    esy_log_send_desc_t *candidate =
        (esy_log_send_desc_t *)esy_block_ring_get_read_block(&ctx->ready_rings[c]);
    /* At most 255 cores * 8 blocks are outstanding, below half the sequence
     * space. Unsigned subtraction therefore orders descriptors across wrap. */
    if (candidate != NULL &&
        (oldest == NULL || (uint16_t)(oldest->sequence - candidate->sequence) < UINT16_MAX / 2u)) {
      oldest = candidate;
      core = c;
    }
  }
  if (oldest == NULL) {
    ctx->runtime.unlock(key);
    return false;
  }
  uint8_t *payload = NULL;
  if (esy_block_pool_index_to_ptr(&ctx->pools[core], oldest->pool_index, &payload) != ESY_OK) {
    esy_block_ring_commit_read(&ctx->ready_rings[core]);
    ctx->runtime.unlock(key);
    return true;
  }
  size_t length = oldest->len;
  ctx->worker_active = true;
  ctx->runtime.unlock(key);
  (void)fwrite(payload, 1u, length, stdout);
  (void)fflush(stdout);
  key = ctx->runtime.lock();
  esy_block_ring_commit_read(&ctx->ready_rings[core]);
  esy_block_pool_release(&ctx->pools[core], payload);
  ctx->worker_active = false;
  ctx->runtime.unlock(key);
  return true;
}

bool esy_logger_backend_printf_run_one(void *context) {
  return print_one(context);
}

esy_err_t esy_logger_backend_printf_core_init(esy_logger_backend_t *backend,
    const esy_logger_backend_printf_core_config_t *config) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_runtime_t runtime = esy_logger_poll_runtime();
  return esy_logger_backend_printf_init_with_runtime(backend, config, &runtime, NULL);
}

esy_err_t esy_logger_backend_printf_core_process(esy_logger_backend_t *backend, size_t budget) {
  if (!backend || budget == 0u || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_printf_ctx_t *ctx = &s_ctx;
  if (backend->ctx != ctx || ctx->owner != backend || !ctx->initialized || ctx->task ||
      ctx->worker_active || ctx->stopping) return ESY_ERR_STATE;
  for (size_t i = 0u; i < budget && print_one(ctx); ++i) {}
  return ESY_OK;
}

/* Critical section reason: the lifecycle owner publishes stop concurrently
 * with worker wakeups; the worker must acknowledge it before storage reuse. */
bool esy_logger_backend_printf_stop_requested(void *context) {
  esy_logger_backend_printf_ctx_t *ctx = context;
  uint32_t key = ctx->runtime.lock();
  bool stop = ctx->stopping;
  ctx->runtime.unlock(key);
  return stop;
}
