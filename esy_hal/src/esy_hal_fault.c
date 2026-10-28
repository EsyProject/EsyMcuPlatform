/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_fault.h"
#include "esy_hal_backend/esy_hb_fault.h"
#include <stddef.h>

esy_err_t esy_hal_fault_init(const esy_fault_config_t *config) {
  if (config == NULL || config->capture == NULL ||
      config->stack_snapshot_bytes > ESY_FAULT_STACK_MAX_BYTES) {
    return ESY_ERR_INVALID_ARG;
  }
  return esy_hb_fault_init(config);
}

#if defined(__arm__) && defined(__GNUC__)
__attribute__((naked))
void esy_hal_fault_raise(esy_fault_reason_t reason __attribute__((unused)),
                         uint32_t detail0 __attribute__((unused)),
                         uint32_t detail1 __attribute__((unused))) {
  __asm volatile("b esy_hb_fault_raise\n");
}
#else
void esy_hal_fault_raise(esy_fault_reason_t reason, uint32_t detail0,
                         uint32_t detail1) {
  esy_hb_fault_raise(reason, detail0, detail1);
}
#endif
