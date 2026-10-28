/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal_backend/esy_hb_spi.h"

/* This fixture provides custom SPI ops without native resource ownership. The
 * real wrapper retains a reference to its native identity query, but must not
 * call it for custom ops. Reject any such routing without touching the output;
 * the consumer's successful transfers prove this path stays unused. */
esy_err_t esy_hb_spi_get_phys_index(const esy_spi_t *handle,
                                   uint8_t *out_index) {
  if (handle == NULL || out_index == NULL) return ESY_ERR_INVALID_ARG;
  return ESY_ERR_STATE;
}
