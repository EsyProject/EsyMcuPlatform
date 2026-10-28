/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/numerics/esy_numerics_quantize.h"

#include <math.h>
#include <stddef.h>

esy_err_t esy_numerics_quantize_f32_to_q15(const float *in_f32, int16_t *out_q15,
                                           const float *mean, const float *std,
                                           const float *scale, size_t length) {
  if (length == 0u) return ESY_OK;
  if ((in_f32 == NULL) || (out_q15 == NULL) || (mean == NULL) ||
      (std == NULL) || (scale == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!isfinite(in_f32[i]) || !isfinite(mean[i]) || !isfinite(std[i]) ||
        !isfinite(scale[i]) || std[i] == 0.0f) {
      return ESY_ERR_INVALID_ARG;
    }
  }
  for (size_t i = 0; i < length; ++i) {
    /* Double intermediates cover the full finite float input range, including
     * subtraction/division overflow that would occur in float arithmetic. */
    const double q = ((double)in_f32[i] - mean[i]) / std[i] * scale[i];
    if (q >= INT16_MAX) {
      out_q15[i] = INT16_MAX;
    } else if (q <= INT16_MIN) {
      out_q15[i] = INT16_MIN;
    } else {
      out_q15[i] = (int16_t)(q >= 0.0 ? q + 0.5 : q - 0.5);
    }
  }
  return ESY_OK;
}
