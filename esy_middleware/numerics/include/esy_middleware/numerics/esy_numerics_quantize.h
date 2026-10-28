/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_MIDDLEWARE_NUMERICS_ESY_NUMERICS_QUANTIZE_H
#define ESY_MIDDLEWARE_NUMERICS_ESY_NUMERICS_QUANTIZE_H

/**
 * @file esy_numerics_quantize.h
 * @brief Normalization and signed 16-bit quantization helpers.
 */

#include <stddef.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Quantize a float32 array into Q15 fixed-point format.
 *
 * Each output value is computed as `(in_f32[i] - mean[i]) / std[i] * scale[i]`,
 * rounded to the nearest integer (halfway cases away from zero), and clamped to the int16_t range.
 *
 * All array elements must be finite and every std element must be nonzero.
 * Negative std/scale values are accepted. Finite inputs are evaluated with double
 * intermediates before saturation, including values outside the int range.
 * Invalid input preserves the entire output array. Zero length accepts NULL.
 *
 * Synchronous, task-context only. The caller owns all arrays and keeps inputs
 * unchanged until return. Input and output arrays must not overlap. Nothing is
 * retained, no initialization/cleanup is required, and independent arrays may
 * be processed concurrently. Shared mutable arrays require external serialization.
 *
 * This helper is only linked when the consuming project enables
 * `ENABLE_ESY_NUMERICS_QUANTIZE`.
 *
 * @param[in] in_f32 Input float32 array. Must not be NULL when @p length > 0.
 * @param[out] out_q15 Output Q15 array. Must not be NULL when @p length > 0.
 * @param[in] mean Mean array. Must not be NULL when @p length > 0.
 * @param[in] std Standard deviation array. Must not be NULL when @p length > 0.
 * @param[in] scale Scaling factor array. Must not be NULL when @p length > 0.
 * @param[in] length Number of elements.
 *
 * @retval ESY_OK Quantization completed successfully.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer, nonfinite element or zero
 * standard deviation.
 */
esy_err_t esy_numerics_quantize_f32_to_q15(const float *in_f32, int16_t *out_q15,
                                           const float *mean, const float *std,
                                           const float *scale, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* ESY_MIDDLEWARE_NUMERICS_ESY_NUMERICS_QUANTIZE_H */
