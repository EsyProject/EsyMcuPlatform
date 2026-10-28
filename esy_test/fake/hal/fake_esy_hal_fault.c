/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_fault.h"
#include <stddef.h>

static fake_esy_hal_fault_state_t s_ctx;

void fake_esy_hal_fault_reset(void) {
  s_ctx = (fake_esy_hal_fault_state_t){.init_result = ESY_OK};
}

fake_esy_hal_fault_state_t *fake_esy_hal_fault_get_state(void) { return &s_ctx; }

esy_err_t esy_hal_fault_init(const esy_fault_config_t *config) {
  ++s_ctx.init_count;
  if (config == NULL || config->capture == NULL ||
      config->stack_snapshot_bytes > ESY_FAULT_STACK_MAX_BYTES) return ESY_ERR_INVALID_ARG;
  if (s_ctx.init_result != ESY_OK) return s_ctx.init_result;
  if (s_ctx.config.capture != NULL) return ESY_ERR_STATE;
  s_ctx.config = *config;
  return ESY_OK;
}

void esy_hal_fault_raise(esy_fault_reason_t reason, uint32_t detail0,
                         uint32_t detail1) {
  ++s_ctx.raise_count;
  s_ctx.reason = reason;
  s_ctx.detail0 = detail0;
  s_ctx.detail1 = detail1;
  /* A normal host test must use the copied capture callback to inject a
   * snapshot. Accidentally executing a terminal API must fail immediately. */
  __builtin_trap();
}
