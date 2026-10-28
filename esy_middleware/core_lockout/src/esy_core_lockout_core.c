/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_core_lockout_core_internal.h"
#include "esy_hal/esy_hal_system.h"

static esy_core_lockout_core_ctx_t s_ctx;

ESY_INTERNAL esy_err_t esy_core_lockout_core_execute(esy_core_lockout_core_ctx_t *ctx,
    esy_core_lockout_operation_t operation, void *arg) {
  if (!ctx || !operation) return ESY_ERR_INVALID_ARG;
  if (!ctx->initialized || ctx->executing || ctx->guard_release_failed ||
      esy_hal_system_is_in_isr() ||
      (!ctx->peer_ops && esy_hal_system_get_core_id() != ctx->owner_core))
    return ESY_ERR_STATE;
  ctx->executing = true;
  esy_err_t ret = esy_activity_guard_acquire(ctx->activity_guard);
  if (ret != ESY_OK) { ctx->executing = false; return ret; }
  uint32_t other_core = UINT32_MAX;
  ret = ctx->peer_ops ? ctx->peer_ops->lock(ctx->peer_ctx, &other_core) : ESY_OK;
  if (ret == ESY_OK) {
    uint32_t irq_status = esy_hal_system_irq_save();
    operation(arg);
    esy_hal_system_irq_restore(irq_status);
    if (ctx->peer_ops) ret = ctx->peer_ops->release(ctx->peer_ctx, other_core);
  }
  /* A release fault overrides coordination errors: the operation may have run.
   * The generic guard does not promise that another release is safe. */
  if (esy_activity_guard_release(ctx->activity_guard) != ESY_OK) {
    ctx->guard_release_failed = true;
    ret = ESY_ERR_STATE;
  }
  ctx->executing = false;
  return ret;
}

esy_err_t esy_core_lockout_execute(esy_core_lockout_operation_t operation, void *arg) {
  return esy_core_lockout_core_execute(&s_ctx, operation, arg);
}

esy_err_t esy_core_lockout_set_activity_guard(const esy_activity_guard_t *guard) {
  if (s_ctx.initialized || s_ctx.executing || s_ctx.guard_release_failed ||
      esy_hal_system_is_in_isr()) return ESY_ERR_STATE;
  if (guard && (!guard->ctx || !guard->acquire || !guard->release))
    return ESY_ERR_INVALID_ARG;
  s_ctx.activity_guard = guard;
  return ESY_OK;
}

esy_err_t esy_core_lockout_initialize(const esy_core_lockout_peer_ops_t *peer_ops,
    void *peer_ctx) {
  if (esy_hal_system_is_in_isr() || s_ctx.executing || s_ctx.guard_release_failed)
    return ESY_ERR_STATE;
  if (peer_ops && (!peer_ctx || !peer_ops->prepare || !peer_ops->lock || !peer_ops->release))
    return ESY_ERR_INVALID_ARG;
  if (s_ctx.mode_selected && (s_ctx.peer_ops != peer_ops || s_ctx.peer_ctx != peer_ctx))
    return ESY_ERR_STATE;
  uint8_t owner_core = esy_hal_system_get_core_id();
  if (!peer_ops && (!esy_hal_system_get_core_count() ||
      owner_core >= esy_hal_system_get_core_count() ||
      (s_ctx.mode_selected && owner_core != s_ctx.owner_core))) return ESY_ERR_STATE;
  /* Selecting the mode before allocation preserves partially created workers. */
  s_ctx.peer_ops = peer_ops;
  s_ctx.peer_ctx = peer_ctx;
  s_ctx.owner_core = owner_core;
  s_ctx.mode_selected = true;
  if (peer_ops) {
    esy_err_t ret = peer_ops->prepare(peer_ctx);
    if (ret != ESY_OK) return ret;
  }
  s_ctx.initialized = true;
  return ESY_OK;
}

esy_err_t esy_core_lockout_core_init(void) {
  return esy_core_lockout_initialize(NULL, NULL);
}
