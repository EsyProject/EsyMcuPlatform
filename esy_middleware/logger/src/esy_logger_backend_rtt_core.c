/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/logger/esy_logger_backend_rtt.h"
#include "esy_middleware/logger/esy_logger.h"

#include <stdbool.h>
#include <string.h>

#include <SEGGER_RTT.h>

#include "esy_logger_backend_runtime_internal.h"
#include "esy_logger_backend_rtt_internal.h"
#include "esy_hal/esy_hal_system.h"

#define ESY_LOGGER_RTT_MAX_BUFFER_SIZE 256u

typedef struct {
  esy_logger_backend_runtime_t runtime;
  uint8_t *buffer;
  size_t buffer_size;
  uint32_t up_buffer_index;
  bool initialized;
  bool acquired;
  bool stopping;
  esy_logger_backend_t *owner;
} esy_logger_backend_rtt_ctx_t;

static esy_logger_backend_rtt_ctx_t s_ctx;

static esy_err_t acquire_buffer(void *self, uint8_t **buf, size_t *buf_len);
static esy_err_t submit_buffer(void *self, const uint8_t *buf, size_t len);

static esy_err_t validate_static_resources(
    const esy_logger_backend_rtt_static_resources_t *resources) {
  if (resources == NULL || resources->buffer == NULL ||
      resources->buffer_size == 0u ||
      resources->buffer_size > ESY_LOGGER_RTT_MAX_BUFFER_SIZE) {
    return ESY_ERR_INVALID_ARG;
  }

  return ESY_OK;
}

void esy_logger_backend_rtt_default_cfg(
    esy_logger_backend_rtt_config_t *cfg) {
  if (cfg == NULL) {
    return;
  }

  cfg->up_buffer_index = 0u;
  cfg->static_resources = NULL;
}

esy_err_t esy_logger_backend_rtt_init_with_runtime(
    esy_logger_backend_t *logger_backend,
    const esy_logger_backend_rtt_config_t *config,
    const esy_logger_backend_runtime_t *runtime) {
  if (logger_backend == NULL || config == NULL) {
    return ESY_ERR_INVALID_ARG;
  }

  if (logger_backend->ctx != NULL || logger_backend->acquire_buffer != NULL ||
      logger_backend->submit_buffer != NULL) return ESY_ERR_STATE;

  esy_err_t ret = validate_static_resources(config->static_resources);
  if (ret != ESY_OK) {
    return ret;
  }

  esy_logger_backend_rtt_ctx_t *ctx = &s_ctx;
  if (ctx->initialized) {
    return ESY_ERR_STATE;
  }
  memset(ctx, 0, sizeof(*ctx));
  ctx->runtime = *runtime;

  ctx->buffer = config->static_resources->buffer;
  ctx->buffer_size = config->static_resources->buffer_size;
  ctx->up_buffer_index = config->up_buffer_index;

  /* Critical section reason: publish the scratch-buffer state and owner together
   * before producers can acquire the shared buffer. */
  uint32_t key = ctx->runtime.lock();
  logger_backend->acquire_buffer = acquire_buffer;
  logger_backend->submit_buffer = submit_buffer;
  logger_backend->ctx = ctx;
  ctx->owner = logger_backend;
  ctx->initialized = true;
  ctx->runtime.unlock(key);

  return ESY_OK;
}

esy_err_t esy_logger_backend_rtt_deinit(esy_logger_backend_t *backend) {
  if (backend == NULL || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_err_t ret = esy_logger_detach(backend);
  if (ret != ESY_OK) return ret;
  esy_logger_backend_rtt_ctx_t *ctx = &s_ctx;
  if (backend->ctx == NULL) return ESY_OK;
  if (backend->ctx != ctx || ctx->owner != backend) return ESY_ERR_INVALID_ARG;
  /* Critical section reason: lifecycle and producers share the scratch-buffer
   * lease; no buffer/context may be cleared while formatting or writing. */
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
  if (ctx->acquired) {
    ctx->runtime.unlock(key);
    return ESY_ERR_BUSY;
  }
  void (*unlock)(uint32_t) = ctx->runtime.unlock;
  memset(ctx, 0, sizeof(*ctx));
  memset(backend, 0, sizeof(*backend));
  unlock(key);
  return ESY_OK;
}

static esy_err_t acquire_buffer(void *self, uint8_t **buf, size_t *buf_len) {
  if (self == NULL || buf == NULL || buf_len == NULL) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_rtt_ctx_t *ctx = &s_ctx;
  *buf = NULL;
  *buf_len = 0u;
  /* Critical section reason: MPSC producers and the lifecycle owner share a
   * single scratch-buffer lease and stopping state. */
  uint32_t key = ctx->runtime.lock();
  if (ctx->owner != self || !ctx->initialized || ctx->stopping) {
    ctx->runtime.unlock(key);
    return ESY_ERR_STATE;
  }
  if (ctx->acquired) {
    ctx->runtime.unlock(key);
    return ESY_ERR_BUSY;
  }
  ctx->acquired = true;
  *buf = ctx->buffer;
  *buf_len = ctx->buffer_size;
  ctx->runtime.unlock(key);
  return ESY_OK;
}

static esy_err_t submit_buffer(void *self, const uint8_t *buf, size_t len) {
  if (self == NULL || buf == NULL) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_rtt_ctx_t *ctx = &s_ctx;
  /* Critical section reason: validate the lease and capture the stop decision
   * together; acquired remains set throughout the provider call. */
  uint32_t key = ctx->runtime.lock();
  if (ctx->owner != self || !ctx->initialized || !ctx->acquired || buf != ctx->buffer) {
    ctx->runtime.unlock(key);
    return ESY_ERR_INVALID_ARG;
  }
  esy_err_t ret = ctx->stopping ? ESY_ERR_STATE : ESY_OK;
  if (len > ctx->buffer_size) ret = ESY_ERR_INVALID_ARG;
  ctx->runtime.unlock(key);
  if (ret == ESY_OK && len != 0u) {
    unsigned written = SEGGER_RTT_Write((unsigned)ctx->up_buffer_index, buf, (unsigned)len);
    if (written != (unsigned)len) ret = ESY_ERR_IO;
  }
  key = ctx->runtime.lock();
  ctx->acquired = false;
  ctx->runtime.unlock(key);
  return ret;
}

esy_err_t esy_logger_backend_rtt_core_init(esy_logger_backend_t *backend,
    const esy_logger_backend_rtt_config_t *config) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_runtime_t runtime = esy_logger_poll_runtime();
  return esy_logger_backend_rtt_init_with_runtime(backend, config, &runtime);
}
