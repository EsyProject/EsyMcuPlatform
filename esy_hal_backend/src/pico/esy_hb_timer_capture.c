/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_timer_capture.h"

/* RP2040/RP2350 have no native timer input-capture provider in this backend.
 * Reject before HAL board setup; GPIO interrupts remain a separate API. */
esy_err_t esy_hb_timer_capture_claim(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  if (cap == NULL || cfg == NULL) return ESY_ERR_INVALID_ARG;
  return ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_timer_capture_init(
    esy_timer_capture_t *cap, const esy_timer_capture_config_t *cfg) {
  return esy_hb_timer_capture_claim(cap, cfg);
}

esy_err_t esy_hb_timer_capture_suspend(esy_timer_capture_t *cap) {
  return cap == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_timer_capture_deinit(esy_timer_capture_t *cap) {
  if (cap == NULL) return ESY_ERR_INVALID_ARG;
  return cap->ctx == NULL ? ESY_OK : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_timer_capture_get_value(
    const esy_timer_capture_t *cap, uint32_t *value) {
  return cap == NULL || value == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_timer_capture_get_interval(
    const esy_timer_capture_t *cap, uint32_t *interval) {
  return cap == NULL || interval == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_timer_capture_get_phys_index(
    const esy_timer_capture_t *cap, uint8_t *index) {
  return cap == NULL || index == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

esy_err_t esy_hb_timer_capture_get_caps(
    const esy_timer_capture_t *cap, uint32_t *caps) {
  return cap == NULL || caps == NULL ? ESY_ERR_INVALID_ARG : ESY_ERR_NOT_SUPPORTED;
}

void esy_hb_timer_capture_handle_irq(uint32_t index) { (void)index; }
