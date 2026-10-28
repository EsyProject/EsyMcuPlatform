/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_CORE_LOCKOUT_CORE_INTERNAL_H
#define ESY_CORE_LOCKOUT_CORE_INTERNAL_H
#include "esy_middleware/core_lockout/esy_core_lockout_core.h"

/* Immutable operations from the OS adapter; the core has no OS resource types. */
typedef struct {
  esy_err_t (*prepare)(void *ctx);
  esy_err_t (*lock)(void *ctx, uint32_t *other_core);
  esy_err_t (*release)(void *ctx, uint32_t other_core);
} esy_core_lockout_peer_ops_t;

typedef struct {
  bool initialized;
  bool executing;
  bool mode_selected;
  bool guard_release_failed;
  uint8_t owner_core;
  const esy_activity_guard_t *activity_guard;
  const esy_core_lockout_peer_ops_t *peer_ops;
  void *peer_ctx;
} esy_core_lockout_core_ctx_t;

/* Production assembly entry shared with the OS adapter, not a test seam.
 * Selecting the mode fixes it even if peer preparation fails. */
esy_err_t esy_core_lockout_initialize(const esy_core_lockout_peer_ops_t *peer_ops,
    void *peer_ctx);

/** @brief Execute with explicit state and injected peer coordination.
 * @param ctx Ready state, externally serialized against every other user.
 * @param operation Synchronous operation; provider owns its RAM/XIP safety.
 * @param arg Borrowed operation argument; valid until the call returns.
 * @return OK or admission/coordination error; release failure latches STATE.
 */
ESY_INTERNAL esy_err_t esy_core_lockout_core_execute(esy_core_lockout_core_ctx_t *ctx,
    esy_core_lockout_operation_t operation, void *arg);
#endif
