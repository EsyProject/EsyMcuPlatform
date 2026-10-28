/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_POLY_FIT_H
#define ESY_POLY_FIT_H

#include <stdint.h>

#include "esy_middleware/numerics/esy_numerics_config.h"

#include "esy_common/defs/base/esy_common_defs.h"  /* esy_err_t and ESY_* error codes */
#include "esy_fit.h"                     /* generic linear regression core */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_poly_fit.h
 * @brief Polynomial regression with input scaling and model selection.
 *
 * ## Overview
 * This module implements polynomial regression on top of the generic
 * linear regression kernel (@ref esy_fit).
 *
 * Key features:
 * - Input scaling: x is transformed into a scaled domain
 *     xs = (x - mean) * scale
 *   which improves numerical conditioning for higher-order polynomials.
 * - Training is performed in the scaled domain.
 * - After training, coefficients can be expanded back to the original
 *   x-domain for fast runtime evaluation using Horner's method.
 * - Degree selection via LOOCV (leave-one-out cross validation).
 *
 * The module is designed for embedded and constrained environments:
 * - No dynamic allocation.
 * - Fixed compile-time limits.
 * - Public fitting APIs validate all pointer and dimension inputs.
 *
 * Calls are synchronous and task-context only. Inputs must be finite and remain
 * unchanged until return. Arrays, outputs and models must not overlap. Callers
 * own all storage; no pointer is retained. Independent storage may be used
 * concurrently; shared model/buffer access requires external serialization.
 * Output models need no initialization and may be reused. There are no cleanup
 * resources. Failure preserves A_out and model_out; rmse_out is reset to zero.
 * Only coefficients through degree are valid after success.
 */

/* -------------------------------------------------------------------------- */
/* Data structures                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Polynomial model trained in the scaled input domain.
 *
 * The model stores coefficients for:
 *   y(xs) = w[0] + w[1]*xs + ... + w[d]*xs^d
 *
 * where:
 *   xs = (x - mean) * scale
 */
typedef struct {
  int   degree;                    /**< Polynomial degree d. */
  float mean;                      /**< Input mean used for scaling. */
  float scale;                     /**< Input scale (>0), typically normalizes xs to ~[-1, 1]. */
  float w[ESY_FIT_MAX_DEG + 1];     /**< Scaled-domain coefficients w[0..d]. */
  float rmse_train;                /**< Training RMSE evaluated in the scaled/feature domain. */
} esy_poly_model_t;

/* -------------------------------------------------------------------------- */
/* API                                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fit a polynomial model and expand coefficients to the original x-domain.
 *
 * The fitting process is:
 * 1. Compute scaling parameters (mean, scale).
 * 2. Fit the polynomial in the scaled domain using ridge regression.
 * 3. Expand the scaled-domain coefficients into original-domain coefficients A.
 *
 * The resulting coefficients A allow fast runtime evaluation directly on x:
 *   y = A[0] + A[1]*x + ... + A[d]*x^d
 *
 * @param[in]  X         Input samples x (length n).
 * @param[in]  Y         Target samples y (length n).
 * @param[in]  n         Number of samples (1..ESY_FIT_MAX_N, required n >= degree+1, even with ridge).
 * @param[in]  degree    Polynomial degree (0..ESY_FIT_MAX_DEG).
 * @param[in]  lambda    Ridge coefficient (>= 0, 0 = ordinary least squares).
 * @param[out] A_out     Output coefficients in original x-domain (length degree+1).
 *                       A[0] is the constant term.
 * @param[out] model_out Optional output of the scaled-domain model (may be NULL).
 * @param[out] rmse_out  Optional output of training RMSE (may be NULL).
 *                       When non-NULL, it is set to 0.0f before validation.
 * @return esy_err_t     ESY_OK on success; ESY_ERR_INVALID_ARG for invalid
 *                       inputs (including nonfinite samples/lambda);
 *                       ESY_ERR_FAIL for solve, scaling or coefficient overflow.
 */
esy_err_t esy_poly_fit(const float *X, const float *Y, int n,
                       int degree, float lambda,
                       float *A_out,
                       esy_poly_model_t *model_out,
                       float *rmse_out);

/**
 * @brief Select the optimal polynomial degree using LOOCV.
 *
 * This function maps the polynomial degree `deg` to the feature dimension
 * `m = deg + 1` and reuses explicit leave-one-out fits from the generic core.
 * One scaling transform, computed from the complete X array, is fixed for all
 * folds. This validates that fixed feature map, not a pipeline that relearns
 * normalization per fold (which changes the ridge penalty). Failed candidates
 * are skipped. If no candidate is valid, returns ESY_ERR_FAIL; invalid bounds or
 * nonfinite samples/lambda return ESY_ERR_INVALID_ARG.
 *
 * In case of ties (|Δ| <= 1e-6), the lower degree is preferred.
 *
 * @param[in] X        Input samples x (length n).
 * @param[in] Y        Target samples y (length n).
 * @param[in] n        Number of samples (2..ESY_FIT_MAX_N).
 * @param[in] deg_min  Minimum degree to consider (>= 0).
 * @param[in] deg_max  Maximum degree to consider (<= ESY_FIT_MAX_DEG and >= deg_min).
 * @param[in] lambda   Ridge coefficient (>= 0).
 * @return int         Best degree (>= 0) on success; negative ESY_* error code on failure.
 */
int esy_poly_select_degree_loocv(const float *X, const float *Y, int n,
                                 int deg_min, int deg_max, float lambda);

/* -------------------------------------------------------------------------- */
/* Prediction helpers                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Predict using original-domain coefficients.
 *
 * Evaluates the polynomial using Horner's method:
 *   y = (((A[d] * x + A[d-1]) * x + ...) * x + A[0])
 *
 * @param[in] A       Original-domain coefficients (length degree+1).
 * @param[in] degree  Polynomial degree.
 * @param[in] x       Input value.
 * @return Predicted y value.
 *
 * @pre @p A is not NULL and @p degree is in range for the coefficient buffer.
 * Task-context only; the coefficient buffer must not be modified concurrently.
 * This unchecked helper does not report overflow at extrapolated x values.
 */
static inline float esy_poly_predict_raw(const float *A, int degree, float x) {
  float y = A[degree];
  for (int j = degree - 1; j >= 0; --j) {
    y = y * x + A[j];
  }
  return y;
}

/**
 * @brief Predict using the scaled-domain model.
 *
 * This evaluates the polynomial in the scaled domain using the stored model:
 *   xs = (x - mean) * scale
 *   y  = w[0] + w[1]*xs + ... + w[d]*xs^d
 *
 * @param[in] m  Scaled-domain polynomial model.
 * @param[in] x  Input value in the original domain.
 * @return Predicted y value.
 *
 * @pre @p m is not NULL and `m->degree` is valid for `m->w`.
 * Task-context only; the model must not be modified concurrently.
 * This unchecked helper does not report overflow at extrapolated x values.
 */
static inline float esy_poly_predict_scaled(const esy_poly_model_t *m, float x) {
  const float xs = (float)(((double)x - m->mean) * m->scale);
  float y = m->w[m->degree];
  for (int j = m->degree - 1; j >= 0; --j) {
    y = y * xs + m->w[j];
  }
  return y;
}

#ifdef __cplusplus
}
#endif

#endif /* ESY_POLY_FIT_H */
